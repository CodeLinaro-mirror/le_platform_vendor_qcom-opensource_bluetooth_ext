/******************************************************************************
 *  Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *  Not a Contribution.
 *
 *  Copyright (C) 2009-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/


/*****************************************************************************
 *
 *  Filename:      btif_avk.c
 *
 *  Description:   Bluedroid AVK implementation
 *
 *****************************************************************************/

#include <assert.h>
#include <string.h>

#include <hardware/bluetooth.h>
#include "hardware/bt_av.h"
#include "osi/include/allocator.h"
#include <cutils/properties.h>

#define LOG_TAG "bt_btif_avk_split"

#include "btif_avk.h"
#include "btif_util.h"
#include "btif_profile_queue.h"
#include "bta_api.h"
#include "bta_av_api.h"
#include "btif_avk_media.h"
#include "bta_avk_api.h"
#include "bta_avk_co.h"
#include "btu.h"
#include "bt_utils.h"
#include "hardware/bt_av_vendor.h"
#include "osi/include/fixed_queue.h"
#include "osi/include/list.h"

/*****************************************************************************
**  Constants & Macros
******************************************************************************/
#define BTIF_AVK_SERVICE_NAME "Advanced Audio Sink"

/* Number of BTIF-AV control blocks */
/* Now supports Two AV connections. */
#define BTIF_AVK_NUM_CB       2
#define HANDLE_TO_INDEX(x) ((x & BTA_AVK_HNDL_MSK) - 1)

typedef enum {
    BTIF_AVK_STATE_IDLE = 0x0,
    BTIF_AVK_STATE_OPENING,
    BTIF_AVK_STATE_OPENED,
    BTIF_AVK_STATE_STARTING,
    BTIF_AVK_STATE_STARTED,
    BTIF_AVK_STATE_SUSPENDING,
    BTIF_AVK_STATE_CLOSING,
    BTIF_AVK_STATE_HDL_VSC
} btif_avk_state_t;

typedef enum {
    BTIF_AVK_VSC_STOP = 0x0, // initial state, VSC not started
    BTIF_AVK_VSC_STARTING,// VSC_Start command is sent, response awated
    BTIF_AVK_VSC_START_FAILED,// VSC Start Command Failed
    BTIF_AVK_VSC_STARTED, // VSC start successful
    BTIF_AVK_VSC_STOPPING // VSC_Stop command is sent, response awated
} btif_avk_vsc_command_state_t;

/* Should not need dedicated suspend state as actual actions are no
   different than open state. Suspend flags are needed however to prevent
   media task from trying to restart stream during remote suspend or while
   we are in the process of a local suspend */

#define BTIF_AVK_FLAG_LOCAL_SUSPEND_PENDING 0x1
#define BTIF_AVK_FLAG_REMOTE_SUSPEND        0x2
#define BTIF_AVK_FLAG_PENDING_START         0x4
#define BTIF_AVK_FLAG_PENDING_STOP          0x8
/* Host role defenitions */
#define HOST_ROLE_MASTER                   0x00
#define HOST_ROLE_SLAVE                    0x01
#define HOST_ROLE_UNKNOWN                  0xff

#define MAX_A2DP_SINK_DATA_QUEUE_SZ         20

#define DEFAULT_RENDERING_DELAY            140      //define a fix rendering delay
#define APTX_RENDERING_DELAY               200
#define AAC_RENDERING_DELAY                180

#define BTIF_AVK_PENDING_RESPONSE_TIMEOUT     7000 // 7 seconds
#define APTX_AD_RENDERING_DELAY            150
/*****************************************************************************
**  Local type definitions
******************************************************************************/

// strucutre to hold remembered messages
#define MSG_QUE_LEN 20
typedef struct {
    uint8_t msg;
    uint8_t state;
    uint8_t*  p_data;
}btif_avk_msg_t;

typedef struct
{
    tBTA_AVK_HNDL bta_handle;
    bt_bdaddr_t peer_bda;
    btif_sm_handle_t sm_handle;
    uint8_t flags;
    tBTA_AVK_EDR edr;
    uint8_t edr_3mbps;
    btif_sm_state_t state;
    bool is_slave;
    uint16_t sink_codec_type;
    bool avdt_sync;    //for AVDT1.3 delay reporting
    uint32_t delay;    // value used for delay reporting in case of split
    uint32_t qahw_delay;
    uint8_t prev_state;
    uint8_t vsc_command_status;
    btif_avk_msg_t rem_msg[MSG_QUE_LEN];
} btif_avk_cb_t;

pthread_mutex_t split_sink_codec_q_lock;

typedef struct
{
    bt_bdaddr_t *target_bda;
    uint16_t uuid;
} btif_avk_connect_req_t;

typedef struct
{
    int sample_rate;
    int channel_count;
    RawAddress peer_bd;
    uint8_t codec_type;
    btav_codec_config_t   codec_info;
} btif_avk_config_req_t;


/*****************************************************************************
**  Static variables
******************************************************************************/
static btav_sink_callbacks_t *bt_avk_callbacks = NULL;
static btav_sink_vendor_callbacks_t *bt_av_sink_vendor_callbacks = NULL;
static btif_avk_cb_t btif_avk_cb[BTIF_AVK_NUM_CB];
static alarm_t *avk_open_on_rc_timer = NULL;
static uint8_t btif_max_avk_clients = 1;
static alarm_t *avk_pending_start_timer = NULL;
static alarm_t *avk_pending_suspend_timer = NULL;

static uint16_t enable_delay_reporting = 0; // by default disable it
static uint16_t enable_avrc_browsing = 0;
static RawAddress streaming_bda;
extern tBTA_AVK_CO_CODEC_CAP_LIST *p_bta_avk_codec_pri_list;
extern tBTA_AVK_CO_CODEC_CAP_LIST bta_avk_supp_codec_cap[BTIF_SV_AVK_AA_SEP_INDEX];
extern uint8_t bta_avk_num_codec_configs;

/* both interface and media task needs to be ready to alloc incoming request */
#define CHECK_BTAVK_INIT() if (((bt_avk_callbacks == NULL)) \
        || (btif_avk_cb[0].sm_handle == NULL))\
{\
     BTIF_TRACE_WARNING("%s: BTAVK not initialized", __FUNCTION__);\
     return BT_STATUS_NOT_READY;\
}\
else\
{\
     BTIF_TRACE_EVENT("%s", __FUNCTION__);\
}

/* Helper macro to avoid code duplication in the state machine handlers */
#define CHECK_AVK_RC_EVENT(e, d) \
    case BTA_AVK_RC_OPEN_EVT: \
    case BTA_AVK_RC_CLOSE_EVT: \
    case BTA_AVK_REMOTE_CMD_EVT: \
    case BTA_AVK_VENDOR_CMD_EVT: \
    case BTA_AVK_META_MSG_EVT: \
    case BTA_AVK_BROWSE_MSG_EVT: \
    case BTA_AVK_RC_BROWSE_OPEN_EVT: \
    case BTA_AVK_RC_FEAT_EVT: \
    case BTA_AVK_REMOTE_RSP_EVT: \
    { \
         btif_avk_rc_handler(e, d);\
    }break; \


static bool btif_avk_split_state_idle_handler(btif_sm_event_t event, void *data, int index);
static bool btif_avk_split_state_opening_handler(btif_sm_event_t event, void *data, int index);
static bool btif_avk_split_state_opened_handler(btif_sm_event_t event, void *data, int index);
//state to handle incoming start request.
static bool btif_avk_split_state_starting_handler(btif_sm_event_t event, void *data,int index);
// suspend handler: handle incoming suspend request.
static bool btif_avk_split_state_suspending_handler(btif_sm_event_t event, void *data,int index);
static bool btif_avk_split_state_started_handler(btif_sm_event_t event, void *data,int index);
static bool btif_avk_split_state_closing_handler(btif_sm_event_t event, void *data,int index);
//state to handle VSC commands
static bool btif_avk_split_state_vsc_handler(btif_sm_event_t event, void *data,int index);

static bool btif_avk_is_device_connected_connecting(int idx);
static uint8_t btif_avk_idx_by_bdaddr( RawAddress * bd_addr);
static int btif_avk_get_valid_idx_for_rc_events(RawAddress bd_addr, int rc_handle);
static int btif_get_conn_state_of_device(RawAddress address);
static bt_status_t connect_int(bt_bdaddr_t *bd_addr, uint16_t uuid);
static void btif_avk_event_free_data(btif_sm_event_t event, void *p_data);
static bool btif_avk_is_playing_on_other_idx(int current_index);
static bt_status_t init_sink_vendor(btav_sink_vendor_callbacks_t* callbacks, int max, int a2dp_multicast_state, uint8_t streaming_prarm);
bool btif_avk_split_is_connected(void);
void btif_avk_pending_start_timeout_handler(void* data);
void btif_avk_pending_suspend_timeout_handler(void* data);

static const btif_sm_handler_t btif_avk_state_handlers[] =
{
    btif_avk_split_state_idle_handler,
    btif_avk_split_state_opening_handler,
    btif_avk_split_state_opened_handler,
    btif_avk_split_state_starting_handler,
    btif_avk_split_state_started_handler,
    btif_avk_split_state_suspending_handler,
    btif_avk_split_state_closing_handler,
    btif_avk_split_state_vsc_handler
};

// Anubhav
/*************************************************************************
** Extern functions
*************************************************************************/
extern void btif_avk_rc_handler(tBTA_AVK_EVT event, tBTA_AVK *p_data);
extern bool btif_avk_rc_get_connected_peer(RawAddress* peer_addr);
extern uint8_t btif_avk_rc_get_connected_peer_handle(const RawAddress& peer_addr);
extern uint16_t btif_dm_get_br_edr_links();
extern uint16_t btif_dm_get_le_links();
extern void btif_avk_rc_ctrl_send_pause(bt_bdaddr_t *bd_addr);
extern void btif_avk_rc_ctrl_send_play(bt_bdaddr_t *bd_addr);

extern void alarm_set_on_mloop(alarm_t *alarm, period_ms_t interval_ms, alarm_callback_t cb, void *data);
/*****************************************************************************
** Local helper functions
******************************************************************************/
bool btif_avk_split_is_device_connected(RawAddress address);

static const char *dump_avk_sm_state_name(btif_avk_state_t state)
{
    switch (state)
    {
        CASE_RETURN_STR(BTIF_AVK_STATE_IDLE)
        CASE_RETURN_STR(BTIF_AVK_STATE_OPENING)
        CASE_RETURN_STR(BTIF_AVK_STATE_OPENED)
        CASE_RETURN_STR(BTIF_AVK_STATE_STARTING)
        CASE_RETURN_STR(BTIF_AVK_STATE_STARTED)
        CASE_RETURN_STR(BTIF_AVK_STATE_SUSPENDING)
        CASE_RETURN_STR(BTIF_AVK_STATE_CLOSING)
        CASE_RETURN_STR(BTIF_AVK_STATE_HDL_VSC)
        default: return "UNKNOWN_STATE";
    }
}

static const char *dump_avk_sm_event_name(btif_avk_sm_event_t event)
{
    switch((int)event)
    {
        CASE_RETURN_STR(BTA_AVK_ENABLE_EVT)
        CASE_RETURN_STR(BTA_AVK_REGISTER_EVT)
        CASE_RETURN_STR(BTA_AVK_OPEN_EVT)
        CASE_RETURN_STR(BTA_AVK_CLOSE_EVT)
        CASE_RETURN_STR(BTA_AVK_START_EVT)
        CASE_RETURN_STR(BTA_AVK_STOP_EVT)
        CASE_RETURN_STR(BTA_AVK_PROTECT_REQ_EVT)
        CASE_RETURN_STR(BTA_AVK_PROTECT_RSP_EVT)
        CASE_RETURN_STR(BTA_AVK_RC_OPEN_EVT)
        CASE_RETURN_STR(BTA_AVK_RC_CLOSE_EVT)
        CASE_RETURN_STR(BTA_AVK_REMOTE_CMD_EVT)
        CASE_RETURN_STR(BTA_AVK_REMOTE_RSP_EVT)
        CASE_RETURN_STR(BTA_AVK_VENDOR_CMD_EVT)
        CASE_RETURN_STR(BTA_AVK_VENDOR_RSP_EVT)
        CASE_RETURN_STR(BTA_AVK_RECONFIG_EVT)
        CASE_RETURN_STR(BTA_AVK_SUSPEND_EVT)
        CASE_RETURN_STR(BTA_AVK_PENDING_EVT)
        CASE_RETURN_STR(BTA_AVK_META_MSG_EVT)
        CASE_RETURN_STR(BTA_AVK_REJECT_EVT)
        CASE_RETURN_STR(BTA_AVK_RC_FEAT_EVT)
        CASE_RETURN_STR(BTIF_SM_ENTER_EVT)
        CASE_RETURN_STR(BTIF_SM_EXIT_EVT)
        CASE_RETURN_STR(BTIF_AVK_CONNECT_REQ_EVT)
        CASE_RETURN_STR(BTIF_AVK_DISCONNECT_REQ_EVT)
        CASE_RETURN_STR(BTIF_AVK_START_STREAM_REQ_EVT)
        CASE_RETURN_STR(BTIF_AVK_SINK_START_STREAM_REQ_EVT)
        CASE_RETURN_STR(BTIF_AVK_STOP_STREAM_REQ_EVT)
        CASE_RETURN_STR(BTIF_AVK_SUSPEND_STREAM_REQ_EVT)
        CASE_RETURN_STR(BTIF_AVK_SINK_CONFIG_REQ_EVT)
        CASE_RETURN_STR(BTIF_AVK_SPLIT_SINK_START_IND_RSP)
        CASE_RETURN_STR(BTIF_AVK_SPLIT_SINK_SUSPEND_IND_RSP)
        CASE_RETURN_STR(BTIF_AVK_SPLIT_SINK_START_CAPTURE)
        CASE_RETURN_STR(BTIF_AVK_SPLIT_SINK_STOP_CAPTURE)
        CASE_RETURN_STR(BTIF_AVK_SPLIT_SINK_CHECK_A2DP_READY)
        CASE_RETURN_STR(BTIF_AVK_SPLIT_SINK_SETUP_COMPLETE)
        CASE_RETURN_STR(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_START)
        CASE_RETURN_STR(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP)
        CASE_RETURN_STR(BTIF_AVK_SPLIT_SINK_VSC_START_COMPLETE_EVT)
        CASE_RETURN_STR(BTIF_AVK_SPLIT_SINK_VSC_STOP_COMPLETE_EVT)
        CASE_RETURN_STR(BTA_AVK_OFFLOAD_START_RSP_EVT)
        CASE_RETURN_STR(BTA_AVK_OFFLOAD_STOP_RSP_EVT)
        CASE_RETURN_STR(BTIF_AVK_SPLIT_PENDING_START_ALARM_TIMEOUT)
        CASE_RETURN_STR(BTIF_AVK_SPLIT_PENDING_SUSPEND_ALARM_TIMEOUT)
        CASE_RETURN_STR(BTA_AVK_MTU_CONFIG_EVT)
        default: return "UNKNOWN_EVENT";
   }
}



/****************************************************************************
**  Local helper functions
*****************************************************************************/
/*******************************************************************************
**
** Function         btif_initiate_avk_open_timer_timeout
**
** Description      Timer to trigger AV open if the remote headset establishes
**                  RC connection w/o AV connection. The timer is needed to IOP
**                  with headsets that do establish AV after RC connection.
**
** Returns          void
**
*******************************************************************************/
static void btif_initiate_avk_open_timer_timeout(UNUSED_ATTR void *data)
{
    RawAddress peer_addr;

    /* is there at least one RC connection - There should be */
    /*We have Two Connections.*/
    if (btif_avk_rc_get_connected_peer(&peer_addr))
    {
        /*Check if this peer_addr is same as currently connected AV*/
        if (btif_get_conn_state_of_device(peer_addr) == BTIF_AVK_STATE_OPENED)
        {
            BTIF_TRACE_DEBUG("AV is already connected");
        }
        else
        {
            uint8_t rc_handle;
            int index;
            /* Multicast: Check if AV slot is available for connection
             * If not available, AV got connected to different devices.
             * Disconnect this RC connection without AV connection.
             */
            rc_handle = btif_avk_rc_get_connected_peer_handle(peer_addr);
            index = btif_avk_get_valid_idx_for_rc_events(peer_addr, rc_handle);
            if(index >= btif_max_avk_clients)
            {
                BTIF_TRACE_ERROR("%s No slot free for AV connection, back off",
                            __FUNCTION__);
                return;
            }
            BTIF_TRACE_DEBUG("%s Issuing connect to the remote RC peer", __FUNCTION__);
            if(bt_avk_callbacks != NULL)
                btif_queue_connect(UUID_SERVCLASS_AUDIO_SINK, peer_addr,
                        connect_int, btif_max_avk_clients);
        }
    }
    else
    {
        BTIF_TRACE_ERROR("%s No connected RC peers", __FUNCTION__);
    }

}

/*****************************************************************************
**  Static functions
******************************************************************************/

static void btif_report_connection_state(btav_connection_state_t state, bt_bdaddr_t *bd_addr)
{
    if (bt_avk_callbacks != NULL) {
        HAL_CBACK(bt_avk_callbacks, connection_state_cb, *bd_addr, state);
    }
}

static void btif_report_audio_state(btav_audio_state_t state, bt_bdaddr_t *bd_addr)
{
    if (bt_avk_callbacks != NULL) {
        HAL_CBACK(bt_avk_callbacks, audio_state_cb, *bd_addr, state);
    }
}

/*******************************************************************************
**
** Function         memorize_msg
**
** Description      Pumps messages to a stack
**
** Returns          void
**
*******************************************************************************/

static void memorize_msg(uint8_t index, uint8_t event, btif_avk_state_t state, void *p_data)
{
    BTIF_TRACE_DEBUG(" %s  event = %s, state = %d", __FUNCTION__,
        dump_avk_sm_event_name((btif_avk_sm_event_t)event), state);
    uint8_t i = 0;
    uint8_t len = 0;
    for (i = 0; i < MSG_QUE_LEN; i++) {
        if (btif_avk_cb[index].rem_msg[i].msg == 0) {
            btif_avk_cb[index].rem_msg[i].msg = event;
            btif_avk_cb[index].rem_msg[i].state = state;
            if (p_data != NULL) {
                if (event == BTIF_AVK_CONNECT_REQ_EVT) {
                    len = sizeof(btif_avk_connect_req_t);
                } else {
                    len = sizeof(tBTA_AVK);
                }
                btif_avk_cb[index].rem_msg[i].p_data = (uint8_t *) osi_malloc(len);
                memcpy(btif_avk_cb[index].rem_msg[i].p_data, p_data, len);
            } else {
                btif_avk_cb[index].rem_msg[i].p_data = NULL;
            }
            BTIF_TRACE_DEBUG(" %s adding event %d @ %d", __FUNCTION__,event,i);
            return;
        }
    }
    BTIF_TRACE_DEBUG(" %s, MSG List Full index = %d", __FUNCTION__, i);
}

/*******************************************************************************
**
** Function         handle_memorized_msgs
**
** Description      Retrevies messages stored on stack
**
** Returns          void
**
*******************************************************************************/

static void handle_memorized_msgs(uint8_t index) {
    // first figure out start and end of msg que
    int i = 0;
    uint8_t end_index = 0;
    for (i = 0; i< MSG_QUE_LEN; i++) {
        if (btif_avk_cb[index].rem_msg[i].msg == 0) {
            break;
        }
    }
    end_index = i;
    BTIF_TRACE_DEBUG(" %s end_index = %d", __FUNCTION__, end_index);
    if ( end_index == 0)// no messages in list.
        return;
    // under assumption that we have enough space to accomodate even if
    // all messages are re memorized as part of processing memorized msgs.
    for (i = 0; i < end_index; i++) {
         btif_sm_dispatch(btif_avk_cb[index].sm_handle, btif_avk_cb[index].rem_msg[i].msg,
                          btif_avk_cb[index].rem_msg[i].p_data);
    }
    // we have acted upon all rememberd messages, make everything 0 now.
    for(i = (end_index -1) ; i>= 0; i--) {
        btif_avk_cb[index].rem_msg[i].msg = 0;
        if(btif_avk_cb[index].rem_msg[i].p_data != NULL) {
            osi_free(btif_avk_cb[index].rem_msg[i].p_data);
            btif_avk_cb[index].rem_msg[i].p_data = NULL;
        }
    }

    BTIF_TRACE_DEBUG(" %s out of function", __FUNCTION__);
}

void btif_avk_reset_messages(uint8_t index) {
    BTIF_TRACE_DEBUG( " %s for index %d",__func__, index);
    uint8_t i = 0;
    for (i=0; i< MSG_QUE_LEN; i++) {
        btif_avk_cb[index].rem_msg[i].msg = 0;
        btif_avk_cb[index].rem_msg[i].state = 0;
        if(btif_avk_cb[index].rem_msg[i].p_data != NULL) {
            osi_free(btif_avk_cb[index].rem_msg[i].p_data);
            btif_avk_cb[index].rem_msg[i].p_data = NULL;
        }
    }

}
/*****************************************************************************
**
** Function     btif_avk_split_state_idle_handler
**
** Description  State managing disconnected AV link
**
** Returns      TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static bool btif_avk_split_state_idle_handler(btif_sm_event_t event, void *p_data, int index)
{
    BTIF_TRACE_IMP("%s event:%s flags %x on Index = %d", __FUNCTION__,
                     dump_avk_sm_event_name((btif_avk_sm_event_t)event), btif_avk_cb[index].flags, index);

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            /* clear the peer_bda */
            BTIF_TRACE_EVENT("IDLE state for index: %d ", index);
            btif_avk_cb[index].flags = 0;
            btif_avk_cb[index].edr_3mbps = 0;
            btif_avk_cb[index].edr = 0;
            btif_avk_cb[index].is_slave = FALSE;
            btif_avk_cb[index].sink_codec_type = 0xFF;
            btif_avk_cb[index].avdt_sync = FALSE;
            btif_avk_cb[index].delay = 0;
            btif_avk_cb[index].qahw_delay = 0;
            btif_avk_cb[index].vsc_command_status = BTIF_AVK_VSC_STOP;

            /* This API will be called twice at initialization
            ** Idle can be moved when device is disconnected too.
            ** Take care of other connected device here.*/
            if (!btif_avk_split_is_connected())
            {
                BTIF_TRACE_EVENT("reset A2dp states in IDLE ");
                btif_avk_split_a2dp_on_idle();
            }
            // reset all memorized messages list.
            btif_avk_reset_messages(index);
            if((!streaming_bda.IsEmpty()) && (streaming_bda == btif_avk_cb[index].peer_bda)) {
                //streaming bda is assigned to current device, but we move to opened state.
                // mark it empty
                streaming_bda = RawAddress::kEmpty;
            }
            break;

        case BTIF_SM_EXIT_EVT:
            btif_avk_cb[index].prev_state = BTIF_AVK_STATE_IDLE;
            break;

        case BTA_AVK_ENABLE_EVT:
            BTIF_TRACE_EVENT("AVK is enabled now for index: %d", index);
            break;

        case BTA_AVK_REGISTER_EVT:
            BTIF_TRACE_EVENT("The AV Handle:%d", ((tBTA_AVK*)p_data)->registr.hndl);
            btif_avk_cb[index].bta_handle = ((tBTA_AVK*)p_data)->registr.hndl;
            if (btif_max_avk_clients == index + 1) {
                if (bt_av_sink_vendor_callbacks != NULL) {
                    HAL_CBACK(bt_av_sink_vendor_callbacks, registration_vendor_cb, TRUE);
                }
            }
            btif_read_adapter_property(BT_PROPERTY_CLASS_OF_DEVICE);
            break;
        case BTIF_AVK_CONNECT_REQ_EVT:
            /* For outgoing connect stack and app are in sync.
            */
            memcpy(&btif_avk_cb[index].peer_bda, ((btif_avk_connect_req_t*)p_data)->target_bda,
                                                                        sizeof(bt_bdaddr_t));
            BTA_AvkOpen(btif_avk_cb[index].peer_bda, btif_avk_cb[index].bta_handle,
                        TRUE, BTA_SEC_NONE, ((btif_avk_connect_req_t*)p_data)->uuid);
            btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_OPENING);
            break;

        case BTA_AVK_PENDING_EVT:
        case BTA_AVK_RC_OPEN_EVT:
            /* IOP_FIX: Jabra 620 only does RC open without AV open whenever it connects. So
             * as per the AV WP, an AVRC connection cannot exist without an AV connection. Therefore,
             * we initiate an AV connection if an RC_OPEN_EVT is received when we are in AV_CLOSED state.
             * We initiate the AV connection after a small 3s timeout to avoid any collisions from the
             * headsets, as some headsets initiate the AVRC connection first and then
             * immediately initiate the AV connection
             *
             * TODO: We may need to do this only on an AVRCP Play. FixMe
             */
            /* Check if connection allowed with this device */
            /* In Dual A2dp case, this event can come for both the headsets.
             * Reject second connection request as we are already checking
             * for device priority for first device and we cannot queue
             * incoming connections requests.
             */
            if (event == BTA_AVK_RC_OPEN_EVT)
            {
                if (((tBTA_AVK*)p_data)->rc_open.status == BTA_AVK_SUCCESS)
                {
                    btif_avk_cb[index].peer_bda = ((tBTA_AVK*)p_data)->rc_open.peer_addr;
                }
                else
                {
                    return TRUE;
                }
            }
            else
            {
                btif_avk_cb[index].peer_bda = ((tBTA_AVK*)p_data)->pend.bd_addr;
            }

            // Only for AVDTP connection request move to opening state
            if (event == BTA_AVK_PENDING_EVT)
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_OPENING);

            if (bt_avk_callbacks != NULL)
            {
                if(event == BTA_AVK_PENDING_EVT)
                {
                    BTA_AvkOpen(btif_avk_cb[index].peer_bda, btif_avk_cb[index].bta_handle,
                       TRUE, BTA_SEC_NONE, UUID_SERVCLASS_AUDIO_SINK);
                }
                else if(event == BTA_AVK_RC_OPEN_EVT)
                {
                    alarm_set_on_mloop(avk_open_on_rc_timer,
                                       BTIF_TIMEOUT_AV_OPEN_ON_RC_SECS,
                                       btif_initiate_avk_open_timer_timeout, NULL);
                    btif_avk_rc_handler(event, (tBTA_AVK*)p_data);
                }
            }
            break;

        case BTIF_AVK_SINK_CONFIG_REQ_EVT:
        {
            btif_avk_config_req_t req;
            // copy to avoid alignment problems
            /* in this case, L2CAP connection is still up, but bt-app moved to disc state
               so lets move bt-app to connected state first */
            memcpy(&req, p_data, sizeof(req));
            btif_report_connection_state(BTAV_CONNECTION_STATE_CONNECTED, (bt_bdaddr_t *)&(req.peer_bd));
            BTIF_TRACE_WARNING("BTIF_AVK_SINK_CONFIG_REQ_EVT %d %d %s %d",
                    req.sample_rate, req.channel_count, btif_avk_cb[index].peer_bda.ToString().c_str(), req.codec_type);

            if (bt_av_sink_vendor_callbacks != NULL) {
                HAL_CBACK(bt_av_sink_vendor_callbacks, audio_codec_config_vendor_cb,
                        &(req.peer_bd), req.codec_type, req.codec_info);
            }
        } break;
        case BTIF_AVK_SPLIT_SINK_CHECK_A2DP_READY:
            BTIF_TRACE_DEBUG(" VSC command Status %d",btif_avk_cb[index].vsc_command_status);
            // in this state, we are not ready.
            btif_update_cmd_status(A2DP_AVK_CTRL_CMD_CHECK_READY,A2DP_CTRL_ACK_FAILURE);
            break;
        case BTA_AVK_OPEN_EVT:
        {
            /* We get this event in Idle State if Signaling
             * channel is not closed, only Streaming channel was
             * closed earlier, and now only stream setup process is
             * initiated.
             */
            tBTA_AVK *p_bta_data = (tBTA_AVK*)p_data;
            btav_connection_state_t state;
            BTIF_TRACE_DEBUG("status:%d, edr 0x%x",p_bta_data->open.status,
                               p_bta_data->open.edr);

            if (p_bta_data->open.status == BTA_AVK_SUCCESS)
            {
                 state = BTAV_CONNECTION_STATE_CONNECTED;
                 btif_avk_cb[index].edr = p_bta_data->open.edr;
                 if (p_bta_data->open.role == HOST_ROLE_SLAVE)
                 {
                    btif_avk_cb[index].is_slave = TRUE;
                 }

                 if (p_bta_data->open.edr & BTA_AVK_EDR_3MBPS)
                 {
                     BTIF_TRACE_DEBUG("remote supports 3 mbps");
                     btif_avk_cb[index].edr_3mbps = TRUE;
                 }
                btif_avk_cb[index].peer_bda = ((tBTA_AVK*)p_data)->open.bd_addr;
            }
            else
            {
                BTIF_TRACE_WARNING("BTA_AVK_OPEN_EVT::FAILED status: %d",
                                     p_bta_data->open.status );
                state = BTAV_CONNECTION_STATE_DISCONNECTED;
            }

            /* change state to open based on the status */
            if (p_bta_data->open.status == BTA_AVK_SUCCESS)
            {
                /* inform the application of the event */
                btif_report_connection_state(state, (bt_bdaddr_t *)&(btif_avk_cb[index].peer_bda));
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_OPENED);
            }
            /* Bring up AVRCP connection too */
            BTA_AvkOpenRc(btif_avk_cb[index].bta_handle);
            btif_queue_advance();
        } break;

        case BTA_AVK_REMOTE_CMD_EVT:
        case BTA_AVK_VENDOR_CMD_EVT:
        case BTA_AVK_META_MSG_EVT:
        case BTA_AVK_RC_FEAT_EVT:
        case BTA_AVK_REMOTE_RSP_EVT:
        case BTA_AVK_BROWSE_MSG_EVT:
        case BTA_AVK_RC_BROWSE_OPEN_EVT:
            btif_avk_rc_handler(event, (tBTA_AVK*)p_data);
            break;

        case BTA_AVK_RC_CLOSE_EVT:
            BTIF_TRACE_DEBUG("BTA_AV_RC_CLOSE_EVT: Stopping AV timer.");
            if(alarm_is_scheduled(avk_open_on_rc_timer)) {
                alarm_cancel(avk_open_on_rc_timer);
            }
            btif_avk_rc_handler(event, (tBTA_AVK*)p_data);
            break;

        case BTIF_AVK_SPLIT_SINK_START_CAPTURE:
            BTIF_TRACE_WARNING("Sending ACK_FAILURE since BT connection is not established");
            btif_update_cmd_status(A2DP_AVK_CTRL_CMD_START_CAPTURE, A2DP_CTRL_ACK_FAILURE);
            break;

             /* We get these event in Idle State when some times in suspending state,
              * BTA_AVK_CLOSE_EVT is handled before the
              * VSC_STOP_COMPLETE_EVT and ACK is not sent to Audio hal.
              * hanlde the VSC_STOP_COMPLETE_EVT in idle state.
              */
        case BTIF_AVK_SPLIT_SINK_VSC_STOP_COMPLETE_EVT:
        case BTIF_AVK_SPLIT_SINK_STOP_CAPTURE:
             BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
             if (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STOP) {
                btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_SUCCESS);
             } else {
                btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_FAILURE);
             }
             break;

        default:
            BTIF_TRACE_WARNING("%s : unhandled event:%s", __FUNCTION__,
                                dump_avk_sm_event_name((btif_avk_sm_event_t)event));
            return FALSE;
    }

    return TRUE;
}
/*****************************************************************************
**
** Function        btif_avk_split_state_opening_handler
**
** Description     Intermediate state managing events during establishment
**                 of avdtp channel
**
** Returns         TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static bool btif_avk_split_state_opening_handler(btif_sm_event_t event, void *p_data, int index)
{
    int i;
    BTIF_TRACE_IMP("%s event:%s flags %x on index = %d", __FUNCTION__,
                     dump_avk_sm_event_name((btif_avk_sm_event_t)event), btif_avk_cb[index].flags, index);
    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            /* inform the application that we are entering connecting state */
            if (bt_avk_callbacks != NULL)
            {
                HAL_CBACK(bt_avk_callbacks, connection_state_cb,
                         btif_avk_cb[index].peer_bda, BTAV_CONNECTION_STATE_CONNECTING);
            }
            break;

        case BTIF_SM_EXIT_EVT:
            btif_avk_cb[index].prev_state = BTIF_AVK_STATE_OPENING;
            break;

        case BTIF_AVK_SPLIT_SINK_CHECK_A2DP_READY:
            BTIF_TRACE_DEBUG(" VSC command Status %d",btif_avk_cb[index].vsc_command_status);
            // in this state, we are not ready.
            btif_update_cmd_status(A2DP_AVK_CTRL_CMD_CHECK_READY,A2DP_CTRL_ACK_FAILURE);
            break;

        case BTA_AVK_REJECT_EVT:
            BTIF_TRACE_DEBUG(" Received  BTA_AVK_REJECT_EVT ");
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
                                        (bt_bdaddr_t *)&(btif_avk_cb[index].peer_bda));
            btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_IDLE);
            break;

        case BTA_AVK_OPEN_EVT:
        {
            tBTA_AVK *p_bta_data = (tBTA_AVK*)p_data;
            btav_connection_state_t state;
            btif_sm_state_t av_state;
            BTIF_TRACE_DEBUG("status:%d, edr 0x%x, role: 0x%x",p_bta_data->open.status,
                             p_bta_data->open.edr, p_bta_data->open.role);

            if (p_bta_data->open.status == BTA_AVK_SUCCESS)
            {
                 state = BTAV_CONNECTION_STATE_CONNECTED;
                 av_state = BTIF_AVK_STATE_OPENED;
                 btif_avk_cb[index].edr = p_bta_data->open.edr;
                 if (p_bta_data->open.role == HOST_ROLE_SLAVE)
                 {
                    btif_avk_cb[index].is_slave = TRUE;
                 }
                 if (p_bta_data->open.edr & BTA_AVK_EDR_3MBPS)
                 {
                     BTIF_TRACE_DEBUG("remote supports 3 mbps");
                     btif_avk_cb[index].edr_3mbps = TRUE;
                 }
                 btif_avk_cb[index].avdt_sync = bta_avk_is_avdt_sync(btif_avk_cb[index].bta_handle);
                 BTIF_TRACE_DEBUG(" %s ~~ BTA_AVK_OPEN_EVT btif_avk_cb[%d].avdt_sync is [%d]",__func__,index, btif_avk_cb[index].avdt_sync);
            }
            else
            {
                BTIF_TRACE_WARNING("BTA_AVK_OPEN_EVT::FAILED status: %d",
                                     p_bta_data->open.status );
                RawAddress peer_addr;
                if ((btif_avk_rc_get_connected_peer(&peer_addr))&&(btif_avk_cb[index].peer_bda == peer_addr))
                {
                    /* Disconnect AVRCP connection, if A2DP
                     * conneciton failed, for any reason
                     */
                    BTIF_TRACE_WARNING(" %s Disconnecting AVRCP ",__func__);
                    BTA_AvkCloseRc(btif_avk_rc_get_connected_peer_handle(peer_addr));
                }
                state = BTAV_CONNECTION_STATE_DISCONNECTED;
                av_state  = BTIF_AVK_STATE_IDLE;
            }

            /* inform the application of the event */
            btif_report_connection_state(state, &(btif_avk_cb[index].peer_bda));
            /* change state to open/idle based on the status */
            btif_sm_change_state(btif_avk_cb[index].sm_handle, av_state);
            if (p_bta_data->open.status == BTA_AVK_SUCCESS)
            {
                BTA_AvkOpenRc(btif_avk_cb[index].bta_handle);
            }
            btif_queue_advance();
        } break;

        case BTA_AVK_MTU_CONFIG_EVT:
        {
            tBTA_AVK *p_av = (tBTA_AVK*)p_data;
            BTIF_TRACE_DEBUG("%s: BTA_AVK_MTU_CONFIG_EVT", __func__);
            HAL_CBACK(bt_av_sink_vendor_callbacks, mtu_config_cb,
                      p_av->mtu_config.mtu,btif_avk_cb[index].peer_bda);
        } break;

        case BTIF_AVK_SINK_CONFIG_REQ_EVT:
        {
            btif_avk_config_req_t req;
            // copy to avoid alignment problems
            memcpy(&req, p_data, sizeof(req));


            BTIF_TRACE_DEBUG("BTIF_AVK_SINK_CONFIG_REQ_EVT %d %d %s %d",
                    req.sample_rate, req.channel_count, btif_avk_cb[index].peer_bda.ToString().c_str(), req.codec_type);

            if (bt_av_sink_vendor_callbacks != NULL) {
                HAL_CBACK(bt_av_sink_vendor_callbacks, audio_codec_config_vendor_cb,
                        &(btif_avk_cb[index].peer_bda), req.codec_type, req.codec_info);
            }
        } break;

        case BTIF_AVK_CONNECT_REQ_EVT:
            // Check for device, if same device which moved to opening then ignore callback
            if (memcmp ((bt_bdaddr_t*)p_data, &(btif_avk_cb[index].peer_bda),
                sizeof(btif_avk_cb[index].peer_bda)) == 0)
            {
                BTIF_TRACE_DEBUG("%s: Same device moved to Opening state,ignore Connect Req", __func__);
                btif_queue_advance();
                break;
            }
            else
            {
                BTIF_TRACE_DEBUG("%s: Moved from idle by Incoming Connection request", __func__);
                btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED, (bt_bdaddr_t*)p_data);
                btif_queue_advance();
                break;
            }

        case BTA_AVK_PENDING_EVT:
            // Check for device, if same device which moved to opening then ignore callback
            if  (((tBTA_AVK*)p_data)->pend.bd_addr == btif_avk_cb[index].peer_bda)
            {
                BTIF_TRACE_DEBUG("%s: Same device moved to Opening state,ignore Pending Req", __func__);
                break;
            }
            else
            {
                BTIF_TRACE_DEBUG("%s: Moved from idle by outgoing Connection request", __func__);
                BTA_AvkDisconnect(((tBTA_AVK*)p_data)->pend.bd_addr);
                break;
            }

        case BTA_AVK_CLOSE_EVT:

            btif_avk_cb[index].avdt_sync = FALSE;
            /* inform the application that we are disconnected */
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_avk_cb[index].peer_bda));
            btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_IDLE);
            break;

        case BTIF_AVK_DISCONNECT_REQ_EVT:
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_avk_cb[index].peer_bda));
            BTA_AvkClose(btif_avk_cb[index].bta_handle);
            btif_queue_advance();
            btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_IDLE);
            break;

        case BTIF_AVK_SPLIT_SINK_START_CAPTURE:
            BTIF_TRACE_WARNING("Sending ACK_FAILURE since BT connection is not established");
            btif_update_cmd_status(A2DP_AVK_CTRL_CMD_START_CAPTURE, A2DP_CTRL_ACK_FAILURE);
            break;

        CHECK_AVK_RC_EVENT(event, (tBTA_AVK *)p_data);

        default:
            BTIF_TRACE_WARNING("%s : unhandled event:%s", __FUNCTION__,
                                dump_avk_sm_event_name((btif_avk_sm_event_t)event));
            return FALSE;

   }
   return TRUE;
}


/*****************************************************************************
**
** Function        btif_avk_split_state_closing_handler
**
** Description     Intermediate state managing events during closing
**                 of avdtp channel
**
** Returns         TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static bool btif_avk_split_state_closing_handler(btif_sm_event_t event, void *p_data, int index)
{
    BTIF_TRACE_IMP("%s event:%s flags %x and index = %d", __FUNCTION__,
                     dump_avk_sm_event_name((btif_avk_sm_event_t)event), btif_avk_cb[index].flags, index);

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            break;

        case BTA_AVK_STOP_EVT:
        case BTIF_AVK_STOP_STREAM_REQ_EVT:
            break;

        case BTIF_SM_EXIT_EVT:
            btif_avk_cb[index].prev_state = BTIF_AVK_STATE_CLOSING;
            break;

        case BTA_AVK_CLOSE_EVT:

            /* inform the application that we are disconnecting */
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED, (bt_bdaddr_t *)&(btif_avk_cb[index].peer_bda));

            btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_IDLE);
            break;

        /* Handle the RC_CLOSE event for the cleanup */
        case BTA_AVK_RC_CLOSE_EVT:
            btif_avk_rc_handler(event, (tBTA_AVK*)p_data);
            break;

        case BTIF_AVK_SPLIT_SINK_START_CAPTURE:
             BTIF_TRACE_WARNING("Sending ACK_FAILURE since BT connection is not established");
             btif_update_cmd_status(A2DP_AVK_CTRL_CMD_START_CAPTURE, A2DP_CTRL_ACK_FAILURE);
             break;

        case BTIF_AVK_SPLIT_SINK_CHECK_A2DP_READY:
            BTIF_TRACE_DEBUG(" VSC command Status %d",btif_avk_cb[index].vsc_command_status);
            // in this state, we are not ready.
            btif_update_cmd_status(A2DP_AVK_CTRL_CMD_CHECK_READY,A2DP_CTRL_ACK_FAILURE);
            break;

        default:
            BTIF_TRACE_WARNING("%s : unhandled event:%s", __FUNCTION__,
                                dump_avk_sm_event_name((btif_avk_sm_event_t)event));
            return FALSE;
   }
   return TRUE;
}


/*****************************************************************************
**
** Function     btif_avk_split_state_opened_handler
**
** Description  Handles AV events while AVDTP is in OPEN state
**
** Returns      TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static bool btif_avk_split_state_opened_handler(btif_sm_event_t event, void *p_data, int index)
{
    tBTA_AVK *p_av = (tBTA_AVK*)p_data;
    tBTIF_STATUS status = BTIF_SUCCESS;

    BTIF_TRACE_IMP("%s event:%s flags %x  index =%d VSC_Cmd_Status = %d", __FUNCTION__,
         dump_avk_sm_event_name((btif_avk_sm_event_t)event), btif_avk_cb[index].flags, index,
         btif_avk_cb[index].vsc_command_status);


    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            btif_avk_cb[index].flags &= ~BTIF_AVK_FLAG_PENDING_STOP;
            //Cancel avk_open_on_rc_timer after a2dp connection
            if(alarm_is_scheduled(avk_open_on_rc_timer)) {
                alarm_cancel(avk_open_on_rc_timer);
            }
            handle_memorized_msgs(index);
            break;

        case BTIF_SM_EXIT_EVT:
            btif_avk_cb[index].prev_state = BTIF_AVK_STATE_OPENED;
            break;

        case BTIF_AVK_START_STREAM_REQ_EVT:
            // we received start request from btapp. only update streaming device here,
            streaming_bda = btif_avk_cb[index].peer_bda;
            BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
            if (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
                // VSC command has already been sent. directly send start req to remote
                BTA_AvkStart(btif_avk_cb[index].bta_handle);
            } else {
                // wait for BTIF_AVK_SPLIT_SINK_START_CAPTURE
                btif_avk_cb[index].flags |= BTIF_AVK_FLAG_PENDING_START;
            }
            break;
        case BTIF_AVK_SPLIT_SINK_START_CAPTURE:
            btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
            // send a message to send VSC command
            btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_START, &index,
                                       sizeof(uint8_t));
            break;
        case BTIF_AVK_SPLIT_SINK_VSC_START_COMPLETE_EVT:
            BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
            if (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
                // Vsc successful,ack back,  wait for sink setup complete.
                btif_update_cmd_status(A2DP_AVK_CTRL_CMD_START_CAPTURE, A2DP_CTRL_ACK_SUCCESS);
            } else {
                btif_update_cmd_status(A2DP_AVK_CTRL_CMD_START_CAPTURE, A2DP_CTRL_ACK_FAILURE);
            }
            break;
        case BTIF_AVK_SPLIT_SINK_SETUP_COMPLETE:
        {
            uint8_t latency[8];
            memcpy(latency, p_data, 8);
            if((btif_avk_cb[index].flags & BTIF_AVK_FLAG_PENDING_START)) {
                // if start request is sent earlier, we were waiting for session
                // complete to happen to send START_REQUEST.
                BTA_AvkStart(btif_avk_cb[index].bta_handle);
                btif_avk_cb[index].flags &= ~ BTIF_AVK_FLAG_PENDING_START;
                btif_avk_cb[index].delay = 0;
                btif_avk_cb[index].delay = (latency[0] & 0x000000FF) |
                                           ((latency[1] << 8) & 0x0000FF00) |
                                           ((latency[2] << 16) & 0x00FF0000) |
                                           ((latency[3] << 32) & 0xFF000000);
                if(btif_avk_cb[index].avdt_sync) {
                    BTA_AvkUpdateDelayReport(btif_avk_cb[index].bta_handle);
                }
            } else {
                BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
                switch(btif_avk_cb[index].vsc_command_status) {
                    case BTIF_AVK_VSC_STARTED:
                        btif_update_cmd_status(A2DP_AVK_CTRL_SESSION_SETUP_COMPLETE,
                                        A2DP_CTRL_ACK_SUCCESS);
                        btif_avk_cb[index].delay = 0;
                        btif_avk_cb[index].delay = (latency[0] & 0x000000FF) |
                                                   ((latency[1] << 8) & 0x0000FF00) |
                                                   ((latency[2] << 16) & 0x00FF0000) |
                                                   ((latency[3] << 32) & 0xFF000000);
                        if(btif_avk_cb[index].avdt_sync) {
                            BTA_AvkUpdateDelayReport(btif_avk_cb[index].bta_handle);
                        }
                        break;
                    case BTIF_AVK_VSC_STOP:
                        // VSC would have failed, by this time
                        btif_update_cmd_status(A2DP_AVK_CTRL_SESSION_SETUP_COMPLETE,
                                        A2DP_CTRL_ACK_FAILURE);
                        break;
                    default:
                        BTIF_TRACE_ERROR("%s  unexpected state %d ",__func__,
                                    btif_avk_cb[index].vsc_command_status);
                        break;
                }
            }
            break;
        }
        case BTIF_AVK_SINK_START_STREAM_REQ_EVT:
            //check if AVRCP connection is there, send AVRCP_PLAY, otherwise send AVDTP_START
            // check if rc connected
            if(btif_avk_rc_get_connected_peer_handle(btif_avk_cb[index].peer_bda))
            {
                // send avrcp play
                btif_avk_rc_ctrl_send_play(&btif_avk_cb[index].peer_bda);
            }
            else
            {
                streaming_bda = btif_avk_cb[index].peer_bda;
                BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
                if (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
                    // VSC command has already been sent. directly send start req to remote
                    BTA_AvkStart(btif_avk_cb[index].bta_handle);
                } else {
                    // wait for BTIF_AVK_SPLIT_SINK_START_CAPTURE
                    btif_avk_cb[index].flags |= BTIF_AVK_FLAG_PENDING_START;
                }
            }
            break;
        case BTA_AVK_START_EVT:
        {
            btif_sm_state_t av_state = BTIF_AVK_STATE_STARTED;
            BTIF_TRACE_IMP("BTA_AVK_START_EVT status %d, suspending %d, init %d",
                p_av->start.status, p_av->start.suspending, p_av->start.initiator);
            BTIF_TRACE_IMP("BTA_AVK_START_EVT role: %d", p_av->start.role);
            if (p_av->start.role == HOST_ROLE_SLAVE)
            {
                btif_avk_cb[index].is_slave = TRUE;
            }
            else
            {
                // update if we are master after role switch before start
                btif_avk_cb[index].is_slave = FALSE;
            }

            if ((p_av->start.status == BTA_SUCCESS) && (p_av->start.suspending == TRUE))
                return TRUE;

            if (p_av->start.status != BTA_AVK_SUCCESS)
            {
                return FALSE;
            }

            if (p_av->start.initiator == TRUE) {
                // we got START_RSP from remote, DUT Initiator
                switch(btif_avk_cb[index].vsc_command_status) {
                    case BTIF_AVK_VSC_STARTED:
                        btif_update_cmd_status(A2DP_AVK_CTRL_SESSION_SETUP_COMPLETE,
                                        A2DP_CTRL_ACK_SUCCESS);
                        break;
                    case BTIF_AVK_VSC_STOP:
                        // VSC would have failed, by this time
                        btif_update_cmd_status(A2DP_AVK_CTRL_SESSION_SETUP_COMPLETE,
                                        A2DP_CTRL_ACK_FAILURE);
                        break;
                    default:
                        BTIF_TRACE_ERROR("%s  unexpected state %d ",__func__,
                                    btif_avk_cb[index].vsc_command_status);
                        break;
                    }
                av_state = BTIF_AVK_STATE_STARTED;
            } else {
                // incmoing START request. send HAL_CBAC to btapp
                av_state = BTIF_AVK_STATE_STARTING;
                // if streaming_bda is empty, only then assign
                if(streaming_bda.IsEmpty()) {
                    streaming_bda = btif_avk_cb[index].peer_bda;
                }
                alarm_set_on_mloop(avk_pending_start_timer,BTIF_AVK_PENDING_RESPONSE_TIMEOUT,
                                btif_avk_pending_start_timeout_handler,
                                (void *)&btif_avk_cb[index].peer_bda);
                BTIF_TRACE_IMP("%s : alarm set = %d",__func__,
                                alarm_is_scheduled(avk_pending_start_timer));
                HAL_CBACK(bt_av_sink_vendor_callbacks, start_ind_cb, btif_avk_cb[index].peer_bda);
            }
            btif_sm_change_state(btif_avk_cb[index].sm_handle, av_state);
        }
            break;

         case BTIF_AVK_SPLIT_SINK_VSC_STOP_COMPLETE_EVT:
            BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                            btif_avk_cb[index].vsc_command_status);
            if (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STOP) {
               btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_SUCCESS);
            } else {
               btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_FAILURE);
            }
            break;

        case BTIF_AVK_SPLIT_SINK_STOP_CAPTURE:
            BTIF_TRACE_EVENT("%s vsc status  %d",__FUNCTION__,btif_avk_cb[index].vsc_command_status);
            switch(btif_avk_cb[index].vsc_command_status) {
                case BTIF_AVK_VSC_STARTED:
                    // send stop command to soc
                    btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
                    // send a message to send VSC command
                    btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP, &index,
                                       sizeof(uint8_t));
                    break;
                case BTIF_AVK_VSC_STOP:
                    // VSC Stop is already done, just ack back
                    btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_SUCCESS);
                    break;
                default:
                    BTIF_TRACE_ERROR("%s  unexpected state %d ",__func__,btif_avk_cb[index].vsc_command_status);
                    break;
            }
            break;

        case BTIF_AVK_SPLIT_SINK_CHECK_A2DP_READY:
            BTIF_TRACE_DEBUG(" VSC command Status %d flag %d ",
                       btif_avk_cb[index].vsc_command_status,
                      (btif_avk_cb[index].flags & BTIF_AVK_FLAG_PENDING_START));
            // in this state, we are not ready.
            if(btif_avk_cb[index].flags & BTIF_AVK_FLAG_PENDING_START)
            {
                btif_update_cmd_status(A2DP_AVK_CTRL_CMD_CHECK_READY, A2DP_CTRL_ACK_SUCCESS);
            }
            else
            {
                btif_update_cmd_status(A2DP_AVK_CTRL_CMD_CHECK_READY, A2DP_CTRL_ACK_FAILURE);
            }
            break;

        case BTIF_AVK_DISCONNECT_REQ_EVT:
            BTIF_TRACE_EVENT("%s vsc status  %d",__FUNCTION__,btif_avk_cb[index].vsc_command_status);
            if(btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
                // memorize this message, need to execute this again after sending VSC command.
                memorize_msg(index,event,BTIF_AVK_STATE_OPENED, p_data);
                // send stop command to soc and once confirmed, send suspend rquest to remote
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
                // send a message to send VSC command
                btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP, &index,
                                    sizeof(uint8_t));
                break;
            }
            BTA_AvkClose(btif_avk_cb[index].bta_handle);
            BTA_AvkCloseRc(btif_avk_cb[index].bta_handle);

            /* inform the application that we are disconnecting */
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTING, (bt_bdaddr_t *)&(btif_avk_cb[index].peer_bda));
            break;

        case BTA_AVK_CLOSE_EVT:
             /* avdtp link is closed */
            /* inform the application that we are disconnected */
            BTIF_TRACE_EVENT("%s vsc status  %d",__FUNCTION__,btif_avk_cb[index].vsc_command_status);
            if(btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
                // memorize this message, need to execute this again after sending VSC command.
                memorize_msg(index,event,BTIF_AVK_STATE_OPENED, p_data);
                // send stop command to soc and once confirmed, send suspend rquest to remote
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
                // send a message to send VSC command
                btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP, &index,
                                    sizeof(uint8_t));
                break;
            }
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
                                        (bt_bdaddr_t *)&(btif_avk_cb[index].peer_bda));

            btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_IDLE);
            break;

        case BTA_AVK_RECONFIG_EVT:
            if((btif_avk_cb[index].flags & BTIF_AVK_FLAG_PENDING_START) &&
                (p_av->reconfig.status == BTA_AVK_SUCCESS))
            {
               APPL_TRACE_WARNING("reconfig done BTA_AVstart()");
               BTA_AvkStart(btif_avk_cb[index].bta_handle);
            }
            else if(btif_avk_cb[index].flags & BTIF_AVK_FLAG_PENDING_START)
            {
               btif_avk_cb[index].flags &= ~BTIF_AVK_FLAG_PENDING_START;
            }
            break;

        case BTIF_AVK_CONNECT_REQ_EVT:
            if (memcmp ((bt_bdaddr_t*)p_data, &(btif_avk_cb[index].peer_bda),
                sizeof(btif_avk_cb[index].peer_bda)) == 0)
            {
                BTIF_TRACE_DEBUG("%s: Ignore BTIF_AVK_CONNECT_REQ_EVT for same device", __func__);
            }
            else
            {
                BTIF_TRACE_DEBUG("%s: Moved to opened by Other Incoming Conn req", __func__);
                btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
                        (bt_bdaddr_t*)p_data);
            }
            btif_queue_advance();
            break;

        CHECK_AVK_RC_EVENT(event, (tBTA_AVK*)p_data);

        case BTIF_AVK_SINK_CONFIG_REQ_EVT:
        {
            btif_avk_config_req_t req;
            // copy to avoid alignment problems
            memcpy(&req, p_data, sizeof(req));
            BTIF_TRACE_DEBUG("BTIF_AVK_SINK_CONFIG_REQ_EVT %d %d %s %d",
                    req.sample_rate, req.channel_count, (btif_avk_cb[index].peer_bda).ToString().c_str(), req.codec_type);
            if (bt_av_sink_vendor_callbacks != NULL) {
                HAL_CBACK(bt_av_sink_vendor_callbacks, audio_codec_config_vendor_cb,
                        &(btif_avk_cb[index].peer_bda), req.codec_type, req.codec_info);
            }
        } break;

        case BTA_AVK_MTU_CONFIG_EVT:
        {
            BTIF_TRACE_DEBUG("%s: BTA_AVK_MTU_CONFIG_EVT", __func__);
            HAL_CBACK(bt_av_sink_vendor_callbacks, mtu_config_cb,
                      p_av->mtu_config.mtu,btif_avk_cb[index].peer_bda);
        } break;

        default:
            BTIF_TRACE_WARNING("%s : unhandled event:%s", __FUNCTION__,
                               dump_avk_sm_event_name((btif_avk_sm_event_t)event));
            return FALSE;

    }
    return TRUE;
}

/*****************************************************************************
**
** Function     btif_avk_split_state_suspending_handler
**
** Description  Handles AV events while A2DP stream is suspending state
**
** Returns      TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static bool btif_avk_split_state_suspending_handler(btif_sm_event_t event, void *p_data, int index)
{
    tBTA_AVK *p_av = (tBTA_AVK*)p_data;

    BTIF_TRACE_IMP("%s event:%s flags %x  index =%d VSC_Cmd_Status = %d", __FUNCTION__,
         dump_avk_sm_event_name((btif_avk_sm_event_t)event), btif_avk_cb[index].flags, index,
         btif_avk_cb[index].vsc_command_status);

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            handle_memorized_msgs(index);
            break;
        case BTIF_SM_EXIT_EVT:
            btif_avk_cb[index].prev_state = BTIF_AVK_STATE_SUSPENDING;
            break;
        case BTIF_AVK_SPLIT_SINK_SUSPEND_IND_RSP:
             BTIF_TRACE_DEBUG(" %s SUSPEND_CMD_RSP accepted =%d ",__FUNCTION__,
                             p_av->suspend_rsp.accepted);
             if (p_av->suspend_rsp.accepted == CMD_REJECTED) {
                 if(alarm_is_scheduled(avk_pending_suspend_timer))
                     alarm_cancel(avk_pending_suspend_timer);
                 BTA_AvkSendPedingSuspendRej(btif_avk_cb[index].bta_handle);
                 // ideally remote has suspended, not sure it will still send packets or not.
                 btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_STARTED);
             }
             // else suspend request is accepted, will wait for stop capture to happen
             break;
         case BTIF_AVK_SPLIT_SINK_STOP_CAPTURE:
             // MM-Audio sessoin is stopped
             BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
             if(alarm_is_scheduled(avk_pending_suspend_timer))
                 alarm_cancel(avk_pending_suspend_timer);
             // check the last vsc_command status.
            switch (btif_avk_cb[index].vsc_command_status) {
                case BTIF_AVK_VSC_STARTED:
                   // VSC START was successful, send VSC_STOP
                   btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
                   // send a message to send VSC command
                   btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP, &index,
                                       sizeof(uint8_t));
                   break;
                case BTIF_AVK_VSC_STOP:
                   // VSC Stop was already sent.send suspend cfm and move to OPENED
                   BTA_AvkSendPedingSuspendCnf(btif_avk_cb[index].bta_handle);
                   btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_OPENED);
                   btif_report_audio_state(BTAV_AUDIO_STATE_REMOTE_SUSPEND,
                                                            &(btif_avk_cb[index].peer_bda));
                   btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_SUCCESS);
                   break;
                case BTIF_AVK_VSC_STARTING:
                case BTIF_AVK_VSC_START_FAILED:
                case BTIF_AVK_VSC_STOPPING:
                   // this should not happen actually in this state.
                   break;
             }
             break;
         case BTIF_AVK_SPLIT_SINK_VSC_STOP_COMPLETE_EVT:
             BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
             if (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STOP) {
                BTA_AvkSendPedingSuspendCnf(btif_avk_cb[index].bta_handle);
                btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_SUCCESS);
                btif_report_audio_state(BTAV_AUDIO_STATE_REMOTE_SUSPEND, &(btif_avk_cb[index].peer_bda));
             } else {
                 btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_FAILURE);
             }
             btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_OPENED);
             break;
         case BTIF_AVK_DISCONNECT_REQ_EVT:
         case BTA_AVK_CLOSE_EVT:
         case BTA_AVK_STOP_EVT:
             if(alarm_is_scheduled(avk_pending_suspend_timer))
                 alarm_cancel(avk_pending_suspend_timer);
            BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
            memorize_msg(index,event,BTIF_AVK_STATE_SUSPENDING, p_data);
            if (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
                // cancel timer
                // VSC START was successful, send VSC_STOP
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
                // send a message to send VSC command
                btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP, &index,
                            sizeof(uint8_t));
            }
            if (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STOP) {
                BTA_AvkSendPedingSuspendCnf(btif_avk_cb[index].bta_handle);
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_OPENED);
                btif_report_audio_state(BTAV_AUDIO_STATE_STOPPED,
                                                        &(btif_avk_cb[index].peer_bda));
            }
            break;

        case BTIF_AVK_SPLIT_SINK_START_CAPTURE:
             BTIF_TRACE_WARNING("Sending ACK_FAILURE since BT connection is not established");
             btif_update_cmd_status(A2DP_AVK_CTRL_CMD_START_CAPTURE, A2DP_CTRL_ACK_FAILURE);
             break;

        case BTIF_AVK_SPLIT_PENDING_SUSPEND_ALARM_TIMEOUT:
             if(btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
             // Stop vsc and send suspend command to remote
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
                btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP, &index,
                           sizeof(uint8_t));
             }
            if ((btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STOP) ||
                (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_START_FAILED)) {
                BTA_AvkSendPedingSuspendCnf(btif_avk_cb[index].bta_handle);
                btif_report_audio_state(BTAV_AUDIO_STATE_REMOTE_SUSPEND,
                                                &(btif_avk_cb[index].peer_bda));
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_OPENED);
             }
             break;
        CHECK_AVK_RC_EVENT(event, (tBTA_AVK*)p_data);
        default:
            BTIF_TRACE_WARNING("%s : unhandled event:%s", __FUNCTION__,
                               dump_avk_sm_event_name((btif_avk_sm_event_t)event));
            return FALSE;
    }
    return TRUE;
}

/*****************************************************************************
**
** Function     btif_avk_split_state_starting_handler
**
** Description  Handles AV events while A2DP stream is starting state
**
** Returns      TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static bool btif_avk_split_state_starting_handler(btif_sm_event_t event, void *p_data, int index)
{
    tBTA_AVK *p_av = (tBTA_AVK*)p_data;

    BTIF_TRACE_IMP("%s event:%s flags %x  index =%d", __FUNCTION__,
         dump_avk_sm_event_name((btif_avk_sm_event_t)event), btif_avk_cb[index].flags, index);

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            handle_memorized_msgs(index);
            break;
        // sink_start capture called by mm-audio, we will move to vsc state
        case BTIF_AVK_SPLIT_SINK_START_CAPTURE:
            if(alarm_is_scheduled(avk_pending_start_timer))
                alarm_cancel(avk_pending_start_timer);
            BTIF_TRACE_DEBUG(" VSC command Status %d",btif_avk_cb[index].vsc_command_status);
            if (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
                btif_update_cmd_status(A2DP_AVK_CTRL_CMD_CHECK_READY,A2DP_CTRL_ACK_SUCCESS);
                break;
            }
            btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
            // send a message to send VSC command
            btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_START, &index,
                                       sizeof(uint8_t));
            break;
            // once we receive setup complete, we send Start Confirm to remote
            // we will move to started states
        case BTIF_AVK_SPLIT_SINK_SETUP_COMPLETE:
        {
            uint8_t latency[8];
            memcpy(latency, p_data, 8);
            btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_STARTED);
            //BTA_AvkSendPedingStartCnf(btif_avk_cb[index].bta_handle);
            btif_update_cmd_status(A2DP_AVK_CTRL_SESSION_SETUP_COMPLETE, A2DP_CTRL_ACK_SUCCESS);
            btif_avk_cb[index].delay = 0;
            btif_avk_cb[index].delay = (latency[0] & 0x000000FF) |
                                       ((latency[1] << 8) & 0x0000FF00) |
                                       ((latency[2] << 16) & 0x00FF0000) |
                                       ((latency[3] << 32) & 0xFF000000);
            if(btif_avk_cb[index].avdt_sync) {
                BTA_AvkUpdateDelayReport(btif_avk_cb[index].bta_handle);
            }
            break;
            // if we are in this state, VSC command has already been sent
        }
        case BTIF_AVK_SPLIT_SINK_CHECK_A2DP_READY:
            BTIF_TRACE_DEBUG(" VSC command Status %d",btif_avk_cb[index].vsc_command_status);
            switch(btif_avk_cb[index].vsc_command_status) {
                case BTIF_AVK_VSC_STARTED:
                case BTIF_AVK_VSC_STOP:// in case check_ready is called before start_catpure
                    btif_update_cmd_status(A2DP_AVK_CTRL_CMD_CHECK_READY,A2DP_CTRL_ACK_SUCCESS);
                    break;
                default:
                    btif_update_cmd_status(A2DP_AVK_CTRL_CMD_CHECK_READY,A2DP_CTRL_ACK_FAILURE);
                    break;
             }
             break;
        case BTIF_AVK_SPLIT_SINK_START_IND_RSP:
            BTIF_TRACE_DEBUG(" %s START_CMD_RSP accepted =%d ",__FUNCTION__,
                             p_av->start_rsp.accepted);
             if (p_av->start_rsp.accepted == CMD_REJECTED) {
                 if(alarm_is_scheduled(avk_pending_start_timer))
                     alarm_cancel(avk_pending_start_timer);
                 BTA_AvkSendPedingStartRej(btif_avk_cb[index].bta_handle);
                 btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_OPENED);
             }
             break;
        case BTIF_AVK_SPLIT_SINK_SUSPEND_IND_RSP:
             BTIF_TRACE_DEBUG(" %s SUSPEND_CMD_RSP accepted =%d ",__FUNCTION__,
                             p_av->suspend_rsp.accepted);
             if (p_av->suspend_rsp.accepted == CMD_REJECTED) {
                 if(alarm_is_scheduled(avk_pending_suspend_timer))
                     alarm_cancel(avk_pending_suspend_timer);
                 BTA_AvkSendPedingSuspendRej(btif_avk_cb[index].bta_handle);
                 btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_OPENED);
             }
             break;
        case BTIF_AVK_SPLIT_SINK_STOP_CAPTURE:
             // MM-Audio sessoin creation failed
             BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
             switch (btif_avk_cb[index].vsc_command_status) {
               case BTIF_AVK_VSC_STARTED:
               case BTIF_AVK_VSC_START_FAILED:
                   // VSC Command was exchanged, but MM-Session setup failed
                   // send VSC_STOP in this case
                   btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
                   // send a message to send VSC command
                   btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP, &index,
                                       sizeof(uint8_t));
               case BTIF_AVK_VSC_STOP:
                   // we haven't sent VSC_START command  send START_REJ and move to opened
                   BTA_AvkSendPedingStartRej(btif_avk_cb[index].bta_handle);
                   btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_OPENED);
                   btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_SUCCESS);
                   break;
               case BTIF_AVK_VSC_STARTING:
               case BTIF_AVK_VSC_STOPPING:
                   // this should not happen actually in this state.
                   break;
             }
             break;
        case BTIF_AVK_SPLIT_SINK_VSC_START_COMPLETE_EVT:
             BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
             if (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
                 BTA_AvkSendPedingStartCnf(btif_avk_cb[index].bta_handle);
                 btif_update_cmd_status(A2DP_AVK_CTRL_CMD_START_CAPTURE, A2DP_CTRL_ACK_SUCCESS);
             } else {
                 btif_update_cmd_status(A2DP_AVK_CTRL_CMD_START_CAPTURE, A2DP_CTRL_ACK_FAILURE);
             }
             break;
        case BTIF_AVK_SPLIT_SINK_VSC_STOP_COMPLETE_EVT:
             BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
             if (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STOP) {
                 btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_SUCCESS);
             } else {
                 btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_FAILURE);
             }
             break;
        case BTIF_AVK_SPLIT_PENDING_START_ALARM_TIMEOUT:
             switch(btif_avk_cb[index].vsc_command_status) {
                 case BTIF_AVK_VSC_STOP:
                 case BTIF_AVK_VSC_START_FAILED:
                 case BTIF_AVK_VSC_STARTED:
                     BTIF_TRACE_DEBUG("%s: send start cfm",__func__);
                     // in starting stating state, start_capture event not received from audio
                     btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_STARTED);
                     BTA_AvkSendPedingStartCnf(btif_avk_cb[index].bta_handle);
                     break;
                 default:
                     //Dont do anything
                     break;
             }
             break;
        case BTIF_SM_EXIT_EVT:
             btif_avk_cb[index].prev_state = BTIF_AVK_STATE_STARTING;
             break;
        case BTIF_AVK_DISCONNECT_REQ_EVT:
            BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);

            //Cancel alarm (if schedules) and send stsrt response
            if(alarm_is_scheduled(avk_pending_start_timer)) {
                alarm_cancel(avk_pending_start_timer);
            }
            BTA_AvkSendPedingStartCnf(btif_avk_cb[index].bta_handle);

            /*memorize disconnect_req and move to started state.
             It is handled in started state*/
            memorize_msg(index,event,BTIF_AVK_STATE_STARTING, p_data);
            btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_STARTED);
            break;
        case BTA_AVK_CLOSE_EVT:
        case BTA_AVK_STOP_EVT:
            BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
            if(btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
                // VSC command was sent but we got stop before setup-complete
                // memorize this message and move to started state, there we're
                // handling this message.
                memorize_msg(index,event,BTIF_AVK_STATE_STARTING, p_data);
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_STARTED);
            }
            if(btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTING ||
               btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STOPPING) {
                // VSC command is in progress, just memorize this message
                memorize_msg(index,event,BTIF_AVK_STATE_STARTING, p_data);
            } else {
                // cancel start_pending alarm and move to idle state
                if(alarm_is_scheduled(avk_pending_start_timer)) {
                    alarm_cancel(avk_pending_start_timer);
                }
                /* inform the application that we are disconnected */
                btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
                                         (bt_bdaddr_t *)&(btif_avk_cb[index].peer_bda));
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_IDLE);
            }
            break;
        CHECK_AVK_RC_EVENT(event, (tBTA_AVK*)p_data);
        default:
            BTIF_TRACE_WARNING("%s : unhandled event:%s", __FUNCTION__,
                               dump_avk_sm_event_name((btif_avk_sm_event_t)event));
            return FALSE;
    }
    return TRUE;
}

/*****************************************************************************
**
** Function     btif_avk_split_state_vsc_handler
**
** Description  Handles AV events while we are handling VSC command
**
** Returns      TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/
// this is an intermediate state.
static bool btif_avk_split_state_vsc_handler(btif_sm_event_t event, void *p_data, int index)
{
    tBTA_AVK *p_av = (tBTA_AVK*)p_data;

    BTIF_TRACE_IMP("%s event:%s flags %x  index =%d", __FUNCTION__,
         dump_avk_sm_event_name((btif_avk_sm_event_t)event), btif_avk_cb[index].flags, index);

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            break;
        case BTIF_SM_EXIT_EVT:
            btif_avk_cb[index].prev_state = BTIF_AVK_STATE_HDL_VSC;
            break;
        case BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_START:
            btif_avk_cb[index].vsc_command_status = BTIF_AVK_VSC_STARTING;
            BTA_AvkOffloadStart(btif_avk_cb[index].bta_handle);
            break;
        case BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP:
            btif_avk_cb[index].vsc_command_status = BTIF_AVK_VSC_STOPPING;
            BTA_AvkOffloadStop(btif_avk_cb[index].bta_handle);
            break;
        case BTA_AVK_OFFLOAD_START_RSP_EVT:
            BTIF_TRACE_DEBUG(" %s OFFLOAD_START_RSP Status %d vsc_cmd_status %d",__func__,
                             p_av->offload_rsp.status,btif_avk_cb[index].vsc_command_status);
            if (p_av->offload_rsp.status == 0) {
                btif_avk_cb[index].vsc_command_status = BTIF_AVK_VSC_STARTED;
            } else {
                btif_avk_cb[index].vsc_command_status = BTIF_AVK_VSC_START_FAILED;
            }
            btif_sm_change_state(btif_avk_cb[index].sm_handle, btif_avk_cb[index].prev_state);
            btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_VSC_START_COMPLETE_EVT, &index,
                                       sizeof(uint8_t));
            break;
        case BTA_AVK_OFFLOAD_STOP_RSP_EVT:
            btif_avk_cb[index].vsc_command_status = BTIF_AVK_VSC_STOP;
            btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_VSC_STOP_COMPLETE_EVT, &index,
                                       sizeof(uint8_t));
            btif_sm_change_state(btif_avk_cb[index].sm_handle, btif_avk_cb[index].prev_state);
            break;
        // memorize all other message here, we will act on them once we move to stable state
        default:
            memorize_msg(index,event,BTIF_AVK_STATE_HDL_VSC, p_data);
            break;
    }
    return TRUE;
}
/*****************************************************************************
**
** Function     btif_avk_split_state_started_handler
**
** Description  Handles AV events while A2DP stream is started
**
** Returns      TRUE if event was processed, FALSE otherwise
**
*******************************************************************************/

static bool btif_avk_split_state_started_handler(btif_sm_event_t event, void *p_data, int index)
{
    tBTA_AVK *p_av = (tBTA_AVK*)p_data;
    btif_sm_state_t state = BTIF_AVK_STATE_IDLE;
    int i;

    BTIF_TRACE_IMP("%s event:%s flags %x  index =%d VSC_Cmd_Status = %d", __FUNCTION__,
         dump_avk_sm_event_name((btif_avk_sm_event_t)event), btif_avk_cb[index].flags, index,
         btif_avk_cb[index].vsc_command_status);

    switch (event)
    {
        case BTIF_SM_ENTER_EVT:
            /* we are again in started state, clear any remote suspend flags */
            btif_avk_cb[index].flags &= ~BTIF_AVK_FLAG_REMOTE_SUSPEND;
            // if we are coming back from HDL_VSC/STARTING,
            // no need to send STARTED_IND to app
            btif_report_audio_state(BTAV_AUDIO_STATE_STARTED, &(btif_avk_cb[index].peer_bda));
            handle_memorized_msgs(index);
            btif_avk_cb[index].flags &= ~ BTIF_AVK_FLAG_PENDING_START;
            break;

        case BTIF_SM_EXIT_EVT:
            /* restore the a2dp consumer task priority when stop audio playing. */
            //adjust_priority_a2dp(FALSE);
            btif_avk_cb[index].prev_state = BTIF_AVK_STATE_STARTED;
            break;

        case BTIF_AVK_START_STREAM_REQ_EVT:
            break;

        case BTA_AVK_START_EVT:
            BTIF_TRACE_EVENT("%s Already in started state, send CNF",__FUNCTION__);
            BTA_AvkSendPedingStartCnf(btif_avk_cb[index].bta_handle);
            break;

        case BTIF_AVK_SPLIT_SINK_START_CAPTURE:
            BTIF_TRACE_EVENT("%s vsc status  %d",__FUNCTION__,
                        btif_avk_cb[index].vsc_command_status);
            switch(btif_avk_cb[index].vsc_command_status) {
                case BTIF_AVK_VSC_STARTED:
                    btif_update_cmd_status(A2DP_AVK_CTRL_CMD_START_CAPTURE, A2DP_CTRL_ACK_SUCCESS);
                    break;
                case BTIF_AVK_VSC_STOP:
                case BTIF_AVK_VSC_START_FAILED:
                    /* In started State, VSC not sent */
                    btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
                    // send a message to send VSC command
                    btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_START,
                                    &index, sizeof(uint8_t));
                    break;
                case BTIF_AVK_VSC_STARTING:
                case BTIF_AVK_VSC_STOPPING:
                /* Don't do anything, let's first come back to stable state */
                    break;
            }
            break;

        case BTIF_AVK_SPLIT_SINK_SETUP_COMPLETE:
            switch(btif_avk_cb[index].vsc_command_status) {
                case BTIF_AVK_VSC_STARTED:
                {
                    uint8_t latency[8];
                    memcpy(latency, p_data, 8);
                    btif_update_cmd_status(A2DP_AVK_CTRL_SESSION_SETUP_COMPLETE,
                                    A2DP_CTRL_ACK_SUCCESS);
                    btif_avk_cb[index].delay = 0;
                    btif_avk_cb[index].delay = (latency[0] & 0x000000FF) |
                                               ((latency[1] << 8) & 0x0000FF00) |
                                               ((latency[2] << 16) & 0x00FF0000) |
                                               ((latency[3] << 32) & 0xFF000000);
                    if(btif_avk_cb[index].avdt_sync) {
                        BTA_AvkUpdateDelayReport(btif_avk_cb[index].bta_handle);
                    }
                    break;
                }
                default:
                    btif_update_cmd_status(A2DP_AVK_CTRL_SESSION_SETUP_COMPLETE,
                                    A2DP_CTRL_ACK_FAILURE);
                    break;
            }
            break;

        case BTIF_AVK_SPLIT_SINK_VSC_START_COMPLETE_EVT:
            BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
            if (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
                // Vsc successful,ack back,  wait for sink setup complete.
                btif_update_cmd_status(A2DP_AVK_CTRL_CMD_START_CAPTURE, A2DP_CTRL_ACK_SUCCESS);
            } else {
                btif_update_cmd_status(A2DP_AVK_CTRL_CMD_START_CAPTURE, A2DP_CTRL_ACK_FAILURE);
            }
            break;

        case BTIF_AVK_SPLIT_SINK_STOP_CAPTURE:
            BTIF_TRACE_EVENT("%s vsc status  %d",__FUNCTION__,btif_avk_cb[index].vsc_command_status);
            switch(btif_avk_cb[index].vsc_command_status) {
                case BTIF_AVK_VSC_STARTED:
                    // send stop command to soc and once confirmed, send suspend rquest to remote
                    btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
                    // send a message to send VSC command
                    btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP, &index,
                                       sizeof(uint8_t));
                    break;
                case BTIF_AVK_VSC_STOP:
                    // VSC Stop is already done, just ack back
                    btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_SUCCESS);
                    break;
                case BTIF_AVK_VSC_STOPPING:
                    /* VSC_STOP command has been sent, but ack did not came yet. 
                     * This can happen, if SUspend would have been initiated first.
                     * In this case let's wait for VSC Command ack
                     * */
                    break;
                default:
                    BTIF_TRACE_ERROR("%s  unexpected state %d ",__func__,btif_avk_cb[index].vsc_command_status);
                    break;
            }
            break;

        case BTIF_AVK_SPLIT_SINK_VSC_STOP_COMPLETE_EVT:
             BTIF_TRACE_DEBUG(" %s vsc_command_status %d",__FUNCTION__,
                             btif_avk_cb[index].vsc_command_status);
             if (btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STOP) {
                btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_SUCCESS);
             } else {
                btif_update_cmd_status(A2DP_AVK_CTRL_CMD_STOP_CAPTURE, A2DP_CTRL_ACK_FAILURE);
             }
             break;

        case BTIF_AVK_SPLIT_SINK_CHECK_A2DP_READY:
            BTIF_TRACE_DEBUG(" VSC command Status %d",btif_avk_cb[index].vsc_command_status);
            switch(btif_avk_cb[index].vsc_command_status) {
                case BTIF_AVK_VSC_STARTED:
                    btif_update_cmd_status(A2DP_AVK_CTRL_CMD_CHECK_READY,A2DP_CTRL_ACK_SUCCESS);
                    break;
                default:
                    btif_update_cmd_status(A2DP_AVK_CTRL_CMD_CHECK_READY,A2DP_CTRL_ACK_FAILURE);
                    break;
             }
             break;
        case BTIF_AVK_STOP_STREAM_REQ_EVT:
        case BTIF_AVK_SUSPEND_STREAM_REQ_EVT:
            btif_avk_cb[index].flags |= BTIF_AVK_FLAG_LOCAL_SUSPEND_PENDING;
            /* if we were remotely suspended but suspend locally, local suspend
               always overrides */
            btif_avk_cb[index].flags &= ~BTIF_AVK_FLAG_REMOTE_SUSPEND;
            //suspend request is coming from application.we need to check current vsc command status.
            BTIF_TRACE_EVENT("%s vsc status  %d",__FUNCTION__,btif_avk_cb[index].vsc_command_status);
            switch(btif_avk_cb[index].vsc_command_status) {
                case BTIF_AVK_VSC_STARTED:
                    // memorize this message, need to execute this again after sending VSC command.
                    memorize_msg(index,event,BTIF_AVK_STATE_STARTED, p_data);
                    // send stop command to soc and once confirmed, send suspend rquest to remote
                    btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
                    // send a message to send VSC command
                    btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP, &index,
                                       sizeof(uint8_t));
                    break;
                case BTIF_AVK_VSC_STOP:
                    // VSC Stop is already sent, send Suspend request to remote.
                    BTA_AvkStop(TRUE, btif_avk_cb[index].bta_handle);
                    break;
                case BTIF_AVK_VSC_STOPPING:
                    /* VSC_STOP command has been sent, but ack did not came yet. 
                     * This can happen, if STOP_CAPTURE came first and then SUSPEND_REQUEST came.
                     * In this case let's wait for VSC Command ack
                     * */
                    break;
            }
            break;
        case BTIF_AVK_SINK_START_STREAM_REQ_EVT:
            BTIF_TRACE_EVENT("BTIF_AVK_SINK_START_STREAM_REQ_EVT  already in started state");
            break;
        case BTIF_AVK_DISCONNECT_REQ_EVT:
            BTIF_TRACE_EVENT("%s vsc status  %d",__FUNCTION__,btif_avk_cb[index].vsc_command_status);
            if(btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
                // memorize this message, need to execute this again after sending VSC command.
                memorize_msg(index,event,BTIF_AVK_STATE_STARTED, p_data);
                // send stop command to soc and once confirmed, send suspend rquest to remote
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
                // send a message to send VSC command
                btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP, &index,
                                    sizeof(uint8_t));
                break;
            }

            /* request avdtp to close */
            BTA_AvkClose(btif_avk_cb[index].bta_handle);
            BTA_AvkCloseRc(btif_avk_cb[index].bta_handle);

            /* inform the application that we are disconnecting */
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTING, &(btif_avk_cb[index].peer_bda));

            /* wait in closing state until fully closed */
            btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_CLOSING);
            break;

        case BTA_AVK_SUSPEND_EVT:

            BTIF_TRACE_DEBUG("BTA_AVK_SUSPEND_EVT status %d, init %d",
                 p_av->suspend.status, p_av->suspend.initiator);
            if (p_av->suspend.initiator != TRUE)
            {
                /* remote suspend request,manage flags, inform bt-app, move to suspend_pending state
                 */
                if ((btif_avk_cb[index].flags & BTIF_AVK_FLAG_LOCAL_SUSPEND_PENDING) == 0) {
                    btif_avk_cb[index].flags |= BTIF_AVK_FLAG_REMOTE_SUSPEND;
                }
                btif_avk_cb[index].flags &= ~BTIF_AVK_FLAG_LOCAL_SUSPEND_PENDING;
                HAL_CBACK(bt_av_sink_vendor_callbacks, suspend_ind_cb, btif_avk_cb[index].peer_bda);
                alarm_set_on_mloop(avk_pending_suspend_timer,BTIF_AVK_PENDING_RESPONSE_TIMEOUT,
                                   btif_avk_pending_suspend_timeout_handler,
                                   (void *)&btif_avk_cb[index].peer_bda);
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_SUSPENDING);
                break;
            }
            else 
            {
                // confirmation of SUSPEND_REQUEST,VSC was send already.nothing to be done
                // just move to opened state.
            }
            /* if not successful, remain in current state */
            if (p_av->suspend.status != BTA_AVK_SUCCESS)
            {
                btif_avk_cb[index].flags &= ~BTIF_AVK_FLAG_LOCAL_SUSPEND_PENDING;
                return FALSE;
            }
            btif_report_audio_state(BTAV_AUDIO_STATE_REMOTE_SUSPEND, &(btif_avk_cb[index].peer_bda));
            btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_OPENED);

            /* suspend completed and state changed, clear pending status */
            btif_avk_cb[index].flags &= ~BTIF_AVK_FLAG_LOCAL_SUSPEND_PENDING;
            /* clear delay recode array when stream suspended */
            break;
        case BTA_AVK_STOP_EVT:
            BTIF_TRACE_EVENT("%s vsc status  %d",__FUNCTION__,btif_avk_cb[index].vsc_command_status);
            if(btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
                // memorize this message, need to execute this again after sending VSC command.
                memorize_msg(index,event,BTIF_AVK_STATE_STARTED, p_data);
                // send stop command to soc and once confirmed, send suspend rquest to remote
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
                // send a message to send VSC command
                btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP, &index,
                                    sizeof(uint8_t));
                break;
            }
            btif_avk_cb[index].flags |= BTIF_AVK_FLAG_PENDING_STOP;

            btif_report_audio_state(BTAV_AUDIO_STATE_STOPPED, &(btif_avk_cb[index].peer_bda));
            /* if stop was successful, change state to open */
            if (p_av->suspend.status == BTA_AVK_SUCCESS)
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_OPENED);

            break;

        case BTA_AVK_CLOSE_EVT:
            BTIF_TRACE_EVENT("%s vsc status  %d",__FUNCTION__,btif_avk_cb[index].vsc_command_status);
            if(btif_avk_cb[index].vsc_command_status == BTIF_AVK_VSC_STARTED) {
                // memorize this message, need to execute this again after sending VSC command.
                memorize_msg(index,event,BTIF_AVK_STATE_STARTED, p_data);
                // send stop command to soc and once confirmed, send suspend rquest to remote
                btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_HDL_VSC);
                // send a message to send VSC command
                btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP, &index,
                                    sizeof(uint8_t));
                break;
            }
             btif_avk_cb[index].flags |= BTIF_AVK_FLAG_PENDING_STOP;

            /* avdtp link is closed */
            APPL_TRACE_WARNING("Stop the AV Data channel");

            /* inform the application that we are disconnected */
            btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
                                        (bt_bdaddr_t *)&(btif_avk_cb[index].peer_bda));

            btif_sm_change_state(btif_avk_cb[index].sm_handle, BTIF_AVK_STATE_IDLE);
            break;
        CHECK_AVK_RC_EVENT(event, (tBTA_AVK*)p_data);

        default:
            BTIF_TRACE_WARNING("%s : unhandled event:%s", __FUNCTION__,
                                 dump_avk_sm_event_name((btif_avk_sm_event_t)event));
            return FALSE;

    }
    return TRUE;
}


static void btif_avk_event_deep_copy(uint16_t event, char *p_dest, char *p_src)
{
    tBTA_AVK *av_src = (tBTA_AVK *)p_src;
    tBTA_AVK *av_dest = (tBTA_AVK *)p_dest;

    // First copy the structure
    memcpy(p_dest, p_src, sizeof(tBTA_AVK));

    switch (event)
    {
        case BTA_AVK_META_MSG_EVT:
            if (av_src->meta_msg.p_data && av_src->meta_msg.len)
            {
                av_dest->meta_msg.p_data = (uint8_t *)osi_calloc(av_src->meta_msg.len);
                assert(av_dest->meta_msg.p_data);
                memcpy(av_dest->meta_msg.p_data, av_src->meta_msg.p_data, av_src->meta_msg.len);
            }

            if (av_src->meta_msg.p_msg)
            {
                av_dest->meta_msg.p_msg =(tAVRC_MSG *) osi_calloc(sizeof(tAVRC_MSG));
                assert(av_dest->meta_msg.p_msg);
                memcpy(av_dest->meta_msg.p_msg, av_src->meta_msg.p_msg, sizeof(tAVRC_MSG));

                if ((av_src->meta_msg.p_msg->hdr.opcode == AVRC_OP_VENDOR) &&
                    av_src->meta_msg.p_msg->vendor.p_vendor_data &&
                    av_src->meta_msg.p_msg->vendor.vendor_len)
                {
                    av_dest->meta_msg.p_msg->vendor.p_vendor_data = (uint8_t *)osi_calloc(
                        av_src->meta_msg.p_msg->vendor.vendor_len);
                    assert(av_dest->meta_msg.p_msg->vendor.p_vendor_data);
                    memcpy(av_dest->meta_msg.p_msg->vendor.p_vendor_data,
                        av_src->meta_msg.p_msg->vendor.p_vendor_data,
                        av_src->meta_msg.p_msg->vendor.vendor_len);
                }
            }
            break;
 /*       case BTA_AVK_BROWSE_MSG_EVT:
            if (av_src->browse_msg.p_msg)
            {
                av_dest->browse_msg.p_msg = osi_calloc(sizeof(tAVRC_MSG));
                assert(av_dest->browse_msg.p_msg);
                memcpy(av_dest->browse_msg.p_msg, av_src->browse_msg.p_msg, sizeof(tAVRC_MSG));

                if (av_src->browse_msg.p_msg->browse.p_browse_data &&
                    av_src->browse_msg.p_msg->browse.browse_len)
                {
                    av_dest->browse_msg.p_msg->browse.p_browse_data = osi_calloc(
                        av_src->browse_msg.p_msg->browse.browse_len);
                    assert(av_dest->browse_msg.p_msg->browse.p_browse_data);
                    memcpy(av_dest->browse_msg.p_msg->browse.p_browse_data,
                        av_src->browse_msg.p_msg->browse.p_browse_data,
                        av_src->browse_msg.p_msg->browse.browse_len);
                }
            }
            break;
*/
        default:
            break;
    }
}

static void btif_avk_event_free_data(btif_sm_event_t event, void *p_data)
{
    switch (event)
    {
        case BTA_AVK_META_MSG_EVT:
            {
                tBTA_AVK *av = (tBTA_AVK*)p_data;
                if (av->meta_msg.p_data)
                    osi_free(av->meta_msg.p_data);

                if (av->meta_msg.p_msg) {
                  if (av->meta_msg.p_msg->hdr.opcode == AVRC_OP_VENDOR) {
                    osi_free(av->meta_msg.p_msg->vendor.p_vendor_data);
                  }
                  osi_free_and_reset((void**)&av->meta_msg.p_msg);
                }

            }
            break;
  /*      case BTA_AVK_BROWSE_MSG_EVT:
            {
                tBTA_AVK *av = (tBTA_AVK*)p_data;

                if (av->browse_msg.p_msg)
                {
                {
                    if (av->browse_msg.p_msg->browse.p_browse_data)
                        osi_free(av->browse_msg.p_msg->browse.p_browse_data);
                    osi_free(av->browse_msg.p_msg);
                }
            }
            break;
*/
        default:
            break;
    }
}

/*****************************************************************************
**  Local event handlers
******************************************************************************/

static void btif_avk_handle_event(uint16_t event, char* p_param)
{
    int index = 0;
    tBTA_AVK *p_bta_data = (tBTA_AVK*)p_param;
    bt_bdaddr_t *bt_addr;
    bt_bdaddr_t *bt_addr1;
    uint8_t role;
    int uuid;
    btif_avk_config_req_t req;
    switch (event)
    {
        case BTIF_AVK_INIT_REQ_EVT:
            BTIF_TRACE_IMP("%s: BTIF_AVK_INIT_REQ_EVT", __FUNCTION__);
            btif_avk_split_a2dp_start_media_task();
            break;
        /*events from Upper layer and Media Task*/
        case BTIF_AVK_CLEANUP_REQ_EVT: /*Clean up to be called on default index*/
            BTIF_TRACE_IMP("%s: BTIF_AVK_CLEANUP_REQ_EVT", __FUNCTION__);
            bt_avk_callbacks = NULL;
            btif_avk_split_a2dp_stop_media_task();
            return;
        case BTIF_AVK_CONNECT_REQ_EVT:
            bt_addr = (bt_bdaddr_t *)p_param;
            index = btif_avk_idx_by_bdaddr(bt_addr);
            break;
        case BTIF_AVK_DISCONNECT_REQ_EVT:
            /*Bd address passed should help us in getting the handle*/
            bt_addr = (bt_bdaddr_t *)p_param;
            index = btif_avk_idx_by_bdaddr(bt_addr);
            break;
            // messages coming from mediatask
        case BTIF_AVK_SPLIT_SINK_START_CAPTURE:
        case BTIF_AVK_SPLIT_SINK_CHECK_A2DP_READY:
        case BTIF_AVK_START_STREAM_REQ_EVT:
        case BTIF_AVK_STOP_STREAM_REQ_EVT:
        case BTIF_AVK_SUSPEND_STREAM_REQ_EVT:
        case BTIF_AVK_SPLIT_SINK_SETUP_COMPLETE:
            index = btif_avk_idx_by_bdaddr(&streaming_bda);
            break;
            // for these messages we would be sending index
        case BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_START:
        case BTIF_AVK_SPLIT_SINK_SEND_VSC_A2DP_STOP:
        case BTIF_AVK_SPLIT_SINK_VSC_STOP_COMPLETE_EVT:
        case BTIF_AVK_SPLIT_SINK_VSC_START_COMPLETE_EVT:
        case BTIF_AVK_SPLIT_SINK_STOP_CAPTURE:
            index = *((uint8_t *)p_param);
            break;
        case BTIF_AVK_SPLIT_SINK_START_IND_RSP:
        case BTIF_AVK_SPLIT_SINK_SUSPEND_IND_RSP:
            index = p_bta_data->start_rsp.index;
          break;
        case BTA_AVK_OFFLOAD_START_RSP_EVT:
        case BTA_AVK_OFFLOAD_STOP_RSP_EVT:
          index = HANDLE_TO_INDEX(p_bta_data->offload_rsp.hndl);
          break;
        case BTIF_AVK_SPLIT_PENDING_START_ALARM_TIMEOUT:
        case BTIF_AVK_SPLIT_PENDING_SUSPEND_ALARM_TIMEOUT:
          index = btif_avk_idx_by_bdaddr((bt_bdaddr_t*)p_param);
          break;
        /*Events from the stack, BTA*/
        case BTA_AVK_ENABLE_EVT:
            index = 0;
            break;
        case BTA_AVK_REGISTER_EVT:
            index = HANDLE_TO_INDEX(p_bta_data->registr.hndl);
            break;
        case BTA_AVK_OPEN_EVT:
            index = HANDLE_TO_INDEX(p_bta_data->open.hndl);
            break;
        case BTA_AVK_MTU_CONFIG_EVT:
            index = HANDLE_TO_INDEX(p_bta_data->mtu_config.hndl);
            break;
        case BTA_AVK_ROLE_CHANGED_EVT:
            index = HANDLE_TO_INDEX(p_bta_data->role_changed.hndl);
            role = p_bta_data->role_changed.new_role;
            BTIF_TRACE_EVENT("Role change: 0x%x: new role: %s",
                p_bta_data->role_changed.hndl, (role == HOST_ROLE_SLAVE) ? "Slave" : "Master");
            if (index >= 0 && index < btif_max_avk_clients)
            {
                btif_avk_cb[index].is_slave = (role == HOST_ROLE_SLAVE) ? TRUE : FALSE;
            }
            else
            {
                BTIF_TRACE_ERROR("%s: Invalid index for connection", __FUNCTION__);
            }
            return;

        case BTA_AVK_PENDING_EVT:
            index = HANDLE_TO_INDEX(p_bta_data->pend.hndl);
            break;
        case BTA_AVK_REJECT_EVT:
            index = HANDLE_TO_INDEX(p_bta_data->reject.hndl);
            break;
        case BTA_AVK_STOP_EVT:
            index = HANDLE_TO_INDEX(p_bta_data->suspend.hndl);
            break;
        case BTA_AVK_CLOSE_EVT:
            index = HANDLE_TO_INDEX(p_bta_data->close.hndl);
            break;
        case BTA_AVK_START_EVT:
            index = HANDLE_TO_INDEX(p_bta_data->start.hndl);
            break;
        case BTA_AVK_RECONFIG_EVT:
            index = HANDLE_TO_INDEX(p_bta_data->reconfig.hndl);
            break;
        case BTA_AVK_SUSPEND_EVT:
            index = HANDLE_TO_INDEX(p_bta_data->suspend.hndl);
            break;

        /* Handle all RC events on default index. RC handling should take
         * care of the events. All events come with BD Address
         * Handled well in AV Opening, opened and started state
         * AV Idle handler needs to take care of this event properly.
         */
        case BTA_AVK_RC_OPEN_EVT:
            index = btif_avk_get_valid_idx_for_rc_events(p_bta_data->rc_open.peer_addr,
                    p_bta_data->rc_open.rc_handle);
            break;
        case BTA_AVK_RC_CLOSE_EVT:
        /* If there is no entry in the connection table
         * RC handler has to be called for cleanup.
         * Directly call the RC handler as we cannot
         * associate any AV handle to it.
         */
            index = btif_avk_idx_by_bdaddr(&p_bta_data->rc_open.peer_addr);
            if (index == btif_max_avk_clients)
            {
                btif_avk_rc_handler(event, (tBTA_AVK*)p_bta_data);
            }
            break;
        /* Let the RC handler decide on these passthrough cmds
         * Use rc_handle to get the active AV device and use that mapping.
         */
        case BTA_AVK_RC_BROWSE_CLOSE_EVT:
            index = btif_avk_idx_by_bdaddr(&p_bta_data->rc_browse_close.peer_addr);
            btif_avk_rc_handler(event, (tBTA_AVK*)p_bta_data);
            break;
        case BTA_AVK_REMOTE_CMD_EVT:
        case BTA_AVK_VENDOR_CMD_EVT:
        case BTA_AVK_META_MSG_EVT:
        case BTA_AVK_RC_FEAT_EVT:
        case BTA_AVK_BROWSE_MSG_EVT:
            index = 0;
            break;
        case BTIF_AVK_SINK_CONFIG_REQ_EVT:
            // copy to avoid alignment problems
            memcpy(&req, p_param, sizeof(req));
            index = btif_avk_idx_by_bdaddr(&(req.peer_bd));
            break;
        default:
            BTIF_TRACE_ERROR(" %s Unhandled AVK event = %d",__FUNCTION__ ,event);
            break;
    }
    BTIF_TRACE_DEBUG("Handle the AVK event = %x on index = %d", event, index);
    if (index >= 0 && index < btif_max_avk_clients)
        btif_sm_dispatch(btif_avk_cb[index].sm_handle, event, (void*)p_param);
    else
        BTIF_TRACE_ERROR("Unhandled Index = %d", index);
    btif_avk_event_free_data(event, p_param);

}

/*******************************************************************************
**
** Function         btif_avk_is_device_connected_connecting
**
** Description      Check the validity of the current index for the connection
**
** Returns          bool
**
*******************************************************************************/

static bool btif_avk_is_device_connected_connecting(int idx)
{
    btif_sm_state_t state = btif_sm_get_state(btif_avk_cb[idx].sm_handle);
    return ((state == BTIF_AVK_STATE_OPENED) ||
            (state ==  BTIF_AVK_STATE_STARTED) ||
            (state == BTIF_AVK_STATE_OPENING));
}

/*******************************************************************************
**
** Function         btif_avk_idx_by_bdaddr
**
** Description      Get the index corresponding to BD addr
**
** Returns          UNIT8
**
*******************************************************************************/

static uint8_t btif_avk_idx_by_bdaddr(RawAddress * bd_addr)
{
    int i;
    for (i = 0; i < btif_max_avk_clients; i++)
    {
        if (*bd_addr == btif_avk_cb[i].peer_bda)
            return i;
    }
    return i;
}

/*******************************************************************************
**
** Function         btif_get_conn_state_of_device
**
** Description      Returns the state of AV scb
**
** Returns          int
**
*******************************************************************************/

static int btif_get_conn_state_of_device(RawAddress address)
{
    btif_sm_state_t state = BTIF_AVK_STATE_IDLE;
    int i;
    for (i = 0; i < btif_max_avk_clients; i++)
    {
        if (address == btif_avk_cb[i].peer_bda)
        {
            state = btif_sm_get_state(btif_avk_cb[i].sm_handle);
            BTIF_TRACE_EVENT("BD Found: %s :state: %s", address.ToString().c_str(), dump_avk_sm_state_name((btif_avk_state_t)state));
        }
    }
    return (btif_avk_state_t)state;
}

/*******************************************************************************
**
** Function         btif_avk_get_valid_idx_for_rc_events
**
** Description      gets th valid index for the RC event address
**
** Returns          int
**
*******************************************************************************/

static int btif_avk_get_valid_idx_for_rc_events(RawAddress bd_addr, int rc_handle)
{
    int index = 0;
    /* First try to find if it is first event in AV IF
    * both the handles would be in IDLE state, pick the first
    * If we get second RC event while processing the priority
    * for the first, reject the second connection. */

    /*Get the index from connected SCBs*/
    index = btif_avk_idx_by_bdaddr(&bd_addr);
    if (index == btif_max_avk_clients)
    {
        /* None of the SCBS matched
        * Allocate free SCB, null address SCB*/
        RawAddress empty_addr = RawAddress::kEmpty;
        index = btif_avk_idx_by_bdaddr(&empty_addr);
        BTIF_TRACE_EVENT("btif_avk_get_valid_idx_for_rc_events is %d", index);
        if (index >= btif_max_avk_clients)
        {
            BTIF_TRACE_EVENT("disconnect only AVRCP device rc_handle %d", rc_handle);
            BTA_AvkCloseRc(rc_handle);
        }
    }
    return index;
}

static void bte_avk_callback(tBTA_AVK_EVT event, tBTA_AVK *p_data)
{
    btif_transfer_context(btif_avk_handle_event, event,
                          (char*)p_data, sizeof(tBTA_AVK), btif_avk_event_deep_copy);
}

/*Called only in case of A2dp SInk*/
static void bte_avk_media_callback(tBTA_AVK_EVT event, tBTA_AVK_MEDIA *p_data, RawAddress bd_addr)
{
    btif_sm_state_t state;
    uint8_t que_len;
    tA2DP_STATUS a2d_status;
    tA2D_SBC_CIE sbc_cie;
    uint8_t* start_ptr;
    uint16_t data_len;
    BT_HDR* p_pkt;
    uint8_t rtp_offset = 0;
#if defined(AAC_DECODER_INCLUDED) && (AAC_DECODER_INCLUDED == TRUE)
    tA2D_AAC_CIE aac_cie;
#endif
#if defined(MP3_DECODER_INCLUDED) && (MP3_DECODER_INCLUDED == TRUE)
    tA2D_MP3_CIE mp3_cie;
#endif
#if defined(APTX_CLASSIC_DECODER_INCLUDED) && (APTX_CLASSIC_DECODER_INCLUDED == TRUE)
    tA2D_APTX_CIE aptx_cie;
#endif
#if defined(APTX_AD_DECODER_INCLUDED) && (APTX_AD_DECODER_INCLUDED == TRUE)
    tA2DP_AVK_APTX_AD_CIE aptx_ad_cie;
#endif
    btif_avk_config_req_t config_req;
    int index = btif_avk_idx_by_bdaddr(&bd_addr);
    if (index >= btif_max_avk_clients)
    {
        BTIF_TRACE_DEBUG("%s Invalid index for device", __FUNCTION__);
        return;
    }

    if (event == BTA_AVK_MEDIA_SINK_CFG_EVT) {
        uint8_t* config = (uint8_t*)(p_data->avk_config.codec_info);
        uint8_t codec_type = config[2];
        /* send a command to BT Media Task */
        //memcpy(config_req.codec_info,(uint8_t*)(p_data->avk_config.codec_info), AVDT_CODEC_SIZE);
        config_req.codec_type = codec_type;
        btif_avk_cb[index].sink_codec_type = codec_type;
        switch(codec_type)
        {
        case BTIF_AVK_CODEC_SBC:
            BTIF_TRACE_DEBUG("delay has been inited as :%d", DEFAULT_RENDERING_DELAY);
            btif_avk_cb[index].delay = DEFAULT_RENDERING_DELAY;

            a2d_status = A2D_ParsSbcInfo(&sbc_cie, (uint8_t *)(p_data->avk_config.codec_info), FALSE);
            if (a2d_status == A2DP_SUCCESS) {
                /* Switch to BTIF context */
                config_req.sample_rate = btif_avk_a2dp_get_sbc_track_frequency(sbc_cie.samp_freq);
                config_req.channel_count = btif_avk_a2dp_get_sbc_track_channel_count(sbc_cie.ch_mode);
                config_req.codec_info.sbc_config.samp_freq = sbc_cie.samp_freq;
                config_req.codec_info.sbc_config.ch_mode = sbc_cie.ch_mode;
                config_req.codec_info.sbc_config.block_len = sbc_cie.block_len;
                config_req.codec_info.sbc_config.alloc_mthd = sbc_cie.alloc_mthd;
                config_req.codec_info.sbc_config.max_bitpool = sbc_cie.max_bitpool;
                config_req.codec_info.sbc_config.min_bitpool = sbc_cie.min_bitpool;
                config_req.codec_info.sbc_config.num_subbands = sbc_cie.num_subbands;
                config_req.peer_bd = p_data->avk_config.bd_addr;
                btif_transfer_context(btif_avk_handle_event, BTIF_AVK_SINK_CONFIG_REQ_EVT,
                                         (char*)&config_req, sizeof(config_req), NULL);
            }
            else
            {
                APPL_TRACE_ERROR("ERROR dump_codec_info A2D_ParsSbcInfo fail:%d", a2d_status);
            }
            break;

#if defined(AAC_DECODER_INCLUDED) && (AAC_DECODER_INCLUDED == TRUE)
        case BTA_AVK_CODEC_M24:
            BTIF_TRACE_DEBUG("delay has been inited as :%d", AAC_RENDERING_DELAY);
            btif_avk_cb[index].delay = AAC_RENDERING_DELAY;

            a2d_status = A2D_ParsAacInfo(&aac_cie, (uint8_t *)(p_data->avk_config.codec_info), FALSE);
            if (a2d_status == A2DP_SUCCESS) {
                /* Switch to BTIF context */
                config_req.sample_rate = btif_avk_a2dp_get_aac_track_frequency(aac_cie.samp_freq);
                config_req.channel_count = btif_avk_a2dp_get_aac_track_channel_count(aac_cie.channels);
                config_req.codec_info.aac_config.bit_rate = aac_cie.bit_rate;
                config_req.codec_info.aac_config.sampling_freq = aac_cie.samp_freq;
                config_req.codec_info.aac_config.obj_type = aac_cie.object_type;
                config_req.codec_info.aac_config.channel_count = aac_cie.channels;
                config_req.codec_info.aac_config.vbr = aac_cie.vbr;
                config_req.peer_bd = p_data->avk_config.bd_addr;
                btif_transfer_context(btif_avk_handle_event, BTIF_AVK_SINK_CONFIG_REQ_EVT,
                                     (char*)&config_req, sizeof(config_req), NULL);
            } else {
                APPL_TRACE_ERROR("ERROR dump_codec_info A2D_ParsAacInfo fail:%d", a2d_status);
            }
            break;
#endif
#if defined(MP3_DECODER_INCLUDED) && (MP3_DECODER_INCLUDED == TRUE)
        case BTA_AVK_CODEC_M12:
            BTIF_TRACE_DEBUG("delay has been inited as :%d", DEFAULT_RENDERING_DELAY);
            btif_avk_cb[index].delay = DEFAULT_RENDERING_DELAY;

            a2d_status = A2D_ParsMp3Info(&mp3_cie, (uint8_t *)(p_data->avk_config.codec_info), FALSE);
            if (a2d_status == A2DP_SUCCESS) {
                /* Switch to BTIF context */
                config_req.sample_rate = btif_avk_a2dp_get_mp3_track_frequency(mp3_cie.samp_freq);
                config_req.channel_count = btif_avk_a2dp_get_mp3_track_channel_count(mp3_cie.channels);
                config_req.codec_info.mp3_config.bit_rate = mp3_cie.bit_rate;
                config_req.codec_info.mp3_config.sampling_freq = mp3_cie.samp_freq;
                config_req.codec_info.mp3_config.layer = mp3_cie.layer;
                config_req.codec_info.mp3_config.channel_count = mp3_cie.channels;
                config_req.codec_info.mp3_config.vbr = mp3_cie.vbr;
                config_req.codec_info.mp3_config.mpf = mp3_cie.mpf;
                config_req.codec_info.mp3_config.crc = mp3_cie.crc;
                config_req.peer_bd = p_data->avk_config.bd_addr;
                btif_transfer_context(btif_avk_handle_event, BTIF_AVK_SINK_CONFIG_REQ_EVT,
                                     (char*)&config_req, sizeof(config_req), NULL);
            } else {
                APPL_TRACE_ERROR("ERROR dump_codec_info A2D_ParsMp3Info fail:%d", a2d_status);
            }
            break;
#endif
        case A2D_NON_A2DP_MEDIA_CT:
        /* p_data->avk_config.codec_info  can tell us about APTX-CL or APTX-AD */
#if defined(APTX_CLASSIC_DECODER_INCLUDED) && (APTX_CLASSIC_DECODER_INCLUDED == TRUE)
        if ((get_codec_id(p_data->avk_config.codec_info) == A2D_APTX_CODEC_ID_BLUETOOTH) &&
           (get_vendor_id(p_data->avk_config.codec_info) == A2D_APTX_VENDOR_ID))
        {
                BTIF_TRACE_DEBUG("delay has been inited as :%d", APTX_RENDERING_DELAY);
                btif_avk_cb[index].delay = APTX_RENDERING_DELAY;

                a2d_status = A2D_ParsAptxInfo(&aptx_cie, (uint8_t *)(p_data->avk_config.codec_info), FALSE);
                if (a2d_status == A2DP_SUCCESS) {
                    /* Switch to BTIF context */
                    /* setting APTX for Vendor specific codec right now */
                    config_req.codec_type = A2DP_SINK_AUDIO_CODEC_APTX;
                    config_req.sample_rate = btif_avk_a2dp_get_aptx_track_frequency(aptx_cie.sampleRate);
                    config_req.channel_count = btif_avk_a2dp_get_aptx_track_channel_count(aptx_cie.channelMode);
                    config_req.codec_info.aptx_config.vendor_id = aptx_cie.vendorId;
                    config_req.codec_info.aptx_config.codec_id = aptx_cie.codecId;
                    config_req.codec_info.aptx_config.sampling_freq = aptx_cie.sampleRate;
                    config_req.codec_info.aptx_config.channel_count = aptx_cie.channelMode;
                    config_req.peer_bd = p_data->avk_config.bd_addr;
                    btif_transfer_context(btif_avk_handle_event, BTIF_AVK_SINK_CONFIG_REQ_EVT,
                                        (char*)&config_req, sizeof(config_req), NULL);
                } else {
                    APPL_TRACE_ERROR("ERROR dump_codec_info A2D_ParsAptxInfo fail:%d", a2d_status);
                }
        }
#endif
#if defined(APTX_AD_DECODER_INCLUDED) && (APTX_AD_DECODER_INCLUDED == TRUE)
        if ((get_codec_id(p_data->avk_config.codec_info) == A2DP_AVK_APTX_AD_CODEC_ID_BLUETOOTH) &&
           (get_vendor_id(p_data->avk_config.codec_info) == A2DP_AVK_APTX_AD_VENDOR_ID))
        {
                BTIF_TRACE_DEBUG("delay has been inited as :%d", APTX_AD_RENDERING_DELAY);
                btif_avk_cb[index].delay = APTX_AD_RENDERING_DELAY;

                a2d_status = A2DP_ParseInfoAptxAdaptiveDecoder(&aptx_ad_cie,
                                (uint8_t *)(p_data->avk_config.codec_info), FALSE);
                if (a2d_status == A2DP_SUCCESS) {
                    /* Switch to BTIF context */
                    /* setting APTX for Vendor specific codec right now */
                    config_req.codec_type = A2DP_SINK_AUDIO_CODEC_APTX_AD;
                    config_req.sample_rate = btif_avk_a2dp_get_aptx_ad_track_frequency(aptx_ad_cie.sampleRate);
                    config_req.channel_count = btif_avk_a2dp_get_aptx_ad_track_channel_count(aptx_ad_cie.channelMode);
                    config_req.codec_info.aptx_ad_config.vendor_id = aptx_ad_cie.vendorId;
                    config_req.codec_info.aptx_ad_config.codec_id = aptx_ad_cie.codecId;
                    config_req.codec_info.aptx_ad_config.sampling_freq = aptx_ad_cie.sampleRate;
                    config_req.codec_info.aptx_ad_config.channel_count = aptx_ad_cie.channelMode;
                    config_req.peer_bd = p_data->avk_config.bd_addr;
                    btif_transfer_context(btif_avk_handle_event, BTIF_AVK_SINK_CONFIG_REQ_EVT,
                                        (char*)&config_req, sizeof(config_req), NULL);
                } else {
                    APPL_TRACE_ERROR("ERROR dump_codec_info A2D_AD_ParsAptxInfo fail:%d", a2d_status);
                }
        }
#endif
            break;

            default:
            BTIF_TRACE_DEBUG("delay has been inited as :%d", DEFAULT_RENDERING_DELAY);
            btif_avk_cb[index].delay = DEFAULT_RENDERING_DELAY;
            break;
        }
    }
}


/*******************************************************************************
**
** Function         btif_avk_split_init
**
** Description      Initializes btif AV if not already done
**
** Returns          bt_status_t
**
*******************************************************************************/

bt_status_t btif_avk_split_init(int service_id)
{
    int i;
    if (btif_avk_cb[0].sm_handle == NULL)
    {
        BTIF_TRACE_IMP("%s", __FUNCTION__);
        alarm_free(avk_open_on_rc_timer);
        avk_open_on_rc_timer = alarm_new("btif_av.avk_open_on_rc_timer");
        alarm_free(avk_pending_start_timer);
        avk_pending_start_timer = alarm_new("btif_av.avk_pending_start_timer");
        alarm_free(avk_pending_suspend_timer);
        avk_pending_suspend_timer = alarm_new("btif_av.avk_pending_suspend_timer");
        if(!btif_avk_split_a2dp_is_media_task_stopped())
            return BT_STATUS_FAIL;

        /* Also initialize the AV state machine */
        for (i = 0; i < btif_max_avk_clients; i++)
        {
            btif_avk_cb[i].sm_handle = btif_sm_init((const btif_sm_handler_t*)btif_avk_state_handlers,
                                                    BTIF_AVK_STATE_IDLE, i);
            btif_avk_cb[i].prev_state = BTIF_AVK_STATE_IDLE;
        }

        btif_transfer_context(btif_avk_handle_event, BTIF_AVK_INIT_REQ_EVT,
                (char*)&service_id, sizeof(int), NULL);

        btif_enable_service(service_id);
    }

    return BT_STATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         init_sink
**
** Description      Initializes the AV interface for sink mode
**
** Returns          bt_status_t
**
*******************************************************************************/

static bt_status_t init_sink(btav_sink_callbacks_t* callbacks)
{
    bt_status_t status;

    BTIF_TRACE_EVENT("%s", __FUNCTION__);

    status = BT_STATUS_SUCCESS;

    if (status == BT_STATUS_SUCCESS) {
        bt_avk_callbacks = callbacks;
    }
    pthread_mutex_init(&split_sink_codec_q_lock, NULL);
    pthread_mutex_lock(&split_sink_codec_q_lock);
    if (p_bta_avk_codec_pri_list == NULL) {
        int i = 0;
        bta_avk_num_codec_configs = BTIF_SV_AVK_AA_SEP_INDEX;
        p_bta_avk_codec_pri_list = (tBTA_AVK_CO_CODEC_CAP_LIST*)osi_calloc(bta_avk_num_codec_configs *
            sizeof(tBTA_AVK_CO_CODEC_CAP_LIST));
        if (p_bta_avk_codec_pri_list != NULL) {
            /* Set default priorty order as APTX-AD > APTX-CL > AAC > MP3 > SBC */
#if defined(APTX_AD_DECODER_INCLUDED) && (APTX_AD_DECODER_INCLUDED == TRUE)
            p_bta_avk_codec_pri_list[i].codec_type = A2D_NON_A2DP_MEDIA_CT;
            p_bta_avk_codec_pri_list[i].codec_id = A2DP_AVK_APTX_AD_CODEC_ID_BLUETOOTH;
            p_bta_avk_codec_pri_list[i].vendor_id = A2DP_AVK_APTX_AD_VENDOR_ID;
            memcpy(&p_bta_avk_codec_pri_list[i++].codec_cap.aptx_caps,
                &bta_avk_co_aptx_ad_caps, sizeof(tA2D_APTX_CIE));
            memcpy(&bta_avk_supp_codec_cap[BTIF_SV_AVK_AA_APTX_AD_INDEX]
                .codec_cap.aptx_caps, &bta_avk_co_aptx_ad_caps, sizeof(tA2DP_AVK_APTX_AD_CIE));
#endif
#if defined(APTX_CLASSIC_DECODER_INCLUDED) && (APTX_CLASSIC_DECODER_INCLUDED == TRUE)
            p_bta_avk_codec_pri_list[i].codec_type = A2D_NON_A2DP_MEDIA_CT;
            p_bta_avk_codec_pri_list[i].codec_id = A2D_APTX_CODEC_ID_BLUETOOTH;
            p_bta_avk_codec_pri_list[i].vendor_id = A2D_APTX_VENDOR_ID;
            memcpy(&p_bta_avk_codec_pri_list[i++].codec_cap.aptx_caps,
                &bta_avk_co_aptx_caps, sizeof(tA2D_APTX_CIE));
            memcpy(&bta_avk_supp_codec_cap[BTIF_SV_AVK_AA_APTX_INDEX]
                .codec_cap.aptx_caps, &bta_avk_co_aptx_caps, sizeof(tA2D_APTX_CIE));
#endif
#if defined(AAC_DECODER_INCLUDED) && (AAC_DECODER_INCLUDED == TRUE)
            p_bta_avk_codec_pri_list[i].codec_type = A2D_MEDIA_CT_M24;
            memcpy(&p_bta_avk_codec_pri_list[i++].codec_cap.aac_caps,
                &bta_avk_co_aac_caps, sizeof(tA2D_AAC_CIE));
            memcpy(&bta_avk_supp_codec_cap[BTIF_SV_AVK_AA_AAC_INDEX]
                .codec_cap.aac_caps, &bta_avk_co_aac_caps, sizeof(tA2D_AAC_CIE));
#endif
#if defined(MP3_DECODER_INCLUDED) && (MP3_DECODER_INCLUDED == TRUE)
            p_bta_avk_codec_pri_list[i].codec_type = A2D_MEDIA_CT_M12;
            memcpy(&p_bta_avk_codec_pri_list[i++].codec_cap.mp3_caps,
                &bta_avk_co_mp3_caps, sizeof(tA2D_MP3_CIE));
            memcpy(&bta_avk_supp_codec_cap[BTIF_SV_AVK_AA_MP3_INDEX]
                .codec_cap.mp3_caps, &bta_avk_co_mp3_caps, sizeof(tA2D_MP3_CIE));
#endif
            p_bta_avk_codec_pri_list[i].codec_type = A2D_MEDIA_CT_SBC;
            memcpy(&p_bta_avk_codec_pri_list[i++].codec_cap.sbc_caps,
                &bta_avk_co_sbc_caps, sizeof(tA2D_SBC_CIE));
            memcpy(&bta_avk_supp_codec_cap[BTIF_SV_AVK_AA_SBC_INDEX]
                .codec_cap.sbc_caps, &bta_avk_co_sbc_caps, sizeof(tA2D_SBC_CIE));
        }
        BTIF_TRACE_DEBUG(" %s: initialized codec list with default %d number of codecs",
            __FUNCTION__, i + 1);
    }
    streaming_bda = RawAddress::kEmpty;
    pthread_mutex_unlock(&split_sink_codec_q_lock);

    return status;
}

/*******************************************************************************
**
** Function         get_a2dp_sink_streaming_data
**
** Description      get a2dp sink data stored from Data Q
**
** Returns          number of bytes returned
**
*******************************************************************************/
static uint32_t get_a2dp_sink_streaming_data_vendor (uint16_t codec_type, uint8_t* data, uint32_t size)
{
    BTIF_TRACE_DEBUG(" %s dummy implementation for split ",__func__);
    return 0;
}

/*******************************************************************************
**
** Function         init_sink_vendor
**
** Description      Initializes the AV interface for sink vendor mode
**
** Returns          bt_status_t
**
*******************************************************************************/
static bt_status_t init_sink_vendor(btav_sink_vendor_callbacks_t* callbacks, int max,
                             int a2dp_multicast_state, uint8_t streaming_prarm)
{
    bt_status_t status = BT_STATUS_FAIL;
    char value[PROPERTY_VALUE_MAX] = {'\0'};

    BTIF_TRACE_IMP("%s max = %d", __FUNCTION__, max);

    btif_max_avk_clients = max;
    if (bt_avk_callbacks != NULL)
        status = btif_avk_split_init(BTA_A2DP_SPLIT_SINK_SERVICE_ID);


    if (status == BT_STATUS_SUCCESS) {
        bt_av_sink_vendor_callbacks = callbacks;
    }

    property_get("persist.bluetooth.disabledelayreports", value, "false");
    enable_delay_reporting = (strcmp(value, "false") == 0);
    property_get("persist.bluetooth.disablebrowsing", value, "false");
    enable_avrc_browsing = (strcmp(value, "false") == 0);

    BTIF_TRACE_IMP(" %s enable_delay_reporting = %d, enable_avrc_browsing = %d",
                   __FUNCTION__,enable_delay_reporting, enable_avrc_browsing);

    return status;
}


static bt_status_t start_ind_rsp(const RawAddress& bd_addr, int accepted) {
    tBTA_AVK_START_RSP cmd;
    BTIF_TRACE_DEBUG("%s accepted  = %d for device =%s ", __FUNCTION__, accepted,
                     bd_addr.ToString().c_str());
    if (accepted == CMD_ACCEPTED) {
        streaming_bda = bd_addr;
        // nothing to do, wait for MM-Audio Session establishment
    } else if (accepted == CMD_REJECTED) {
        // send cmd reject :BTIF_AVK_SPLIT_SINK_START_IND_RSP
        cmd.index = btif_avk_idx_by_bdaddr(&bd_addr);
        cmd.accepted = accepted;
        btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_START_IND_RSP, &cmd,
                                  sizeof(tBTA_AVK_START_RSP));
    }
    if(!streaming_bda.IsEmpty()) {
        BTIF_TRACE_DEBUG(" %s streaming bda %s ", __FUNCTION__,
                         streaming_bda.ToString().c_str());
    }
    return BT_STATUS_SUCCESS;
}

static bt_status_t suspend_ind_rsp (const RawAddress& bd_addr, int accepted) {
    tBTA_AVK_SUSPEND_RSP cmd;
    BTIF_TRACE_DEBUG("%s accepted  = %d for device =%s ", __FUNCTION__, accepted,
                     bd_addr.ToString().c_str());
    if (accepted == CMD_ACCEPTED) {
        // nothing to do, wait for MM-Audio Session tear down
    } else if(accepted == CMD_REJECTED) {
       // send cmd reject :BTIF_AVK_SPLIT_SINK_SUSPEND_IND_RSP
       cmd.index = btif_avk_idx_by_bdaddr(&bd_addr);
       cmd.accepted = accepted;
       btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SUSPEND_IND_RSP, &cmd,
                                  sizeof(tBTA_AVK_SUSPEND_RSP));
    }
    if(!streaming_bda.IsEmpty()) {
        BTIF_TRACE_DEBUG(" %s streaming bda %s ", __FUNCTION__,
                         streaming_bda.ToString().c_str());
    }
    return BT_STATUS_SUCCESS;
}

static bt_status_t start_req(const RawAddress& bd_addr) {
    BTIF_TRACE_DEBUG("%s for device  str_bda %s", __FUNCTION__,bd_addr.ToString().c_str());
    // start request from app. First check if other index is in started state.
    uint8_t index = btif_avk_idx_by_bdaddr(&bd_addr);

    if(index >= btif_max_avk_clients || (!btif_avk_is_device_connected_connecting(index))) {
        BTIF_TRACE_DEBUG(" %s invalid index ",__func__);
        return BT_STATUS_PARM_INVALID;
    }

    if (btif_avk_is_playing_on_other_idx(index)) {
        BTIF_TRACE_DEBUG(" %s streaming on other index ",__func__);
        return BT_STATUS_BUSY;
    }

    streaming_bda = bd_addr;
    btif_transfer_context(btif_avk_handle_event, BTIF_AVK_START_STREAM_REQ_EVT,
                                 NULL, 0, NULL);

    if(!streaming_bda.IsEmpty()) {
        BTIF_TRACE_DEBUG(" %s streaming bda %s ", __FUNCTION__,
                         streaming_bda.ToString().c_str());
    }

    return BT_STATUS_SUCCESS;
}

static bt_status_t suspend_req(const RawAddress& bd_addr) {
    BTIF_TRACE_DEBUG("%s for device  str_bda %s", __FUNCTION__,bd_addr.ToString().c_str());
    // start request from app. First check if other index is in started state.
    uint8_t index = btif_avk_idx_by_bdaddr(&bd_addr);
    if(index >= btif_max_avk_clients) {
        BTIF_TRACE_DEBUG(" %s invalid index ",__func__);
        return BT_STATUS_PARM_INVALID;
    }
    btif_transfer_context(btif_avk_handle_event, BTIF_AVK_SUSPEND_STREAM_REQ_EVT,
                                 (char*)&index, sizeof(uint8_t), NULL);

    if(!streaming_bda.IsEmpty()) {
        BTIF_TRACE_DEBUG(" %s streaming bda %s ", __FUNCTION__,
                         streaming_bda.ToString().c_str());
    }

    return BT_STATUS_SUCCESS;
}



/*******************************************************************************
**
** Function         update_streaming_device_vendor
**
** Description      Updates the current streaming device from apps
**
** Returns          void
**
*******************************************************************************/
static void update_streaming_device_vendor(bt_bdaddr_t *bd_addr)
{
    BTIF_TRACE_DEBUG(" %s ", __FUNCTION__);
    streaming_bda = *bd_addr;
    BTIF_TRACE_DEBUG(" %s streaming bda %s ", __FUNCTION__, streaming_bda.ToString().c_str());
}

/*******************************************************************************
**
** Function         update_qahw_delay_vendor
**
** Description      Updates decoding delay during decoding non_SBC stream in LPASS from apps
**
** Returns          void
**
*******************************************************************************/
static void update_qahw_delay_vendor(uint16_t qahwdelay)
{
    uint8_t index = btif_max_avk_clients;
    BTIF_TRACE_DEBUG(" %s delay: %d ", __FUNCTION__,qahwdelay);
    for(int i = 0;i < btif_max_avk_clients; i++) {
        btif_avk_cb[i].qahw_delay = qahwdelay;
    }
    if(btif_avk_split_get_started_device_idx(&index) && btif_avk_cb[index].avdt_sync) {
        /*This is to handle special scenarios,
          where qahw delay is updated after started state.*/
        BTA_AvkUpdateDelayReport(btif_avk_cb[index].bta_handle);
    }
    return;
}

static void is_value_to_be_updated(void *ptr1, void *ptr2, uint8_t num_of_bytes)
{
    uint8_t *p1 = (uint8_t*)ptr1;
    uint8_t *p2 = (uint8_t*)ptr2;
    int i;

    if (p1 == NULL || p2 == NULL)
        return;
    for (i = 0; i < num_of_bytes; i ++) {
        if (!(*p1 & *p2)) {
            *p1 |= *p2;
        }
        p1 ++;
        p2 ++;
        if (p1 == NULL || p2 == NULL)
            return;
   }
}

/*******************************************************************************
**
** Function         update_supported_codecs_param
**
** Description      Updates the codecs supported by Sink as requested by Application Layer
**
** Returns          bt_status_t
**
*******************************************************************************/
static bt_status_t update_supported_codecs_param_vendor(btav_codec_configuration_t
        *p_codec_config_list, uint8_t num_codec_configs)
{
    int i, j;

    if (num_codec_configs == 0 || num_codec_configs > MAX_NUM_CODEC_CONFIGS) {
        BTIF_TRACE_ERROR(" %s Invalid num_codec_configs = %d",
            __func__, num_codec_configs);
        return BT_STATUS_PARM_INVALID;
    }

    if (!p_codec_config_list) {
        BTIF_TRACE_ERROR(" %s codec list is NULL", __func__);
        return BT_STATUS_PARM_INVALID;
    }

    // Check if the codec params sent by upper layers are valid or not.
    for (i = 0; i < num_codec_configs; i ++) {
        switch (p_codec_config_list[i].codec_type) {
            case A2DP_SINK_AUDIO_CODEC_SBC:
                switch (p_codec_config_list[i].codec_config.sbc_config.samp_freq) {
                    case SBC_SAMP_FREQ_16:
                    case SBC_SAMP_FREQ_32:
                    case SBC_SAMP_FREQ_44:
                    case SBC_SAMP_FREQ_48:
                        break;
                    default:
                        BTIF_TRACE_ERROR(" %s Invalid SBC freq = %d",
                            __func__, p_codec_config_list[i].codec_config.sbc_config.samp_freq);
                        return BT_STATUS_PARM_INVALID;
                }
                break;
#if defined(AAC_DECODER_INCLUDED) && (AAC_DECODER_INCLUDED == TRUE)
            case A2DP_SINK_AUDIO_CODEC_AAC:
                switch (p_codec_config_list[i].codec_config.aac_config.sampling_freq) {
                    case AAC_SAMP_FREQ_8000:
                    case AAC_SAMP_FREQ_11025:
                    case AAC_SAMP_FREQ_12000:
                    case AAC_SAMP_FREQ_16000:
                    case AAC_SAMP_FREQ_22050:
                    case AAC_SAMP_FREQ_24000:
                    case AAC_SAMP_FREQ_32000:
                    case AAC_SAMP_FREQ_44100:
                    case AAC_SAMP_FREQ_48000:
                    case AAC_SAMP_FREQ_64000:
                    case AAC_SAMP_FREQ_88200:
                    case AAC_SAMP_FREQ_96000:
                        break;
                    default:
                        BTIF_TRACE_ERROR(" %s Invalid AAC freq = %d",
                            __func__, p_codec_config_list[i].codec_config.aac_config.sampling_freq);
                        return BT_STATUS_PARM_INVALID;
                }
                switch (p_codec_config_list[i].codec_config.aac_config.obj_type) {
                    case AAC_OBJ_TYPE_MPEG_2_AAC_LC:
                    case AAC_OBJ_TYPE_MPEG_4_AAC_LC:
                        break;
                    case AAC_OBJ_TYPE_MPEG_4_AAC_LTP:
                    case AAC_OBJ_TYPE_MPEG_4_AAC_SCA:
                        BTIF_TRACE_ERROR(" %s AAC Object Type = %d currently not supported",
                            __func__, p_codec_config_list[i].codec_config.aac_config.obj_type);
                        return BT_STATUS_UNSUPPORTED;
                    default:
                        BTIF_TRACE_ERROR(" %s Invalid AAC Object Type = %d",
                            __func__, p_codec_config_list[i].codec_config.aac_config.obj_type);
                        return BT_STATUS_PARM_INVALID;
                }
                break;
#endif
#if defined(MP3_DECODER_INCLUDED) && (MP3_DECODER_INCLUDED == TRUE)
            case A2DP_SINK_AUDIO_CODEC_MP3:
                switch (p_codec_config_list[i].codec_config.mp3_config.sampling_freq) {
                    case MP3_SAMP_FREQ_16000:
                    case MP3_SAMP_FREQ_22050:
                    case MP3_SAMP_FREQ_24000:
                    case MP3_SAMP_FREQ_32000:
                    case MP3_SAMP_FREQ_44100:
                    case MP3_SAMP_FREQ_48000:
                        break;
                    default:
                        BTIF_TRACE_ERROR(" %s Invalid MP3 freq = %d",
                            __func__, p_codec_config_list[i].codec_config.mp3_config.sampling_freq);
                        return BT_STATUS_PARM_INVALID;
                }
                switch (p_codec_config_list[i].codec_config.mp3_config.layer) {
                    case MP3_LAYER_3:
                        break;
                    case MP3_LAYER_1:
                    case MP3_LAYER_2:
                        BTIF_TRACE_ERROR(" %s MP3 layer = %d currently not supported",
                            __func__, p_codec_config_list[i].codec_config.mp3_config.layer);
                        return BT_STATUS_UNSUPPORTED;
                    default:
                        BTIF_TRACE_ERROR(" %s Invalid MP3 layer = %d",
                            __func__, p_codec_config_list[i].codec_config.mp3_config.layer);
                        return BT_STATUS_PARM_INVALID;
                }
                break;
#endif
#if defined(APTX_CLASSIC_DECODER_INCLUDED) && (APTX_CLASSIC_DECODER_INCLUDED == TRUE)
            case A2DP_SINK_AUDIO_CODEC_APTX:
                switch (p_codec_config_list[i].codec_config.aptx_config.sampling_freq) {
                    case APTX_SAMPLERATE_44100:
                    case APTX_SAMPLERATE_48000:
                        break;
                    default:
                        BTIF_TRACE_ERROR(" %s Invalid APTX freq = %d",
                            __func__, p_codec_config_list[i].codec_config.aptx_config.sampling_freq);
                        return BT_STATUS_PARM_INVALID;
                }
                break;
#endif
#if defined(APTX_AD_DECODER_INCLUDED) && (APTX_AD_DECODER_INCLUDED == TRUE)
            case A2DP_SINK_AUDIO_CODEC_APTX_AD:
                switch (p_codec_config_list[i].codec_config.aptx_ad_config.sampling_freq) {
                    case APTX_AD_SAMPLERATE_44100:
                    case APTX_AD_SAMPLERATE_48000:
                        break;
                    default:
                        BTIF_TRACE_ERROR(" %s Invalid APTX AD freq = %d",
                            __func__, p_codec_config_list[i].codec_config.aptx_ad_config.sampling_freq);
                        return BT_STATUS_PARM_INVALID;
                }
                break;
#endif
            default:
                BTIF_TRACE_ERROR(" %s Invalid codec type = %d",
                    __func__, p_codec_config_list[i].codec_type);
                return BT_STATUS_PARM_INVALID;
        }
    }

    pthread_mutex_lock(&split_sink_codec_q_lock);
    if (p_bta_avk_codec_pri_list == NULL) {
        BTIF_TRACE_ERROR(" %s p_bta_avk_codec_pri_list is NULL returning!!", __func__);
        pthread_mutex_unlock(&split_sink_codec_q_lock);
        return BT_STATUS_NOT_READY;
    }

    tA2D_SBC_CIE sbc_supported_cap;
#if defined(AAC_DECODER_INCLUDED) && (AAC_DECODER_INCLUDED == TRUE)
    tA2D_AAC_CIE aac_supported_cap;
#endif
#if defined(MP3_DECODER_INCLUDED) && (MP3_DECODER_INCLUDED == TRUE)
    tA2D_MP3_CIE mp3_supported_cap;
#endif
#if defined(APTX_CLASSIC_DECODER_INCLUDED) && (APTX_CLASSIC_DECODER_INCLUDED == TRUE)
    tA2D_APTX_CIE aptx_supported_cap;
#endif
#if defined(APTX_AD_DECODER_INCLUDED) && (APTX_AD_DECODER_INCLUDED == TRUE)
    tA2DP_AVK_APTX_AD_CIE aptx_ad_supported_cap;
#endif
    uint8_t codec_info[BTIF_SV_AVK_AA_SEP_INDEX][AVDT_CODEC_SIZE];

    /* Copy the codec parameters passed from application layer to create a pointer to
     * preferred codec list for outgoing connection */
    /* Free the memory already allocated and reallocate fresh memory */
    osi_free(p_bta_avk_codec_pri_list);
    p_bta_avk_codec_pri_list = (tBTA_AVK_CO_CODEC_CAP_LIST *)osi_calloc((num_codec_configs +
        BTIF_SV_AVK_AA_SEP_INDEX) * sizeof(tBTA_AVK_CO_CODEC_CAP_LIST));
    if (p_bta_avk_codec_pri_list == NULL) {
        BTIF_TRACE_ERROR(" %s p_bta_avk_codec_pri_list is NULL returning!!", __func__);
        pthread_mutex_unlock(&split_sink_codec_q_lock);
        return BT_STATUS_NOMEM;
    }
    /* Set codec supported capabilities to mandatory capabilities for each codec */
    memcpy(&sbc_supported_cap, &bta_avk_co_sbc_caps, sizeof(tA2D_SBC_CIE));
#if defined(AAC_DECODER_INCLUDED) && (AAC_DECODER_INCLUDED == TRUE)
    memcpy(&aac_supported_cap, &bta_avk_co_aac_caps, sizeof(tA2D_AAC_CIE));
#endif
#if defined(MP3_DECODER_INCLUDED) && (MP3_DECODER_INCLUDED == TRUE)
    memcpy(&mp3_supported_cap, &bta_avk_co_mp3_caps, sizeof(tA2D_MP3_CIE));
#endif
#if defined(APTX_CLASSIC_DECODER_INCLUDED) && (APTX_CLASSIC_DECODER_INCLUDED == TRUE)
    memcpy(&aptx_supported_cap, &bta_avk_co_aptx_caps, sizeof(tA2D_APTX_CIE));
#endif
#if defined(APTX_AD_DECODER_INCLUDED) && (APTX_AD_DECODER_INCLUDED == TRUE)
    memcpy(&aptx_ad_supported_cap, &bta_avk_co_aptx_ad_caps, sizeof(tA2DP_AVK_APTX_AD_CIE));
#endif
    for (i = 0; i < num_codec_configs; i ++) {
        p_bta_avk_codec_pri_list[i].codec_type =
            p_codec_config_list[i].codec_type;
        switch (p_codec_config_list[i].codec_type) {
            case A2DP_SINK_AUDIO_CODEC_SBC:
                /* Copy Mandatory SBC codec parameters */
                memcpy(&p_bta_avk_codec_pri_list[i].codec_cap.sbc_caps,
                    &bta_avk_co_sbc_caps, sizeof(tA2D_SBC_CIE));
                /* Update sampling frequency as per Application layer */
                p_bta_avk_codec_pri_list[i].codec_cap.sbc_caps.samp_freq =
                p_codec_config_list[i].codec_config.sbc_config.samp_freq;
                /* Check if supported capability needs to be updated */
                is_value_to_be_updated(&sbc_supported_cap.samp_freq,
                    &p_codec_config_list[i].codec_config.sbc_config.samp_freq, 1);
                break;
#if defined(AAC_DECODER_INCLUDED) && (AAC_DECODER_INCLUDED == TRUE)
            case A2DP_SINK_AUDIO_CODEC_AAC:
                /* Copy Mandatory AAC codec parameters */
                memcpy(&p_bta_avk_codec_pri_list[i].codec_cap.aac_caps,
                    &bta_avk_co_aac_caps, sizeof(tA2D_AAC_CIE));
                /* Update sampling frequency and object type as per Application layer */
                p_bta_avk_codec_pri_list[i].codec_cap.aac_caps.samp_freq =
                p_codec_config_list[i].codec_config.aac_config.sampling_freq;
                p_bta_avk_codec_pri_list[i].codec_cap.aac_caps.object_type =
                p_codec_config_list[i].codec_config.aac_config.obj_type;
                /* Check if supported capability needs to be updated */
                is_value_to_be_updated(&aac_supported_cap.samp_freq,
                    &p_codec_config_list[i].codec_config.aac_config.sampling_freq, 2);
                is_value_to_be_updated(&aac_supported_cap.object_type,
                    &p_codec_config_list[i].codec_config.aac_config.obj_type, 1);
                break;
#endif
#if defined(MP3_DECODER_INCLUDED) && (MP3_DECODER_INCLUDED == TRUE)
            case A2DP_SINK_AUDIO_CODEC_MP3:
                /* Copy Mandatory MP3 codec parameters */
                memcpy(&p_bta_avk_codec_pri_list[i].codec_cap.mp3_caps,
                    &bta_avk_co_mp3_caps, sizeof(tA2D_MP3_CIE));
                /* Update sampling frequency and layer as per Application layer */
                p_bta_avk_codec_pri_list[i].codec_cap.mp3_caps.samp_freq =
                p_codec_config_list[i].codec_config.mp3_config.sampling_freq;
                p_bta_avk_codec_pri_list[i].codec_cap.mp3_caps.layer =
                p_codec_config_list[i].codec_config.mp3_config.layer;
                /* Check if supported capability needs to be updated */
                is_value_to_be_updated(&mp3_supported_cap.samp_freq,
                    &p_codec_config_list[i].codec_config.mp3_config.sampling_freq, 1);
                is_value_to_be_updated(&mp3_supported_cap.layer,
                    & p_codec_config_list[i].codec_config.mp3_config.layer, 1);
                break;
#endif
#if defined(APTX_CLASSIC_DECODER_INCLUDED) && (APTX_CLASSIC_DECODER_INCLUDED == TRUE)
            case A2DP_SINK_AUDIO_CODEC_APTX:
                /* Update Codec Type for APTX */
                p_bta_avk_codec_pri_list[i].codec_type = A2D_NON_A2DP_MEDIA_CT;
                p_bta_avk_codec_pri_list[i].codec_id = A2D_APTX_CODEC_ID_BLUETOOTH;
                p_bta_avk_codec_pri_list[i].vendor_id = A2D_APTX_VENDOR_ID;
                /* Copy Mandatory APTX codec parameters */
                memcpy(&p_bta_avk_codec_pri_list[i].codec_cap.aptx_caps,
                    &bta_avk_co_aptx_caps, sizeof(tA2D_APTX_CIE));
                /* Update sampling frequency as per Application layer */
                p_bta_avk_codec_pri_list[i].codec_cap.aptx_caps.sampleRate =
                p_codec_config_list[i].codec_config.aptx_config.sampling_freq;
                /* Check if supported capability needs to be updated */
                is_value_to_be_updated(&aptx_supported_cap.sampleRate,
                    &p_codec_config_list[i].codec_config.aptx_config.sampling_freq, 1);
                break;
#endif
#if defined(APTX_AD_DECODER_INCLUDED) && (APTX_AD_DECODER_INCLUDED == TRUE)
            case A2DP_SINK_AUDIO_CODEC_APTX_AD:
                /* Update Codec Type for APTX Adaptive*/
                p_bta_avk_codec_pri_list[i].codec_type = A2D_NON_A2DP_MEDIA_CT;
                p_bta_avk_codec_pri_list[i].codec_id = A2DP_AVK_APTX_AD_CODEC_ID_BLUETOOTH;
                p_bta_avk_codec_pri_list[i].vendor_id = A2DP_AVK_APTX_AD_VENDOR_ID;
                /* Copy Mandatory APTX codec parameters */
                memcpy(&p_bta_avk_codec_pri_list[i].codec_cap.aptx_ad_caps,
                    &bta_avk_co_aptx_ad_caps, sizeof(tA2DP_AVK_APTX_AD_CIE));
                /* Update sampling frequency as per Application layer */
                p_bta_avk_codec_pri_list[i].codec_cap.aptx_ad_caps.sampleRate =
                p_codec_config_list[i].codec_config.aptx_ad_config.sampling_freq;
                /* Check if supported capability needs to be updated */
                is_value_to_be_updated(&aptx_ad_supported_cap.sampleRate,
                    &p_codec_config_list[i].codec_config.aptx_ad_config.sampling_freq, 1);
                break;
#endif
        }
    }

    uint8_t codec_type_list[BTIF_SV_AVK_AA_SEP_INDEX];
    uint8_t vnd_id_list[BTIF_SV_AVK_AA_SEP_INDEX];
    uint8_t codec_id_list[BTIF_SV_AVK_AA_SEP_INDEX];
    uint32_t codec_type_added = 0;
    memset(codec_type_list, A2DP_SINK_AUDIO_CODEC_SBC, BTIF_SV_AVK_AA_SEP_INDEX);
    memset(vnd_id_list, 0, BTIF_SV_AVK_AA_SEP_INDEX);
    memset(codec_id_list, 0, BTIF_SV_AVK_AA_SEP_INDEX);

    /* Remove duplicate codecs from list. This will be used for hiding/showing codecs
     * for response to AVDTP discover command */
    j = 0;
    for (i = 0; i < num_codec_configs; i ++) {
        if (!(codec_type_added & p_codec_config_list[i].codec_type)) {
            codec_type_added |= p_codec_config_list[i].codec_type;
            j ++;
            if (j >= BTIF_SV_AVK_AA_SEP_INDEX) {
                BTIF_TRACE_ERROR(" %s num of different codecs(%d) exceeds max limit",
                    __func__, j);
                break;
            }
        }
    }

    /* Add mandatory codec for all supported codec in the end of priority list to handle
         * case if the codec parameters sent by upper layers are not capable of creating connection.
         * In that case, use the below parameters to create connection. in order of priority of
         * APTX-AD > APTX-CL > AAC > MP3 > SBC */
#if defined(APTX_AD_DECODER_INCLUDED) && (APTX_AD_DECODER_INCLUDED == TRUE)
    if (codec_type_added & A2DP_SINK_AUDIO_CODEC_APTX_AD) {
        p_bta_avk_codec_pri_list[num_codec_configs].codec_type
            = A2D_NON_A2DP_MEDIA_CT;
        /* Copy Mandatory APTX-AD codec parameters */
        memcpy(&p_bta_avk_codec_pri_list[num_codec_configs ++]
            .codec_cap.aptx_ad_caps, &bta_avk_co_aptx_ad_caps,
            sizeof(tA2DP_AVK_APTX_AD_CIE));
        BTIF_TRACE_DEBUG(" %s Added Mandatory APTX-AD codec at index %d",
            __func__, num_codec_configs - 1);
    }
#endif
#if defined(APTX_CLASSIC_DECODER_INCLUDED) && (APTX_CLASSIC_DECODER_INCLUDED == TRUE)
    if (codec_type_added & A2DP_SINK_AUDIO_CODEC_APTX) {
        p_bta_avk_codec_pri_list[num_codec_configs].codec_type
            = A2D_NON_A2DP_MEDIA_CT;
        /* Copy Mandatory APTX codec parameters */
        memcpy(&p_bta_avk_codec_pri_list[num_codec_configs ++]
            .codec_cap.aptx_caps, &bta_avk_co_aptx_caps,
            sizeof(tA2D_APTX_CIE));
        BTIF_TRACE_DEBUG(" %s Added Mandatory APTX codec at index %d",
            __func__, num_codec_configs - 1);
    }
#endif
#if defined(AAC_DECODER_INCLUDED) && (AAC_DECODER_INCLUDED == TRUE)
    if (codec_type_added & A2DP_SINK_AUDIO_CODEC_AAC) {
        p_bta_avk_codec_pri_list[num_codec_configs].codec_type
            = A2DP_SINK_AUDIO_CODEC_AAC;
        /* Copy Mandatory AAC codec parameters */
        memcpy(&p_bta_avk_codec_pri_list[num_codec_configs ++]
            .codec_cap.aac_caps, &bta_avk_co_aac_caps,
            sizeof(tA2D_AAC_CIE));
        BTIF_TRACE_DEBUG(" %s Added Mandatory AAC codec at index %d",
            __func__, num_codec_configs - 1);
    }
#endif
#if defined(MP3_DECODER_INCLUDED) && (MP3_DECODER_INCLUDED == TRUE)
    if (codec_type_added & A2DP_SINK_AUDIO_CODEC_MP3) {
        p_bta_avk_codec_pri_list[num_codec_configs].codec_type
            = A2DP_SINK_AUDIO_CODEC_MP3;
        /* Copy Mandatory MP3 codec parameters */
        memcpy(&p_bta_avk_codec_pri_list[num_codec_configs ++]
            .codec_cap.mp3_caps, &bta_avk_co_mp3_caps,
            sizeof(tA2D_MP3_CIE));
        BTIF_TRACE_DEBUG(" %s Added Mandatory MP3 codec at index %d",
            __func__, num_codec_configs - 1);
    }
#endif
    p_bta_avk_codec_pri_list[num_codec_configs].codec_type
        = A2DP_SINK_AUDIO_CODEC_SBC;
    /* Copy Mandatory SBC codec parameters */
    memcpy(&p_bta_avk_codec_pri_list[num_codec_configs ++]
        .codec_cap.sbc_caps, &bta_avk_co_sbc_caps, sizeof(tA2D_SBC_CIE));
    BTIF_TRACE_DEBUG(" %s Added Mandatory SBC codec at index %d",
        __func__, num_codec_configs - 1);

    j = 0;
    /* Create Codec Config array for supported types as per application layer */
    memset(codec_info, 0, BTIF_SV_AVK_AA_SEP_INDEX * AVDT_CODEC_SIZE);
    codec_type_list[j] = A2DP_MEDIA_CT_SBC;
    A2D_BldSbcInfo(AVDT_MEDIA_TYPE_AUDIO, &sbc_supported_cap, codec_info[j ++]);
    memcpy(&bta_avk_supp_codec_cap[BTIF_SV_AVK_AA_SBC_INDEX]
        .codec_cap.sbc_caps, &sbc_supported_cap, sizeof(tA2D_SBC_CIE));
#if defined(AAC_DECODER_INCLUDED) && (AAC_DECODER_INCLUDED == TRUE)
    if (codec_type_added & A2DP_SINK_AUDIO_CODEC_AAC) {
        codec_type_list[j] = A2DP_MEDIA_CT_AAC;
        A2D_BldAacInfo(AVDT_MEDIA_TYPE_AUDIO, &aac_supported_cap, codec_info[j ++]);
        memcpy(&bta_avk_supp_codec_cap[BTIF_SV_AVK_AA_AAC_INDEX]
            .codec_cap.aac_caps, &aac_supported_cap, sizeof(tA2D_AAC_CIE));
    }
#endif
#if defined(MP3_DECODER_INCLUDED) && (MP3_DECODER_INCLUDED == TRUE)
    if (codec_type_added & A2DP_SINK_AUDIO_CODEC_MP3) {
        codec_type_list[j] = A2D_MEDIA_CT_M12;
        A2D_BldMp3Info(AVDT_MEDIA_TYPE_AUDIO, &mp3_supported_cap, codec_info[j ++]);
        memcpy(&bta_avk_supp_codec_cap[BTIF_SV_AVK_AA_MP3_INDEX]
            .codec_cap.mp3_caps, &mp3_supported_cap, sizeof(tA2D_MP3_CIE));
    }
#endif
#if defined(APTX_CLASSIC_DECODER_INCLUDED) && (APTX_CLASSIC_DECODER_INCLUDED == TRUE)
    if (codec_type_added & A2DP_SINK_AUDIO_CODEC_APTX) {
        codec_type_list[j] = A2D_NON_A2DP_MEDIA_CT;
        vnd_id_list[j] = A2DP_AVK_APTX_AD_VENDOR_ID;
        codec_id_list[j] = A2DP_AVK_APTX_AD_CODEC_ID_BLUETOOTH;
        A2D_BldAptxInfo(AVDT_MEDIA_TYPE_AUDIO, &aptx_supported_cap, codec_info[j ++]);
        memcpy(&bta_avk_supp_codec_cap[BTIF_SV_AVK_AA_APTX_INDEX]
            .codec_cap.aptx_caps, &aptx_supported_cap, sizeof(tA2D_APTX_CIE));
    }
#endif
#if defined(APTX_AD_DECODER_INCLUDED) && (APTX_AD_DECODER_INCLUDED == TRUE)
    if (codec_type_added & A2DP_SINK_AUDIO_CODEC_APTX_AD) {
        codec_type_list[j] = A2D_NON_A2DP_MEDIA_CT;
        vnd_id_list[j] = A2D_APTX_VENDOR_ID;
        codec_id_list[j] = A2D_APTX_CODEC_ID_BLUETOOTH;
        A2D_BldAptxADDecoderInfo(AVDT_MEDIA_TYPE_AUDIO, &aptx_ad_supported_cap, codec_info[j ++]);
        memcpy(&bta_avk_supp_codec_cap[BTIF_SV_AVK_AA_APTX_AD_INDEX]
            .codec_cap.aptx_ad_caps, &aptx_ad_supported_cap, sizeof(tA2DP_AVK_APTX_AD_CIE));
    }
#endif
    bta_avk_num_codec_configs = num_codec_configs;
    BTIF_TRACE_DEBUG(" %s Num_codec_configs = %d", __func__, num_codec_configs);
    for (i = 0; i < j; i ++) {
        BTIF_TRACE_VERBOSE(" %s %d %d %d %d %d %d %d %d %d", __func__,
            codec_info[i][0], codec_info[i][1], codec_info[i][2], codec_info[i][3],
            codec_info[i][4], codec_info[i][5], codec_info[i][6], codec_info[i][7],
            codec_info[i][8]);
    }
    /* Update the codec config supported paratmers so that correct response can be
     * sent for AVDTP discover and get capabilities command from remote device */
    BTA_AvkUpdateCodecSupport(codec_type_list, vnd_id_list, codec_id_list,
        codec_info, j);
    pthread_mutex_unlock(&split_sink_codec_q_lock);
    return BT_STATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         update_flush_device_vendor
**
** Description      Updates the current streaming device from apps
**
** Returns          void
**
*******************************************************************************/
static void update_flushing_device_vendor(bt_bdaddr_t *bd_addr)
{
    // update dummy function for split mode.
    BTIF_TRACE_DEBUG(" %s ", __FUNCTION__);
}

/*******************************************************************************
**
** Function         btif_avk_split_index_matches_state
**
** Description      checks if current state matches any of the input state.
**
** Returns          TRUE: state matches input state
**                  FALSE otherwise
**
**
*******************************************************************************/

bool btif_avk_split_index_matches_state(uint8_t index, uint8_t* states, uint8_t num_states)
{
    btif_sm_state_t state = btif_sm_get_state(btif_avk_cb[index].sm_handle);
    BTIF_TRACE_DEBUG(" %s index = %d, state = %d",__func__,index, state);
    for (int i= 0; i < num_states; i++) {
        if(state == states[i])
            return TRUE;
    }
    return FALSE;
}

/*******************************************************************************
**
** Function         is_streaming_bda_empty
**
** Description      checks if streaming_bda is empty(split).
**
** Returns          TRUE: if streaming_bda is NULL
**
**
*******************************************************************************/
bool is_streaming_bda_empty(void)
{
   bool ret;
   ret = streaming_bda.IsEmpty();
   BTIF_TRACE_DEBUG(" %s ret = %d",__func__,ret);
   return ret;
}

/*******************************************************************************
**
** Function         btif_avk_split_get_started_device_idx
**
** Description      updates index of device which is in started state.
**
** Returns          TRUE: if only 1 device is in started/suspending/VSC handling state
**                  FALSE otherwise
**
**
*******************************************************************************/
bool btif_avk_split_get_started_device_idx(uint8_t* index) {

    btif_sm_state_t state = BTIF_AVK_STATE_IDLE;
    uint8_t i;
    uint8_t num_state_to_check;
    uint8_t states[] = {BTIF_AVK_STATE_STARTED,
                        BTIF_AVK_STATE_SUSPENDING,
                        BTIF_AVK_STATE_HDL_VSC};
    num_state_to_check = sizeof(states)/sizeof(uint8_t);

    for (i = 0; i < btif_max_avk_clients; i++)
    {
        state = btif_sm_get_state(btif_avk_cb[i].sm_handle);
        if (btif_avk_split_index_matches_state(i, states, num_state_to_check))
        {
            *index = i;
            return TRUE;
        } else if ((state == BTIF_AVK_STATE_OPENED) &&
                   (btif_avk_cb[i].vsc_command_status == BTIF_AVK_VSC_STARTED)) {
            BTIF_TRACE_DEBUG(" %s, device is in opened & VSC_started state",__func__);
            *index = i;
            return TRUE;
        }
    }
    BTIF_TRACE_DEBUG(" %s, no device found in started/suspending/HDL_VSC state",__func__);
    return FALSE;
}

bool btif_avk_split_is_device_connected(RawAddress address)
{
    btif_sm_state_t state = btif_get_conn_state_of_device(address);

    if ((state == BTIF_AVK_STATE_OPENED) ||
        (state == BTIF_AVK_STATE_STARTED))
        return TRUE;
    else
        return FALSE;
}

/*******************************************************************************
**
** Function         connect
**
** Description      Establishes the AV signalling channel with the remote headset
**
** Returns          bt_status_t
**
*******************************************************************************/

static bt_status_t connect_int(bt_bdaddr_t *bd_addr, uint16_t uuid)
{
    btif_avk_connect_req_t connect_req;
    int i;
    connect_req.target_bda = bd_addr;
    connect_req.uuid = uuid;
    BTIF_TRACE_EVENT("%s", __FUNCTION__);

    for (i = 0; i < btif_max_avk_clients;)
    {
        if(btif_avk_is_device_connected_connecting(i))
        {
            if (*bd_addr == btif_avk_cb[i].peer_bda)
            {
                BTIF_TRACE_ERROR("Attempting connection for non idle device.. back off ");
                btif_queue_advance();
                return BT_STATUS_SUCCESS;
            }
            i++;
        }
        else
            break;
    }
    if (i == btif_max_avk_clients)
    {
        uint8_t rc_handle;

        BTIF_TRACE_ERROR("%s: All indexes are full", __FUNCTION__);

        /* Multicast: Check if AV slot is available for connection
         * If not available, AV got connected to different devices.
         * Disconnect this RC connection without AV connection.
         */
        rc_handle = btif_avk_rc_get_connected_peer_handle(bd_addr->address);
        if (rc_handle != BTIF_AVK_RC_HANDLE_NONE)
        {
            BTIF_TRACE_IMP("Disconnect only AVRC on : %s",bd_addr->ToString().c_str());
            BTA_AvkCloseRc(rc_handle);
        }
        btif_queue_advance();
        return BT_STATUS_FAIL;
    }

    btif_sm_dispatch(btif_avk_cb[i].sm_handle, BTIF_AVK_CONNECT_REQ_EVT, (char*)&connect_req);
    return BT_STATUS_SUCCESS;
}

static bt_status_t sink_connect_src(bt_bdaddr_t *bd_addr)
{
    BTIF_TRACE_EVENT("%s", __FUNCTION__);
    CHECK_BTAVK_INIT();

    return btif_queue_connect(UUID_SERVCLASS_AUDIO_SINK, *bd_addr, connect_int, btif_max_avk_clients);
}

/*******************************************************************************
**
** Function         disconnect
**
** Description      Tears down the AV signalling channel with the remote headset
**
** Returns          bt_status_t
**
*******************************************************************************/
static bt_status_t disconnect(bt_bdaddr_t *bd_addr)
{
    BTIF_TRACE_EVENT("%s", __FUNCTION__);

    CHECK_BTAVK_INIT();

    /* Switch to BTIF context */
    return btif_transfer_context(btif_avk_handle_event, BTIF_AVK_DISCONNECT_REQ_EVT,
                                 (char*)bd_addr, sizeof(bt_bdaddr_t), NULL);
}

/*******************************************************************************
**
** Function         cleanup
**
** Description      Shuts down the AV interface and does the cleanup
**
** Returns          None
**
*******************************************************************************/
static void cleanup(int service_uuid)
{
    int i;
    BTIF_TRACE_IMP("AVK %s", __FUNCTION__);

    btif_transfer_context(btif_avk_handle_event, BTIF_AVK_CLEANUP_REQ_EVT,
            (char*)&service_uuid, sizeof(int), NULL);
    btif_disable_service(service_uuid);

    alarm_free(avk_open_on_rc_timer);
    avk_open_on_rc_timer = NULL;
    alarm_free(avk_pending_start_timer);
    avk_pending_start_timer = NULL;
    alarm_free(avk_pending_suspend_timer);
    avk_pending_suspend_timer = NULL;
}

static void cleanup_sink(void) {
    BTIF_TRACE_EVENT("%s", __FUNCTION__);
    cleanup(BTA_A2DP_SPLIT_SINK_SERVICE_ID);
    for (int i = 0; i < btif_max_avk_clients; i++)
        btif_avk_cb[i].delay == 0;
}

static void cleanup_sink_vendor(void) {
    BTIF_TRACE_EVENT("%s", __FUNCTION__);
    if (bt_av_sink_vendor_callbacks)
    {
        bt_av_sink_vendor_callbacks = NULL;
    }
    pthread_mutex_lock(&split_sink_codec_q_lock);
    if (p_bta_avk_codec_pri_list != NULL) {
        osi_free(p_bta_avk_codec_pri_list);
        p_bta_avk_codec_pri_list = NULL;
    }
    pthread_mutex_unlock(&split_sink_codec_q_lock);
    pthread_mutex_destroy(&split_sink_codec_q_lock);
    BTIF_TRACE_EVENT("%s completed", __FUNCTION__);
}

static const btav_sink_interface_t bt_av_sink_split_interface = {
    sizeof(btav_sink_interface_t),
    init_sink,
    sink_connect_src,
    disconnect,
    cleanup_sink,
    NULL,
    NULL,
};

static const btav_sink_vendor_interface_t bt_avk_sink_split_vendor_interface = {
    sizeof(btav_sink_vendor_interface_t),
    init_sink_vendor,
    NULL,
    get_a2dp_sink_streaming_data_vendor,
    update_streaming_device_vendor,
    update_flushing_device_vendor,
    cleanup_sink_vendor,
    update_qahw_delay_vendor,
    update_supported_codecs_param_vendor,
    start_ind_rsp,
    suspend_ind_rsp,
    start_req,
    suspend_req,
};

/*******************************************************************************
**
** Function         btif_avk_split_dispatch_sm_event
**
** Description      Send event to AV statemachine
**
** Returns          None
**
*******************************************************************************/

/* used to pass events to AV statemachine from other tasks */
void btif_avk_split_dispatch_sm_event(btif_avk_sm_event_t event, void *p_data, int len)
{
    /* Switch to BTIF context */
    BTIF_TRACE_IMP("%s: event: %d, len: %d", __FUNCTION__, event, len);
    btif_transfer_context(btif_avk_handle_event, event,
                          (char*)p_data, len, NULL);
}

/*******************************************************************************
**
** Function         btif_avk_split_sink_execute_service
**
** Description      Initializes/Shuts down the service
**
** Returns          BT_STATUS_SUCCESS on success, BT_STATUS_FAIL otherwise
**
*******************************************************************************/
bt_status_t btif_avk_split_sink_execute_service(bool b_enable)
{
     int i;
     uint16_t features = 0;
     features = BTA_AVK_FEAT_NO_SCO_SSPD|BTA_AVK_FEAT_RCCT|
                BTA_AVK_FEAT_METADATA|BTA_AVK_FEAT_VENDOR|BTA_AVK_FEAT_ADV_CTRL|
                BTA_AVK_FEAT_RCTG|BTA_AVK_FEAT_SPLIT_ENABLED;
     if (b_enable)
     {
         /* Added BTA_AVK_FEAT_NO_SCO_SSPD - this ensures that the BTA does not
          * auto-suspend av streaming on AG events(SCO or Call). The suspend shall
          * be initiated by the app/audioflinger layers */

         if(enable_delay_reporting) {
             features |= BTA_AVK_FEAT_DELAY_RPT;
             BTIF_TRACE_DEBUG("%s ~~ BTA_AvkEnable Added BTA_AVK_FEAT_DELAY_RPT!", __FUNCTION__);
         }
         if(enable_avrc_browsing) {
             features |= BTA_AVK_FEAT_BROWSE;
             BTIF_TRACE_DEBUG("%s ~~ BTA_AvkEnable NOT Added BTA_AVK_FEAT_BROWSE!", __FUNCTION__);
         }
         BTA_AvkEnable(BTA_SEC_AUTHENTICATE, features, bte_avk_callback);
         for (i = 0; i < btif_max_avk_clients; i++)
         {
             BTA_AvkRegister(BTA_AVK_CHNL_AUDIO, BTIF_AVK_SERVICE_NAME, 0, bte_avk_media_callback,
                             UUID_SERVCLASS_AUDIO_SINK);
         }
     }
     else 
     {// for disable
         for (i = 0; i < btif_max_avk_clients; i++)
         {
             if (btif_avk_cb[i].sm_handle != NULL)
             {
                 BTIF_TRACE_IMP("%s: shutting down AV SM", __FUNCTION__);
                 btif_sm_shutdown(btif_avk_cb[i].sm_handle);
                 btif_avk_cb[i].sm_handle = NULL;
             }
             BTA_AvkDeregister(btif_avk_cb[i].bta_handle);
         }
         BTA_AvkDisable();
     }
     return BT_STATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         btif_avk_get_sink_split_interface
**
** Description      Get the AV callback interface for A2DP sink profile
**
** Returns          btav_sink_interface_t
**
*******************************************************************************/
const btav_sink_interface_t *btif_avk_get_sink_split_interface(void)
{
    BTIF_TRACE_EVENT("%s", __FUNCTION__);
    return &bt_av_sink_split_interface;
}

/*******************************************************************************
**
** Function         btif_avk_get_sink_split_vendor_interface
**
** Description      Get the callback interface for  split-a2dp sink profile
**
** Returns          btav_sink_vendor_interface_t
**
*******************************************************************************/
const btav_sink_vendor_interface_t *btif_avk_get_sink_split_vendor_interface(void)
{
    BTIF_TRACE_IMP("%s", __FUNCTION__);
    return &bt_avk_sink_split_vendor_interface;
}

/*******************************************************************************
**
** Function         btif_avk_split_is_connected
**
** Description      Checks if av has a connected sink
**
** Returns          bool
**
*******************************************************************************/
bool btif_avk_split_is_connected(void)
{
    int i;
    bool status = FALSE;
    for (i = 0; i < btif_max_avk_clients; i++)
    {
        btif_avk_cb[i].state = btif_sm_get_state(btif_avk_cb[i].sm_handle);
        if ((btif_avk_cb[i].state == BTIF_AVK_STATE_OPENED) ||
            (btif_avk_cb[i].state ==  BTIF_AVK_STATE_STARTED))
            status = TRUE;
    }
    return status;
}

/*******************************************************************************
**
** Function         btif_avk_is_playing_on_other_idx
**
** Description      Checks if any other AV SCB is connected
**
** Returns          bool
**
*******************************************************************************/

static bool btif_avk_is_playing_on_other_idx(int current_index)
{
    //return true if other IDx is playing
    btif_sm_state_t state = BTIF_AVK_STATE_IDLE;
    int i;
    for (i = 0; i < btif_max_avk_clients; i++)
    {
        if (i != current_index)
        {
            state = btif_sm_get_state(btif_avk_cb[i].sm_handle);
            if ((state == BTIF_AVK_STATE_STARTED)||
                (state == BTIF_AVK_STATE_STARTING)||
                (state == BTIF_AVK_STATE_SUSPENDING))
                return TRUE;
        }
    }
    return FALSE;
}

/******************************************************************************
**
** Function        btif_avk_split_get_delay
**
** Description     returns delay for split a2dp sink.
**
** Returns         integer
******************************************************************************/
uint32_t btif_avk_split_get_delay(RawAddress bd_addr) {
    int index = btif_avk_idx_by_bdaddr(&bd_addr);
    BTIF_TRACE_ERROR("%s delay, qahwdelay of index %d : %d, %d",
                     __func__, index, btif_avk_cb[index].delay,btif_avk_cb[index].qahw_delay);
    return (btif_avk_cb[index].delay + btif_avk_cb[index].qahw_delay);
}

void btif_avk_pending_start_timeout_handler(void* data) {
    BTIF_TRACE_DEBUG("%s",__func__);
    alarm_cancel(avk_pending_start_timer);
    btif_transfer_context(btif_avk_handle_event, BTIF_AVK_SPLIT_PENDING_START_ALARM_TIMEOUT,
                               (char *)data, sizeof(bt_bdaddr_t), NULL);
}

void btif_avk_pending_suspend_timeout_handler(void* data) {
    BTIF_TRACE_DEBUG("%s",__func__);
    alarm_cancel(avk_pending_suspend_timer);
    btif_transfer_context(btif_avk_handle_event, BTIF_AVK_SPLIT_PENDING_SUSPEND_ALARM_TIMEOUT,
                               (char *)data, sizeof(bt_bdaddr_t), NULL);
}
