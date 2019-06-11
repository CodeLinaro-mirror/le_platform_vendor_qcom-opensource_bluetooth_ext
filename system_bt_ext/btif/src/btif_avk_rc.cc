/******************************************************************************
 *  Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 *  Not a Contribution.
 *
 *  Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *  Not a Contribution.
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
 *  Filename:      btif_rc.c
 *
 *  Description:   Bluetooth AVRC implementation
 *
 *****************************************************************************/
#include <errno.h>
#include <hardware/bluetooth.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "bta_api.h"
#include "bta_av_api.h"
#include "bta_avk_api.h"
#include "avrc_defs.h"

#define LOG_TAG "bt_btif_avrc"
#include "btif_common.h"
#include "btif_util.h"
#include "btif_avk.h"
#include "hardware/bt_rc.h"
#include "hardware/bt_rc_vendor.h"
#include "osi/include/list.h"
#include "osi/include/properties.h"
#include "cutils/properties.h"
#include "btu.h"

/*****************************************************************************
**  Constants & Macros
******************************************************************************/

/* Support Two RC Handles simultaneously*/
#define BTIF_AVK_RC_NUM_CB       2
/* Default index*/
#define BTIF_AVK_RC_DEFAULT_INDEX 0
/* cod value for Headsets */
#define COD_AV_HEADSETS        0x0404
/* for AVRC 1.4 need to change this */
#define MAX_RC_NOTIFICATIONS AVRC_EVT_VOLUME_CHANGE
//#define TEST_BROWSE_RESPONSE
#define MAX_FOLDER_RSP_SUPPORT 10

#define IDX_GET_PLAY_STATUS_RSP    0
#define IDX_LIST_APP_ATTR_RSP      1
#define IDX_LIST_APP_VALUE_RSP     2
#define IDX_GET_CURR_APP_VAL_RSP   3
#define IDX_SET_APP_VAL_RSP        4
#define IDX_GET_APP_ATTR_TXT_RSP   5
#define IDX_GET_APP_VAL_TXT_RSP    6
#define IDX_GET_ELEMENT_ATTR_RSP   7
#define IDX_GET_FOLDER_ITEMS_RSP   8
#define IDX_SET_FOLDER_ITEM_RSP    9
#define IDX_SET_ADDRESS_PLAYER_RSP 10
#define IDX_SET_BROWSE_PLAYER_RSP  11
#define IDX_CHANGE_PATH_RSP        12
#define IDX_PLAY_ITEM_RSP          13
#define IDX_GET_ITEM_ATTR_RSP      14
#define IDX_GET_TOTAL_ITEMS_RSP    15
#define MAX_VOLUME 128
#define MAX_LABEL 16
#define MAX_TRANSACTIONS_PER_SESSION 16
#define PLAY_STATUS_PLAYING 1
#define MAX_CMD_QUEUE_LEN 16
#define ERR_PLAYER_NOT_ADDRESED 0x13
#define BTRC_FEAT_AVRC_UI_UPDATE 0x08

#if (defined(AVCT_COVER_ART_INCLUDED) && (AVCT_COVER_ART_INCLUDED == TRUE))
#define MAX_ELEM_ATTR_SIZE 8
#else
#define MAX_ELEM_ATTR_SIZE 7
#endif

#define CHECK_AVK_RC_CONNECTED                                                                  \
    int clients;                                                                           \
    int conn_status = BT_STATUS_NOT_READY;                                                      \
    BTIF_TRACE_DEBUG("## %s ##", __FUNCTION__);                                            \
    for (clients = 0; clients < btif_max_rc_clients; clients++)                            \
    {                                                                                      \
        if ((btif_avk_rc_cb[clients].rc_connected == TRUE))                                    \
            conn_status = BT_STATUS_SUCCESS;                                                    \
    }                                                                                      \
    if(conn_status == BT_STATUS_NOT_READY)                                                      \
    {                                                                                      \
        BTIF_TRACE_WARNING("Function %s() called when RC is not connected", __FUNCTION__); \
        return BT_STATUS_NOT_READY;                                                        \
    }

#define CHECK_AVK_RC_CONNECTED_BY_IDX                                                      \
    int conn_status = BT_STATUS_NOT_READY;                                                 \
    BTIF_TRACE_DEBUG("## %s CHECK_AVK_RC_CONNECTED_BY_IDX##", __FUNCTION__);                                            \
    if ((btif_avk_rc_cb[index].rc_connected == TRUE))                                      \
       conn_status = BT_STATUS_SUCCESS;                                                     \
    if(conn_status == BT_STATUS_NOT_READY)                                                  \
    {                                                                                       \
        BTIF_TRACE_WARNING("Function %s() called when RC is not connected", __FUNCTION__);  \
        return BT_STATUS_NOT_READY;                                                         \
    }

#define CHECK_AVK_BR_CONNECTED_BY_IDX                                                      \
    int br_conn_status = BT_STATUS_NOT_READY;                                                 \
    BTIF_TRACE_DEBUG("## %s CHECK_AVK_BR_CONNECTED_BY_IDX##", __FUNCTION__);                                            \
    if ((btif_avk_rc_cb[index].br_connected == TRUE))                                      \
       br_conn_status = BT_STATUS_SUCCESS;                                                     \
    if(br_conn_status == BT_STATUS_NOT_READY)                                                  \
    {                                                                                       \
        BTIF_TRACE_WARNING("Function %s() called when BR is not connected", __FUNCTION__);  \
        return BT_STATUS_NOT_READY;                                                         \
    }


#define TXN_LABEL_ENQUEUE(handle, label, front, rear, size, item, cmd)   \
{                                                                        \
    if (size == MAX_TRANSACTIONS_PER_SESSION)                            \
    {                                                                    \
        send_reject_response(handle, item, cmd, AVRC_STS_INTERNAL_ERR);  \
        break;                                                           \
    }                                                                    \
    rear = (rear + 1) % MAX_TRANSACTIONS_PER_SESSION;                    \
    label[rear] = item;                                                  \
    size = size + 1;                                                     \
}

#define TXN_LABEL_DEQUEUE(label, front, rear, size)        \
{                                                          \
    if (size == 0)                                         \
        return BT_STATUS_UNHANDLED;                        \
    front = (front + 1) % MAX_TRANSACTIONS_PER_SESSION;    \
    size = size - 1;                                       \
}

#define FILL_PDU_QUEUE(idx, ctype, label, pending, index, cmd)                                 \
{                                                                                              \
    btif_avk_rc_cb[index].rc_pdu_info[idx].ctype = ctype;                                          \
    TXN_LABEL_ENQUEUE(btif_avk_rc_cb[index].rc_handle, btif_avk_rc_cb[index].rc_pdu_info[idx].label,   \
            btif_avk_rc_cb[index].rc_pdu_info[idx].front, btif_avk_rc_cb[index].rc_pdu_info[idx].rear, \
            btif_avk_rc_cb[index].rc_pdu_info[idx].size, label, cmd);                              \
    BTIF_TRACE_DEBUG("%s txn label %d enqueued to txn queue of pdu %s, queue size %d \n",      \
            __FUNCTION__, label, dump_rc_pdu(cmd), btif_avk_rc_cb[index].rc_pdu_info[idx].size);   \
    btif_avk_rc_cb[index].rc_pdu_info[idx].is_rsp_pending = pending;                               \
}

/*****************************************************************************
**  Local type definitions
******************************************************************************/
typedef struct {
    uint8_t bNotify;
    uint8_t label;
} btif_avk_rc_reg_notifications_t;

typedef struct
{
    int front;
    int rear;
    int size;
    uint8_t label[MAX_TRANSACTIONS_PER_SESSION];
    uint8_t   ctype;
    bool is_rsp_pending;
} btif_avk_rc_cmd_ctxt_t;

/* 2 second timeout to get interim response */
#define BTIF_TIMEOUT_RC_INTERIM_RSP_MS     (2 * 1000)
#define BTIF_TIMEOUT_RC_STATUS_CMD_MS      (2 * 1000)
#define BTIF_TIMEOUT_RC_CONTROL_CMD_MS     (2 * 1000)


typedef enum
{
    eNOT_REGISTERED,
    eREGISTERED,
    eINTERIM
} btif_avk_rc_nfn_reg_status_t;

typedef struct {
    uint8_t                       event_id;
    uint8_t                       label;
    btif_avk_rc_nfn_reg_status_t    status;
} btif_avk_rc_supported_event_t;

#define BTIF_RC_STS_TIMEOUT     0xFE
typedef struct {
    uint8_t   index;
    uint8_t   label;
    uint8_t   pdu_id;
} btif_avk_rc_status_cmd_timer_t;

typedef struct {
    uint8_t   label;
    uint8_t   pdu_id;
} btif_avk_rc_control_cmd_timer_t;

typedef struct {
    union {
        btif_avk_rc_status_cmd_timer_t rc_status_cmd;
        btif_avk_rc_control_cmd_timer_t rc_control_cmd;
    };
} btif_avk_rc_timer_context_t;

typedef struct {
    uint8_t       count;
    tAVRC_CAPS_PARAM param;
} btif_avk_rc_caps_t;

typedef struct {
    bool in_use;
    uint8_t lbl;
    uint8_t handle;
    btif_avk_rc_timer_context_t txn_timer_context;
    alarm_t *txn_timer;
} rc_transaction_t;

typedef struct
{
    pthread_mutex_t lbllock;
    rc_transaction_t transaction[MAX_TRANSACTIONS_PER_SESSION];
    bool lbllock_destroyed;
} rc_device_t;

/* TODO : Merge btif_avk_rc_reg_notifications_t and btif_avk_rc_cmd_ctxt_t to a single struct */
typedef struct {
    bool                     rc_connected;
    bool                     br_connected;
    uint8_t                       rc_handle;
    tBTA_AVK_FEAT                rc_features;
    RawAddress                    rc_addr;
    uint16_t                      rc_pending_play;
    btif_avk_rc_cmd_ctxt_t          rc_pdu_info[MAX_CMD_QUEUE_LEN];
    btif_avk_rc_reg_notifications_t rc_notif[MAX_RC_NOTIFICATIONS];
    unsigned int                rc_volume;
    uint8_t                     rc_vol_label;
    list_t                      *rc_supported_event_list;
    bool                     rc_features_processed;
    bool                     rc_play_processed;
    btif_avk_rc_caps_t          rc_caps_company;
    btif_avk_rc_caps_t          rc_caps_event;
    rc_device_t device;
} btif_avk_rc_cb_t;

alarm_t* p_cap_rsp_timer = NULL;

static uint32_t startItem;
static uint32_t endItem;
static uint16_t enable_avrc_browsing = 0;


#define MAX_UINPUT_PATHS 3
static int btif_max_rc_clients = 1;
static bt_status_t send_passthrough_cmd(bt_bdaddr_t *bd_addr, uint8_t key_code, uint8_t key_state);
static uint8_t btif_avk_rc_idx_by_bdaddr(RawAddress * bd_addr);

static void send_reject_response (uint8_t rc_handle, uint8_t label,
    uint8_t pdu, uint8_t status);
static uint8_t opcode_from_pdu(uint8_t pdu);
static void lbl_init();
static void lbl_destroy();
static void init_all_transactions();
static bt_status_t  get_transaction(rc_transaction_t **ptransaction, uint8_t index);
static void release_transaction(uint8_t label, uint8_t index);
static rc_transaction_t* get_transaction_by_lbl(uint8_t label, uint8_t index);
static int btif_avk_rc_get_idx_by_addr(RawAddress * address);
static void sleep_ms(period_ms_t timeout_ms);
#if (AVRC_CTLR_INCLUDED == TRUE)
static void handle_avk_rc_metamsg_cmd(tBTA_AVK_META_MSG *pmeta_msg);
static void handle_avk_rc_metamsg_rsp(tBTA_AVK_META_MSG *pmeta_msg);
static void register_for_event_notification(int index, btif_avk_rc_supported_event_t *p_event);
static bt_status_t getcapabilities_cmd_vendor(bt_bdaddr_t *bd_addr, uint8_t cap_id);
static bt_status_t getcapabilities_cmd(int index, uint8_t cap_id);
static void btif_avk_rc_ctrl_upstreams_rsp_cmd(uint8_t event, tAVRC_COMMAND *pavrc_cmd,
                                           uint8_t label, uint8_t index);
static void btif_avk_rc_ctrl_upstreams_rsp_evt(uint16_t event, tAVRC_RESPONSE *pavrc_resp,
                                           uint8_t* p_buf, uint16_t buf_len, uint8_t rsp_type, uint8_t index);
static void btif_avk_br_ctrl_upstreams_rsp_evt(uint16_t event, tAVRC_RESPONSE *pavrc_resp,
                                        uint8_t rsp_type, uint8_t index);
#endif

/*****************************************************************************
**  Static variables
******************************************************************************/
/* Two RC CBs needed to handle two connections*/
static btif_avk_rc_cb_t btif_avk_rc_cb[BTIF_AVK_RC_NUM_CB];
static btrc_ctrl_callbacks_t *btif_avk_rc_ctrl_callbacks = NULL;
static btrc_ctrl_vendor_callbacks_t *btif_avk_rc_ctrl_vendor_callbacks = NULL;

/*****************************************************************************
**  Static functions
******************************************************************************/
static uint8_t btif_avk_rc_get_idx_by_rc_handle(uint8_t rc_handle);

/*****************************************************************************
**  Externs
******************************************************************************/
extern bool check_cod(const bt_bdaddr_t *remote_bdaddr, uint32_t cod);
extern void btif_avk_get_latest_playing_device(RawAddress * address); //get the Playing device address
extern bool btif_avk_is_playing();
extern bool btif_avk_is_device_connected(RawAddress address);
extern void btif_avk_trigger_dual_handoff(bool handoff, RawAddress address);
extern bool btif_avk_get_multicast_state();
extern bool btif_avk_is_current_device(RawAddress address);
extern uint16_t btif_avk_get_num_connected_devices(void);
extern uint16_t btif_avk_get_num_playing_devices(void);
extern void handle_rc_passthrough_cmd ( tBTA_AV_REMOTE_CMD *p_remote_cmd);
extern void alarm_set_on_mloop(alarm_t *alarm, period_ms_t interval_ms, alarm_callback_t cb, void *data);

/*****************************************************************************
**  Functions
******************************************************************************/

#if (AVRC_CTLR_INCLUDED == TRUE)
void btif_avk_rc_handle_rc_ctrl_features(int index)
{
	BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);

    if ((btif_avk_rc_cb[index].rc_features & BTA_AVK_FEAT_RCTG)||
       ((btif_avk_rc_cb[index].rc_features & BTA_AVK_FEAT_RCCT)&&
        (btif_avk_rc_cb[index].rc_features & BTA_AVK_FEAT_ADV_CTRL)))
    {
        bt_bdaddr_t rc_addr;
        int rc_features = 0;
        rc_addr = btif_avk_rc_cb[index].rc_addr;

        if ((btif_avk_rc_cb[index].rc_features & BTA_AVK_FEAT_ADV_CTRL)&&
             (btif_avk_rc_cb[index].rc_features & BTA_AVK_FEAT_RCCT))
        {
           BTIF_TRACE_DEBUG("%s: rc_features |= BTRC_FEAT_ABSOLUTE_VOLUME rc_features = %d ", __FUNCTION__, rc_features);
            rc_features |= BTRC_FEAT_ABSOLUTE_VOLUME;
        }
        if ((btif_avk_rc_cb[index].rc_features & BTA_AVK_FEAT_METADATA)&&
            (btif_avk_rc_cb[index].rc_features & BTA_AVK_FEAT_VENDOR)&&
            (btif_avk_rc_cb[index].rc_features_processed != TRUE))
        {
            BTIF_TRACE_DEBUG("%s: rc_features |= BTRC_FEAT_METADATA rc_features = %d ", __FUNCTION__, rc_features);
            rc_features |= BTRC_FEAT_METADATA;
            /* Mark rc features processed to avoid repeating
             * the AVRCP procedure every time on receiving this
             * update.
             */
            if (btif_avk_rc_cb[index].rc_features_processed == FALSE)
            {
                BTIF_TRACE_DEBUG("%s: getcapabilities_cmd index = %d ", __FUNCTION__, index);
                btif_avk_rc_cb[index].rc_features_processed = TRUE;
                getcapabilities_cmd(index, AVRC_CAP_COMPANY_ID);
            }
        }
        BTIF_TRACE_DEBUG("Update rc features to CTRL %d",rc_features);
        HAL_CBACK(btif_avk_rc_ctrl_callbacks, getrcfeatures_cb, &rc_addr, rc_features);
    }
}
#endif

/***************************************************************************
 *  Function       btif_avk_rc_init_txn_label_queue
 *
 *  - Argument:    index of rc control block
 *
 *  - Description: initializes the txn label queues for the rc index
 *
 ***************************************************************************/
static void btif_avk_rc_init_txn_label_queue(int index)
{
    int j;
    for (j = 0; j < MAX_CMD_QUEUE_LEN; j++)
    {
        btif_avk_rc_cb[index].rc_pdu_info[j].front = 0;
        btif_avk_rc_cb[index].rc_pdu_info[j].size = 0;
        btif_avk_rc_cb[index].rc_pdu_info[j].rear = MAX_TRANSACTIONS_PER_SESSION - 1;
    }
}

/***************************************************************************
 *  Function       btif_avk_rc_get_connection_state
 *
 *  - Argument:    none
 *
 *  - Description: Return true if any RC is in connected state
 *
 ***************************************************************************/
static bool btif_avk_rc_get_connection_state()
{
    int clients;

    for (clients = 0; clients < btif_max_rc_clients; clients++)
    {
        if (btif_avk_rc_cb[clients].rc_connected == TRUE)
        {
            return TRUE;
        }
    }
    return FALSE;
}

/***************************************************************************
 *  Function       btif_avk_rc_get_valid_idx
 *
 *  - Argument:    none
 *
 *  - Description: Gets the index which is ready for new connection
 *
 ***************************************************************************/
static int btif_avk_rc_get_valid_idx()
{
    int i;
    for (i = 0; i < btif_max_rc_clients; i++)
    {
        if (!(btif_avk_rc_cb[i].rc_connected))
            break;
    }
    return i;
}

/***************************************************************************
 *  Function       btif_avk_rc_get_idx_by_rc_handle
 *
 *  - Argument:    rc handle
 *
 *  - Description: Gets the RC handle index of matching handle
 *
 ***************************************************************************/
static uint8_t btif_avk_rc_get_idx_by_rc_handle(uint8_t rc_handle)
{
    uint8_t i;

    for (i = 0; i < btif_max_rc_clients; i++)
    {
        if (btif_avk_rc_cb[i].rc_handle == rc_handle)
            break;
    }
    return i;
}

/* Get the address of device on which PLAY command came
* This address will be used in AV IF layer to determine
* On which device to START playback. */
/***************************************************************************
 *  Function       btif_avk_rc_get_playing_device
 *
 *  - Argument:    bd_addr
 *
 *  - Description: Copies the BD address of current playing device
 *
 ***************************************************************************/
void btif_avk_rc_get_playing_device(RawAddress * address)
{
    int i;
    for (i = 0; i < btif_max_rc_clients; i++)
    {
        if (btif_avk_rc_cb[i].rc_play_processed)
        {
            //copy bd address
            address = &btif_avk_rc_cb[i].rc_addr;
        }
    }
}

/* Reset the Play trigger, once the AVDTP START is
* sent, called from AV IF layer. */
/***************************************************************************
 *  Function       btif_avk_rc_clear_playing_state
 *
 *  - Argument:    bool
 *
 *  - Description: Clears the PLAY processed.rc_play_processed denotes
 *                 play command has been processed for this device.
 *
 ***************************************************************************/
void btif_avk_rc_clear_playing_state(bool state)
{
    int i;
    for (i = 0; i < btif_max_rc_clients; i++)
    {
        if (btif_avk_rc_cb[i].rc_play_processed)
        {
            btif_avk_rc_cb[i].rc_play_processed = state;
        }
    }
}

/***************************************************************************
 *  Function       btif_avk_rc_clear_priority
 *
 *  - Argument:    Device address
 *
 *  - Description: Clears the priority information for the device
 *                 This can be used while AV disconnection for the device.
 *                 Setting of rc_play_processed flag could have been avoided
 *                 looking at the stream state, but it might still leave some
 *                 corner case of audio suspending just before the play takes
 *                 effect.
 ***************************************************************************/
void btif_avk_rc_clear_priority(RawAddress address)
{
    int index;

    index = btif_avk_rc_get_idx_by_addr(&address);
    if(index < btif_max_rc_clients)
    {
        btif_avk_rc_cb[index].rc_play_processed = FALSE;
    }
}

/***************************************************************************
 *  Function       btif_avk_rc_handle_br_connect
 *
 *  - Argument:    tBTA_AV_RC_OPEN  browse RC open data structure
 *
 *  - Description: browse RC connection event handler
 *
 ***************************************************************************/
void btif_avk_rc_handle_br_connect(tBTA_AVK_RC_BROWSE_OPEN* p_rc_br_open) {
  BTIF_TRACE_DEBUG("%s: btif_avk_rc_handle_br_connect :rc_handle %d status %d", __func__,
                   p_rc_br_open->rc_handle, p_rc_br_open->status);

  int index = btif_avk_rc_get_idx_by_rc_handle(p_rc_br_open->rc_handle);
  BTIF_TRACE_IMP("%s: rc_handle: %d index %d", __FUNCTION__, p_rc_br_open->rc_handle, index);
  if (index == btif_max_rc_clients)
  {
      BTIF_TRACE_ERROR("Got br connect of unknown device");
      return;
  }

  if (p_rc_br_open->status == BTA_AVK_SUCCESS) {
    bt_bdaddr_t rc_addr;
    rc_addr = p_rc_br_open->peer_addr;
    btif_avk_rc_cb[index].br_connected = TRUE;

    BTIF_TRACE_IMP("%s: HAL_CBACK connection_state_cb browse channel connected", __FUNCTION__);
    HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks, browse_connection_state_vendor_cb, TRUE, &rc_addr);
  }
}

 /***************************************************************************
 *  Function       btif_avk_rc_handle_rc_connect
 *
 *  - Argument:    tBTA_AVK_RC_OPEN  RC open data structure
 *
 *  - Description: RC connection event handler
 *
 ***************************************************************************/
static void btif_avk_rc_handle_rc_connect (tBTA_AVK_RC_OPEN *p_rc_open)
{
    bt_status_t result = BT_STATUS_SUCCESS;
    bt_bdaddr_t rc_addr;
    int index;

    BTIF_TRACE_IMP("%s: rc_handle: %d", __FUNCTION__, p_rc_open->rc_handle);
    if(p_rc_open->status == BTA_AVK_SUCCESS)
    {
        //Check if already some RC is connected
        /*Now we can have two RC connections
        * Check should be here about 3rd connection too.
        * Get the free or MAX index. Max index should be rejected. */
        index = btif_avk_rc_get_valid_idx();
        if (index == btif_max_rc_clients)
        {
            /*Reached Max, this connection must be rejected*/
            BTIF_TRACE_ERROR("RC OPEN in MAX connected state");
            BTA_AvkCloseRc(p_rc_open->rc_handle);
            return;
        }
        /*Use the index for this RC connection*/
        BTIF_TRACE_DEBUG("Got RC OPEN on the index= %d", index);
        btif_avk_rc_cb[index].rc_addr = p_rc_open->peer_addr;
        btif_avk_rc_cb[index].rc_features = p_rc_open->peer_features;
        btif_avk_rc_cb[index].rc_vol_label = MAX_LABEL;
        btif_avk_rc_cb[index].rc_volume = MAX_VOLUME;
        btif_avk_rc_cb[index].rc_connected = TRUE;
        btif_avk_rc_cb[index].rc_handle = p_rc_open->rc_handle;
        btif_avk_rc_init_txn_label_queue(index);
        rc_addr = btif_avk_rc_cb[index].rc_addr;
#if (AVRC_CTLR_INCLUDED == TRUE)
        if(btif_avk_rc_ctrl_callbacks != NULL) {
            BTIF_TRACE_DEBUG("HAL_CBACK connection_state_cb: control channel connected ");
            HAL_CBACK(btif_avk_rc_ctrl_callbacks, connection_state_cb, TRUE, FALSE, &rc_addr);
        }
        /* report connection state if remote device is AVRCP target */
        if ((btif_avk_rc_cb[index].rc_features & BTA_AVK_FEAT_RCTG)||
           ((btif_avk_rc_cb[index].rc_features & BTA_AVK_FEAT_RCCT)&&
            (btif_avk_rc_cb[index].rc_features & BTA_AVK_FEAT_ADV_CTRL)))
        {
            btif_avk_rc_handle_rc_ctrl_features(index);
        }
#endif
        /* on locally initiated connection we will get remote features as part of connect
        Delay this update till connection update reaches Apps*/
    }
    else
    {
        BTIF_TRACE_ERROR("%s Connect failed with error code: %d",
            __FUNCTION__, p_rc_open->status);
    }
}

/***************************************************************************
 *  Function       btif_avk_rc_handle_rc_disconnect
 *
 *  - Argument:    tBTA_AVK_RC_CLOSE     RC close data structure
 *
 *  - Description: RC disconnection event handler
 *
 ***************************************************************************/
static void btif_avk_rc_handle_rc_disconnect (tBTA_AVK_RC_CLOSE *p_rc_close)
{
    bt_bdaddr_t rc_addr;
    tBTA_AVK_FEAT features;
    uint8_t index;
    bool is_connected = 0;

    index = btif_avk_rc_get_idx_by_rc_handle(p_rc_close->rc_handle);
    BTIF_TRACE_IMP("%s: rc_handle: %d index %d", __FUNCTION__, p_rc_close->rc_handle, index);
    if (index == btif_max_rc_clients)
    {
        BTIF_TRACE_ERROR("Got disconnect of unknown device");
        return;
    }
    if ((p_rc_close->rc_handle != btif_avk_rc_cb[index].rc_handle)
        && (btif_avk_rc_cb[index].rc_addr != p_rc_close->peer_addr))
    {
        BTIF_TRACE_ERROR("Got disconnect of unknown device");
        return;
    }
#if (AVRC_CTLR_INCLUDED == TRUE)
    rc_addr = btif_avk_rc_cb[index].rc_addr;
    features = btif_avk_rc_cb[index].rc_features;
#endif
    if (btif_avk_rc_cb[index].rc_supported_event_list != NULL) {
        list_clear(btif_avk_rc_cb[index].rc_supported_event_list);
    }
    btif_avk_rc_cb[index].rc_handle = BTIF_AVK_RC_HANDLE_NONE;
    btif_avk_rc_cb[index].rc_connected = FALSE;
    btif_avk_rc_cb[index].br_connected = FALSE;
    rc_addr = btif_avk_rc_cb[index].rc_addr;
    btif_avk_rc_cb[index].rc_addr = RawAddress::kEmpty;
    memset(btif_avk_rc_cb[index].rc_notif, 0, sizeof(btif_avk_rc_cb[index].rc_notif));
    btif_avk_rc_cb[index].rc_features = 0;
    btif_avk_rc_cb[index].rc_vol_label = MAX_LABEL;
    btif_avk_rc_cb[index].rc_volume = MAX_VOLUME;
    btif_avk_rc_cb[index].rc_play_processed = FALSE;
    btif_avk_rc_cb[index].rc_pending_play = FALSE;
    btif_avk_rc_cb[index].rc_features_processed = FALSE;
    memset(&btif_avk_rc_cb[index].rc_caps_company, 0, sizeof(btif_avk_rc_caps_t));
    memset(&btif_avk_rc_cb[index].rc_caps_event, 0, sizeof(btif_avk_rc_caps_t));
    for(int lbl = 0; lbl < MAX_TRANSACTIONS_PER_SESSION ; lbl++)
        release_transaction(lbl, index);
    btif_avk_rc_init_txn_label_queue(index);

    //CLose Uinput only when all RCs are disconnected
    is_connected = btif_avk_rc_get_connection_state();
    BTIF_TRACE_DEBUG("RC connected : %d", is_connected);
    if (is_connected != TRUE && btif_avk_rc_cb[index].device.lbllock_destroyed != TRUE)
    {
        BTIF_TRACE_DEBUG("Clear UINPUT and transactions when zero RC left");
        init_all_transactions();
#ifdef ANDROID
        close_uinput();
#endif
    }
    if (rc_addr.IsEmpty())
    {
        BTIF_TRACE_DEBUG("Cleanup already done");
        return;
    }
#if (AVRC_CTLR_INCLUDED == TRUE)
    /* report connection state if device is AVRCP target */
    if (btif_avk_rc_ctrl_callbacks != NULL)
    {
        HAL_CBACK(btif_avk_rc_ctrl_callbacks, connection_state_cb, FALSE, FALSE, &rc_addr);
    }
#endif
}

static void btif_avk_rc_handle_br_disconnect (tBTA_AVK_RC_BROWSE_CLOSE *p_rc_close)
{
    bt_bdaddr_t rc_addr;
    uint8_t index;

    index = btif_avk_rc_get_idx_by_rc_handle(p_rc_close->rc_handle);
    BTIF_TRACE_IMP("%s: rc_handle: %d index %d", __FUNCTION__, p_rc_close->rc_handle, index);
    if (index == btif_max_rc_clients)
    {
        BTIF_TRACE_ERROR("Got disconnect of unknown device");
        return;
    }
    btif_avk_rc_cb[index].br_connected = FALSE;

#if (AVRC_CTLR_INCLUDED == TRUE)
    rc_addr = btif_avk_rc_cb[index].rc_addr;

    BTIF_TRACE_IMP("%s: HAL_CBACK connection_state_cb browse channel disconnected peer addr: %s", __FUNCTION__, rc_addr.ToString().c_str());
    HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks, browse_connection_state_vendor_cb, FALSE, &rc_addr);
#endif

}


/***************************************************************************
 *  Function       btif_rc_ctrl_send_pause
 *
 *  - Argument:    Index
 *
 *  - Description: Sends PAUSE key event to remote.
 *
 ***************************************************************************/
void btif_avk_rc_ctrl_send_pause(bt_bdaddr_t *bd_addr)
{
// send pass through command  ( AVRCP_PAUSE )to remote.
    BTIF_TRACE_DEBUG("%s: ", __FUNCTION__);
    send_passthrough_cmd(bd_addr, AVRC_ID_PAUSE ,AVRC_STATE_PRESS);
    send_passthrough_cmd(bd_addr, AVRC_ID_PAUSE ,AVRC_STATE_RELEASE);
}
/***************************************************************************
 *  Function       btif_rc_ctrl_send_play
 *
 *  - Argument:    Index
 *
 *  - Description: Sends PLAY key event to remote.
 *
 ***************************************************************************/
void btif_avk_rc_ctrl_send_play(bt_bdaddr_t *bd_addr)
{
// send pass through command (AVRCP_PLAY) to remote.
    BTIF_TRACE_DEBUG("%s: ", __FUNCTION__);
    send_passthrough_cmd(bd_addr, AVRC_ID_PLAY ,AVRC_STATE_PRESS);
    send_passthrough_cmd(bd_addr, AVRC_ID_PLAY ,AVRC_STATE_RELEASE);
}

/***************************************************************************
 *  Function       btif_avk_rc_handle_rc_passthrough_rsp
 *
 *  - Argument:    tBTA_AVK_REMOTE_RSP passthrough command response
 *
 *  - Description: Remote control passthrough response handler
 *
 ***************************************************************************/
static void btif_avk_rc_handle_rc_passthrough_rsp ( tBTA_AVK_REMOTE_RSP *p_remote_rsp)
{
#if (AVRC_CTLR_INCLUDED == TRUE)
    const char *status;
    int index = btif_avk_rc_get_idx_by_rc_handle(p_remote_rsp->rc_handle);
    BTIF_TRACE_DEBUG("%s: index=%d", __FUNCTION__, index);
    bt_bdaddr_t rc_addr;
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return;
    }
    if (btif_avk_rc_cb[index].rc_features & BTA_AVK_FEAT_RCTG)
    {
        int key_state;
        if (p_remote_rsp->key_state == AVRC_STATE_RELEASE)
        {
            status = "released";
            key_state = 1;
        }
        else
        {
            status = "pressed";
            key_state = 0;
        }

        BTIF_TRACE_DEBUG("%s: rc_id=%d status=%s", __FUNCTION__, p_remote_rsp->rc_id, status);
        rc_addr = btif_avk_rc_cb[index].rc_addr;
        release_transaction(p_remote_rsp->label, index);
        if (btif_avk_rc_ctrl_vendor_callbacks != NULL) {
            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks, passthrough_rsp_vendor_cb,
                    p_remote_rsp->rc_id, key_state, &rc_addr);
        }
    }
    else
    {
        BTIF_TRACE_ERROR("%s DUT does not support AVRCP controller role", __FUNCTION__);
    }
#else
    BTIF_TRACE_ERROR("%s AVRCP controller role is not enabled", __FUNCTION__);
#endif
}

/***************************************************************************
 **
 ** Function       btif_avk_rc_handler
 **
 ** Description    RC event handler
 **
 ***************************************************************************/
void btif_avk_rc_handler(tBTA_AVK_EVT event, tBTA_AVK *p_data)
{
    uint8_t index;

    BTIF_TRACE_IMP("%s event:%s", __FUNCTION__, dump_rc_event(event));

    switch (event)
    {
        case BTA_AVK_RC_OPEN_EVT:
        {
            BTIF_TRACE_DEBUG("Peer_features:%x", p_data->rc_open.peer_features);
            btif_avk_rc_handle_rc_connect( &(p_data->rc_open) );
        }break;
        case BTA_AVK_RC_BROWSE_OPEN_EVT:
        {
            BTIF_TRACE_DEBUG("btif_avk_rc_handler : BTA_AVK_RC_BROWSE_OPEN_EVT ");
            btif_avk_rc_handle_br_connect( &(p_data->rc_browse_open) );
        }break;
        case BTA_AVK_RC_BROWSE_CLOSE_EVT:
        {
            BTIF_TRACE_DEBUG("btif_avk_rc_handler : BTA_AVK_RC_BROWSE_CLOSE_EVT ");
            btif_avk_rc_handle_br_disconnect( &(p_data->rc_browse_close) );
        }break;
        case BTA_AVK_RC_CLOSE_EVT:
        {
            btif_avk_rc_handle_rc_disconnect( &(p_data->rc_close) );
        }break;

#if (AVRC_CTLR_INCLUDED == TRUE)
        case BTA_AVK_REMOTE_RSP_EVT:
        {
            BTIF_TRACE_DEBUG("RSP: rc_id:0x%x key_state:%d", p_data->remote_rsp.rc_id,
                               p_data->remote_rsp.key_state);
            btif_avk_rc_handle_rc_passthrough_rsp( (&p_data->remote_rsp) );
        }
        break;
#endif
        case BTA_AVK_RC_FEAT_EVT:
        {
            BTIF_TRACE_DEBUG("Peer_features:%x on RC handle: %d", p_data->rc_feat.peer_features,
                            p_data->rc_feat.rc_handle);
            index = btif_avk_rc_get_idx_by_rc_handle(p_data->rc_feat.rc_handle);
            if (index == btif_max_rc_clients)
            {
                BTIF_TRACE_ERROR("%s: Invalid RC index for BTA_AVK_RC_FEAT_EVT", __FUNCTION__);
                return;
            }
            btif_avk_rc_cb[index].rc_features = p_data->rc_feat.peer_features;
#if (AVRC_CTLR_INCLUDED == TRUE)
            if ((btif_avk_rc_cb[index].rc_connected) && (btif_avk_rc_ctrl_callbacks != NULL))
            {
                btif_avk_rc_handle_rc_ctrl_features(index);
            }
#endif
        }
        break;
        case BTA_AVK_META_MSG_EVT:
        {
#if (AVRC_CTLR_INCLUDED == TRUE)
            if(btif_avk_rc_ctrl_callbacks != NULL)
            {
                /* This is case of Sink + CT + TG(for abs vol)) */
                BTIF_TRACE_DEBUG("BTA_AVK_META_MSG_EVT  code:%d label:%d",
                                                p_data->meta_msg.code,
                                                p_data->meta_msg.label);
                BTIF_TRACE_DEBUG("  company_id:0x%x len:%d handle:%d",
                                            p_data->meta_msg.company_id,
                                            p_data->meta_msg.len,
                                            p_data->meta_msg.rc_handle);

                switch (p_data->meta_msg.p_msg->hdr.opcode) {
                  case AVRC_OP_VENDOR:
                        if ((p_data->meta_msg.code >= AVRC_RSP_NOT_IMPL)&&
                            (p_data->meta_msg.code <= AVRC_RSP_INTERIM))
                        {
                            /* Its a response */
                            handle_avk_rc_metamsg_rsp(&(p_data->meta_msg));
                        }
                        else if (p_data->meta_msg.code <= AVRC_CMD_GEN_INQ)
                        {
                            /* Its a command  */
                            handle_avk_rc_metamsg_cmd(&(p_data->meta_msg));
                        }
                        break;

                    case AVRC_OP_BROWSE:
                      if (p_data->meta_msg.p_msg->hdr.ctype == AVRC_CMD) {
                        handle_avk_rc_metamsg_cmd(&(p_data->meta_msg));
                      } else if (p_data->meta_msg.p_msg->hdr.ctype == AVRC_RSP) {
                        handle_avk_rc_metamsg_rsp(&(p_data->meta_msg));

                        BTIF_TRACE_DEBUG("handle_avk_rc_metamsg_rsp end");
                      }
                      break;
                    }
                }
#endif
            else
            {
                BTIF_TRACE_ERROR("Neither CTRL, nor TG is up, drop meta commands");
            }
        }
        break;
        default:
            BTIF_TRACE_DEBUG("Unhandled RC event : 0x%x", event);
    }
}

/***************************************************************************
 **
 ** Function       btif_avk_rc_get_connected_peer
 **
 ** Description    Fetches the connected headset's BD_ADDR if any
 **
 ***************************************************************************/
bool btif_avk_rc_get_connected_peer(RawAddress* peer_addr)
{
    /*Find the device for which AV is not connected but RC is.*/
    int i;

    for  (i = 0; i < btif_max_rc_clients; i++)
    {
        if (btif_avk_rc_cb[i].rc_connected == TRUE)
        {
            if (!btif_avk_is_device_connected(btif_avk_rc_cb[i].rc_addr))
            {
                *peer_addr = btif_avk_rc_cb[i].rc_addr;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static int btif_avk_rc_get_idx_by_addr(RawAddress * address)
{
    int i;

    for (i = 0; i < btif_max_rc_clients; i++)
    {
        if (btif_avk_rc_cb[i].rc_addr == *address)
        {
            break;
        }
    }
    return i;
}

/***************************************************************************
 **
 ** Function       btif_avk_rc_get_connected_peer_handle
 **
 ** Description    Fetches the connected headset's handle if any
 **
 ***************************************************************************/
uint8_t btif_avk_rc_get_connected_peer_handle(const RawAddress&  peer_addr)
{
    int i;
    for  (i = 0; i < btif_max_rc_clients; i++)
    {
        if ((btif_avk_rc_cb[i].rc_connected == TRUE)
             &&(peer_addr == btif_avk_rc_cb[i].rc_addr))
        {
            return btif_avk_rc_cb[i].rc_handle;
        }
    }
    return BTIF_AVK_RC_HANDLE_NONE;

}

/***************************************************************************
 **
 ** Function       btif_avk_rc_check_handle_pending_play
 **
 ** Description    Clears the queued PLAY command. if bSend is TRUE, forwards to app
 **
 ***************************************************************************/

/* clear the queued PLAY command. if bSend is TRUE, forward to app */
void btif_avk_rc_check_handle_pending_play (RawAddress * peer_addr, bool bSendToApp)
{
    int index = btif_avk_rc_get_idx_by_addr(peer_addr);

    if (index == btif_max_rc_clients)
    {
        BTIF_TRACE_ERROR("%s: Invalid RC index", __FUNCTION__);
        return;
    }

    BTIF_TRACE_DEBUG("%s: bSendToApp=%d", __FUNCTION__, bSendToApp);
    if (btif_avk_rc_cb[index].rc_pending_play)
    {
        if (bSendToApp)
        {
            tBTA_AVK_REMOTE_CMD remote_cmd;
            APPL_TRACE_DEBUG("%s: Sending queued PLAYED event to app", __FUNCTION__);

            memset (&remote_cmd, 0, sizeof(tBTA_AVK_REMOTE_CMD));
            remote_cmd.rc_handle  = btif_avk_rc_cb[index].rc_handle;
            remote_cmd.rc_id      = AVRC_ID_PLAY;
            remote_cmd.hdr.ctype  = AVRC_CMD_CTRL;
            remote_cmd.hdr.opcode = AVRC_OP_PASS_THRU;

            /* delay sending to app, else there is a timing issue in the framework,
             ** which causes the audio to be on th device's speaker. Delay between
             ** OPEN & RC_PLAYs
            */
            sleep_ms (200);
            /* send to app - both PRESSED & RELEASED */
            remote_cmd.key_state  = AVRC_STATE_PRESS;
            handle_rc_passthrough_cmd( (tBTA_AV_REMOTE_CMD*)&remote_cmd );

            sleep_ms (100);

            remote_cmd.key_state  = AVRC_STATE_RELEASE;
            handle_rc_passthrough_cmd( (tBTA_AV_REMOTE_CMD*)&remote_cmd );
        }
        btif_avk_rc_cb[index].rc_pending_play = FALSE;
    }
}

/* Generic reject response */
static void send_reject_response (uint8_t rc_handle, uint8_t label, uint8_t pdu, uint8_t status)
{
    uint8_t ctype = AVRC_RSP_REJ;
    tAVRC_RESPONSE avrc_rsp;
    BT_HDR *p_msg = NULL;
    memset (&avrc_rsp, 0, sizeof(tAVRC_RESPONSE));

    avrc_rsp.rsp.opcode = opcode_from_pdu(pdu);
    avrc_rsp.rsp.pdu    = pdu;
    avrc_rsp.rsp.status = status;

    if (AVRC_STS_NO_ERROR == (status = AVRC_BldResponse(rc_handle, &avrc_rsp, &p_msg)) )
    {
        BTIF_TRACE_DEBUG("%s:Sending error notification to handle:%d. pdu:%s,status:0x%02x",
            __FUNCTION__, rc_handle, dump_rc_pdu(pdu), status);
        BTA_AvkMetaRsp(rc_handle, label, ctype, p_msg);
    }
}

static uint8_t opcode_from_pdu(uint8_t pdu)
{
    uint8_t opcode = 0;

    switch (pdu)
    {
    case AVRC_PDU_NEXT_GROUP:
    case AVRC_PDU_PREV_GROUP: /* pass thru */
        opcode  = AVRC_OP_PASS_THRU;
        break;

    default: /* vendor */
        opcode  = AVRC_OP_VENDOR;
        break;
    }

    return opcode;
}

#if (AVRC_CTLR_INCLUDED == TRUE)

/***************************************************************************
 *
 * Function         iterate_supported_event_list_for_interim_rsp
 *
 * Description      iterator callback function to match the event and handle
 *                  timer cleanup
 * Returns          true to continue iterating, false to stop
 *
 **************************************************************************/
bool avk_iterate_supported_event_list_for_interim_rsp(void* data, void* cb_data) {
  uint8_t* p_event_id;
  btif_avk_rc_supported_event_t* p_event = (btif_avk_rc_supported_event_t*)data;

  p_event_id = (uint8_t*)cb_data;

  if (p_event->event_id == *p_event_id) {
    p_event->status = eINTERIM;
    return false;
  }
  return true;
}

/***************************************************************************
**
** Function         avk_iterate_supported_event_list_for_timeout
**
** Description      Iterator callback function for timeout handling.
**                  As part of the failure handling, it releases the
**                  transaction label and removes the event from list,
**                  this event will not be requested again during
**                  the lifetime of the connection.
** Returns          false to stop iterating, true to continue
**
***************************************************************************/
bool avk_iterate_supported_event_list_for_timeout(void *data, void *cb_data)
{
    BTIF_TRACE_DEBUG("%s: ", __FUNCTION__);

    btif_avk_rc_supported_event_t *p_event = (btif_avk_rc_supported_event_t *)data;

    uint16_t uData = *(uint16_t*)cb_data;

    uint8_t label = (uData & 0xff);
    uint8_t index = ((uData >> 8) & 0xff);
    if (p_event->label == label)
    {
        list_remove(btif_avk_rc_cb[index].rc_supported_event_list, p_event);
        return false;
    }
    return true;
}

static void btif_avk_rc_get_caps_cmd_timeout_handler(UNUSED_ATTR uint16_t event,
                                               char *data)
{
    uint16_t uData = *(uint16_t*)data;    BTIF_TRACE_DEBUG("%s: uData=%d", __FUNCTION__,uData);
    uint8_t cap_id = ((uData >> 8) & 0xff);

    uint8_t index = (uData & 0xff);
    BTIF_TRACE_DEBUG("%s: index = %d cap_id=%d ", __FUNCTION__, index, cap_id);

    if(cap_id == AVRC_CAP_COMPANY_ID)
    {
        HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks, getcap_rsp_vendor_cb, &btif_avk_rc_cb[index].rc_addr,
        AVRC_CAP_COMPANY_ID,(uint32_t *)btif_avk_rc_cb[index].rc_caps_company.param.company_id,
        btif_avk_rc_cb[index].rc_caps_company.count, AVRC_RSP_IMPL_STBL);
    }
    else if(cap_id == AVRC_CAP_EVENTS_SUPPORTED)
    {
        HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks, getcap_rsp_vendor_cb, &btif_avk_rc_cb[index].rc_addr,
        AVRC_CAP_EVENTS_SUPPORTED,(uint32_t *)btif_avk_rc_cb[index].rc_caps_event.param.event_id,
        btif_avk_rc_cb[index].rc_caps_event.count, AVRC_RSP_IMPL_STBL);

    }

    p_cap_rsp_timer = NULL;

}

static void btif_avk_rc_get_caps_cmd_timer_timeout(void *data)
{
    btif_transfer_context(btif_avk_rc_get_caps_cmd_timeout_handler, 0,
                          (char *)data, sizeof(uint16_t),
                          NULL);


}

/***************************************************************************
**
** Function         avk_rc_notification_interim_timout
**
** Description      Interim response timeout handler.
**                  Runs the iterator to check and clear the timed out event.
** Returns          None
**
***************************************************************************/
static void avk_rc_notification_interim_timout (uint8_t index, uint8_t label)
{
    list_node_t *node;
    if (btif_avk_rc_cb[index].rc_supported_event_list == NULL)
        return;
    BTIF_TRACE_DEBUG("%s: index = %d label = %d ", __FUNCTION__, index, label);

    uint16_t data = ((((uint16_t)index << 8) & 0xff00) | (label & 0x00ff));

    list_foreach(btif_avk_rc_cb[index].rc_supported_event_list,
                     avk_iterate_supported_event_list_for_timeout, &data);
    /* Timeout happened for interim response for the registered event,
     * check if there are any pending for registration
     */
    node = list_begin(btif_avk_rc_cb[index].rc_supported_event_list);
    while (node != NULL)
    {
        btif_avk_rc_supported_event_t *p_event;

        p_event = (btif_avk_rc_supported_event_t *)list_node(node);
        if ((p_event != NULL) && (p_event->status == eNOT_REGISTERED))
        {
            BTIF_TRACE_DEBUG("%s: register_for_event_notification", __FUNCTION__, label);
            register_for_event_notification(index, p_event);
            break;
        }
        node = list_next (node);
    }
    /* Todo. Need to initiate application settings query if this
     * is the last event registration.
     */
}

/***************************************************************************
**
** Function         btif_avk_rc_status_cmd_timeout_handler
**
** Description      RC status command timeout handler (Runs in BTIF context).
** Returns          None
**
***************************************************************************/
static void btif_avk_rc_status_cmd_timeout_handler(UNUSED_ATTR uint16_t event,
                                               char *data)
{
    btif_avk_rc_timer_context_t *p_context;
    tAVRC_RESPONSE      avrc_response = {0};
 //   tBTA_AVK_META_MSG    meta_msg;

    p_context = (btif_avk_rc_timer_context_t *)data;
 //   memset(&meta_msg, 0, sizeof(tBTA_AVK_META_MSG));
 //   meta_msg.rc_handle = btif_avk_rc_cb[0].rc_handle;
    release_transaction(p_context->rc_status_cmd.label, p_context->rc_status_cmd.index);

    switch (p_context->rc_status_cmd.pdu_id) {
    case AVRC_PDU_REGISTER_NOTIFICATION:
    BTIF_TRACE_DEBUG("%s: call avk_rc_notification_interim_timout", __FUNCTION__);
        avk_rc_notification_interim_timout(p_context->rc_status_cmd.index, p_context->rc_status_cmd.label);
        break;

    case AVRC_PDU_GET_CAPABILITIES:
    case AVRC_PDU_LIST_PLAYER_APP_ATTR:
    case AVRC_PDU_LIST_PLAYER_APP_VALUES:
    case AVRC_PDU_GET_CUR_PLAYER_APP_VALUE:
    case AVRC_PDU_GET_PLAYER_APP_ATTR_TEXT:
    case AVRC_PDU_GET_PLAYER_APP_VALUE_TEXT:
    case AVRC_PDU_GET_ELEMENT_ATTR:
    case AVRC_PDU_GET_PLAY_STATUS:
    case AVRC_PDU_SET_ADDRESSED_PLAYER:
    case AVRC_PDU_SET_BROWSED_PLAYER:
    case AVRC_PDU_CHANGE_PATH:
    case AVRC_PDU_GET_FOLDER_ITEMS:
    case AVRC_PDU_GET_ITEM_ATTRIBUTES:
        break;
    }
}

/***************************************************************************
**
** Function         btif_avk_rc_status_cmd_timer_timeout
**
**
** Description      RC status command timeout callback.
**                  This is called from BTU context and switches to BTIF
**                  context to handle the timeout events
** Returns          None
**
***************************************************************************/
static void btif_avk_rc_status_cmd_timer_timeout(void *data)
{
    btif_avk_rc_timer_context_t *p_data = (btif_avk_rc_timer_context_t *)data;

    btif_transfer_context(btif_avk_rc_status_cmd_timeout_handler, 0,
                          (char *)p_data, sizeof(btif_avk_rc_timer_context_t),
                          NULL);
}

/***************************************************************************
**
** Function         register_notification_cmd_vendor
**
** Description      Send Command to register for a Notification ID
**
** Returns          void
**
***************************************************************************/
static bt_status_t register_notification_cmd_vendor(bt_bdaddr_t *bd_addr, uint8_t event_id, uint32_t event_value)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
    int count  = 0;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;

    int index = 0;
    if(bd_addr != NULL)
        index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);

    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX

    tran_status = get_transaction(&p_transaction, index);

    BTIF_TRACE_DEBUG("%s: event_id %d  event_value", __FUNCTION__, event_id, event_value);

    if(BT_STATUS_SUCCESS != tran_status || NULL==p_transaction)
        return BT_STATUS_FAIL;

    avrc_cmd.reg_notif.opcode = AVRC_OP_VENDOR;
    avrc_cmd.reg_notif.status = AVRC_STS_NO_ERROR;
    avrc_cmd.reg_notif.event_id = event_id;
    avrc_cmd.reg_notif.pdu = AVRC_PDU_REGISTER_NOTIFICATION;
    if(AVRC_EVT_PLAY_POS_CHANGED == event_id && event_value == 0)
        event_value = 10;
    avrc_cmd.reg_notif.param = event_value;
    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __FUNCTION__,p_transaction->lbl);
        if (p_msg != NULL)
        {
    BTIF_TRACE_DEBUG("%s: btif_avk_rc_cb[index].rc_handle = %d p_transaction->lbl = %d", __FUNCTION__, btif_avk_rc_cb[index].rc_handle,p_transaction->lbl);
            BTA_AvkVendorCmd(btif_avk_rc_cb[index].rc_handle,p_transaction->lbl,AVRC_CMD_NOTIF,
                data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __FUNCTION__, status);
     }
    if (p_msg != NULL)
        osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}



/***************************************************************************
**
** Function         register_notification_cmd
**
** Description      Send Command to register for a Notification ID
**
** Returns          void
**
***************************************************************************/
static bt_status_t register_notification_cmd(uint8_t rc_handle, uint8_t label, uint8_t event_id, uint32_t event_value)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    int count  = 0;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;

    BTIF_TRACE_DEBUG("%s: rc_handle%d event_id %d  event_value", __FUNCTION__, rc_handle, event_id, event_value);

    avrc_cmd.reg_notif.opcode = AVRC_OP_VENDOR;
    avrc_cmd.reg_notif.status = AVRC_STS_NO_ERROR;
    avrc_cmd.reg_notif.event_id = event_id;
    avrc_cmd.reg_notif.pdu = AVRC_PDU_REGISTER_NOTIFICATION;
    if(AVRC_EVT_PLAY_POS_CHANGED == event_id && event_value == 0)
        event_value = 10;
    avrc_cmd.reg_notif.param = event_value;
    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __FUNCTION__,label);
        if (p_msg != NULL)
        {
            BTA_AvkVendorCmd(rc_handle, label, AVRC_CMD_NOTIF,
                data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __FUNCTION__, status);
     }
    if (p_msg != NULL)
        osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}


/***************************************************************************
**
** Function         register_for_event_notification
**
** Description      Helper function registering notification events
**                  sets an interim response timeout to handle if the remote
**                  does not respond.
** Returns          None
**
***************************************************************************/
static void register_for_event_notification(int index, btif_avk_rc_supported_event_t *p_event)
{
    bt_status_t status;
    rc_transaction_t *p_transaction;
    BTIF_TRACE_DEBUG("%s: ", __FUNCTION__);

    status = get_transaction(&p_transaction, index);
    if (status == BT_STATUS_SUCCESS)
    {
        btif_avk_rc_timer_context_t *p_context = &p_transaction->txn_timer_context;

        BTIF_TRACE_DEBUG("%s: call register_notification_cmd index=%d btif_avk_rc_cb[index].rc_handle=%d p_transaction->lbl=%d",
            __FUNCTION__,index,btif_avk_rc_cb[index].rc_handle,p_transaction->lbl);
        status = register_notification_cmd(btif_avk_rc_cb[index].rc_handle, p_transaction->lbl, p_event->event_id, 0);
        if (status != BT_STATUS_SUCCESS)
        {
            BTIF_TRACE_ERROR("%s Error in Notification registration %d",
                __FUNCTION__, status);
            release_transaction (p_transaction->lbl, index);
            return;
        }
        p_event->label = p_transaction->lbl;
        p_event->status = eREGISTERED;
        p_context->rc_status_cmd.index = index;
        p_context->rc_status_cmd.label = p_transaction->lbl;
        p_context->rc_status_cmd.pdu_id = AVRC_PDU_REGISTER_NOTIFICATION;

        alarm_free(p_transaction->txn_timer);
        p_transaction->txn_timer =
            alarm_new("btif_avk_rc.status_command_txn_timer");
        BTIF_TRACE_DEBUG("%s: call btif_avk_rc_status_cmd_timer_timeout ", __FUNCTION__);

        alarm_set_on_mloop(p_transaction->txn_timer,
                           BTIF_TIMEOUT_RC_INTERIM_RSP_MS,
                           btif_avk_rc_status_cmd_timer_timeout, p_context);
    }
    else
    {
        BTIF_TRACE_ERROR("%s Error No more Transaction label %d",
            __FUNCTION__, status);
    }
}


/*******************************************************************************
**
** Function         btif_avk_rc_ctrl_upstreams_rsp_cmd
**
** Description      Executes AVRC UPSTREAMS response events in btif context.
**
** Returns          void
**
*******************************************************************************/
static void btif_avk_rc_ctrl_upstreams_rsp_cmd(uint8_t event, tAVRC_COMMAND *pavrc_cmd,
                                                         uint8_t label, uint8_t index)
{
    bt_bdaddr_t rc_addr;

    BTIF_TRACE_IMP("%s pdu: %s handle: 0x%x", __FUNCTION__,
        dump_rc_pdu(pavrc_cmd->pdu), btif_avk_rc_cb[index].rc_handle);
    rc_addr = btif_avk_rc_cb[index].rc_addr;
#if (AVRC_CTLR_INCLUDED == TRUE)
    switch (event)
    {
    case AVRC_PDU_SET_ABSOLUTE_VOLUME:
         HAL_CBACK(btif_avk_rc_ctrl_callbacks,setabsvol_cmd_cb,
                 &rc_addr, pavrc_cmd->volume.volume, label);
         break;
    case AVRC_PDU_REGISTER_NOTIFICATION:
         if (pavrc_cmd->reg_notif.event_id == AVRC_EVT_VOLUME_CHANGE)
         {
             HAL_CBACK(btif_avk_rc_ctrl_callbacks,registernotification_absvol_cb, &rc_addr, label);
         }
         break;
    }
#endif
}
/*******************************************************************************
**
** Function         btif_avk_br_ctrl_upstreams_rsp_evt
**
** Description      Executes AVRC UPSTREAMS response events in btif context.
**
** Returns          void
**
*******************************************************************************/
static void btif_avk_br_ctrl_upstreams_rsp_evt(uint16_t event, tAVRC_RESPONSE *pavrc_resp,
                                        uint8_t rsp_type, uint8_t index)
{
    bt_bdaddr_t rc_addr;

    BTIF_TRACE_IMP("%s pdu: %s handle: 0x%x rsp_type:%x", __FUNCTION__,
        dump_rc_pdu(pavrc_resp->pdu), btif_avk_rc_cb[index].rc_handle, rsp_type);

    rc_addr = btif_avk_rc_cb[index].rc_addr;

#if (AVRC_CTLR_INCLUDED == TRUE)
    switch (event)
    {
        case AVRC_PDU_SET_BROWSED_PLAYER:
        {
            btrc_status_t rsp_status;
            rsp_status = (btrc_status_t)pavrc_resp->br_player.status;

            if (rsp_status == AVRC_STS_NO_ERROR) {
                uint32_t num_items = pavrc_resp->br_player.num_items;
                uint16_t charset_id = pavrc_resp->br_player.charset_id;
                uint8_t folder_depth = pavrc_resp->br_player.folder_depth;
                btrc_br_folder_name_t *p_folders = NULL;
                if(folder_depth > 0)
                {
                    size_t buf_size = folder_depth * sizeof(btrc_br_folder_name_t);
                    p_folders = (btrc_br_folder_name_t*)osi_calloc(buf_size);
                    for (int i = 0; i < folder_depth; i++) {
                        p_folders[i].str_len = 0;
                         if (pavrc_resp->br_player.p_folders[i].str_len > 0)
                         {
                              memset(p_folders[i].p_str, 0, BTRC_MAX_ATTR_STR_LEN * sizeof(uint8_t));
                              int len = BTRC_MAX_ATTR_STR_LEN - 1;
                              if(pavrc_resp->br_player.p_folders[i].str_len < len)
                                len = pavrc_resp->br_player.p_folders[i].str_len;
                              p_folders[i].str_len = len;
                              memcpy(p_folders[i].p_str, pavrc_resp->br_player.p_folders[i].p_str, len);
                         }
                    }

                }
                HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,setbrowsedplayer_vendor_cb, &rc_addr,
                                        rsp_status,num_items,charset_id,folder_depth,p_folders);
           }
            else
            {
                BTIF_TRACE_ERROR("%s: Error %d", __func__, rsp_status);
                HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,setbrowsedplayer_vendor_cb, &rc_addr,
                                        rsp_status,0,0,0,NULL);
            }
        }
        break;
        case AVRC_PDU_CHANGE_PATH:
        {
            btrc_status_t rsp_status;
            uint32_t num_items = 0;
            rsp_status = (btrc_status_t)pavrc_resp->chg_path.status;
            if(BTRC_STS_NO_ERROR == rsp_status)
                num_items = pavrc_resp->chg_path.num_items;
            else
                BTIF_TRACE_IMP("%s AVRC_PDU_CHANGE_PATH error: rsp_status:%x", __FUNCTION__, rsp_status);

            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,changepath_vendor_cb, &rc_addr,
                                    rsp_status,num_items);

        }
        break;
        case AVRC_PDU_SEARCH:
        {
            btrc_status_t rsp_status;
            uint32_t uid_counter = 0;
            uint32_t num_items = 0;
            rsp_status = (btrc_status_t)pavrc_resp->search.status;
            if(BTRC_STS_NO_ERROR == rsp_status)
            {
                uid_counter = pavrc_resp->search.uid_counter;
                num_items = pavrc_resp->search.num_items;
            }
            else
                BTIF_TRACE_IMP("%s AVRC_PDU_SEARCH error: rsp_status:%x", __FUNCTION__, rsp_status);

            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,search_vendor_cb, &rc_addr,
                                    rsp_status, uid_counter, num_items);

        }
        break;
        case AVRC_PDU_GET_ITEM_ATTRIBUTES:
        {
            btrc_status_t rsp_status;
            rsp_status = (btrc_status_t)pavrc_resp->get_attrs.status;
            uint32_t num_attr = 0;
            btrc_element_attr_val_t *p_attrs = NULL;

            if(BTRC_STS_NO_ERROR == rsp_status)
            {
                num_attr = pavrc_resp->get_attrs.num_attrs;
                if(num_attr > 0)
                {
                    size_t buf_size = num_attr * sizeof(btrc_element_attr_val_t);
                    p_attrs = (btrc_element_attr_val_t*)osi_calloc(buf_size);
                    for (int i = 0; i < num_attr; i++) {
                        p_attrs[i].attr_id = pavrc_resp->get_attrs.p_attrs[i].attr_id;

                        memset(p_attrs[i].text, 0, BTRC_MAX_ATTR_STR_LEN * sizeof(uint8_t));
                        int len = BTRC_MAX_ATTR_STR_LEN - 1;
                        if(pavrc_resp->get_attrs.p_attrs[i].name.str_len < len)
                        len = pavrc_resp->get_attrs.p_attrs[i].name.str_len;
                        memcpy(p_attrs[i].text, pavrc_resp->get_attrs.p_attrs[i].name.p_str, len);
                    }

                }
            }
            else
                BTIF_TRACE_IMP("%s AVRC_PDU_GET_ITEM_ATTRIBUTES error: rsp_status:%x", __FUNCTION__, rsp_status);
            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,getitemattributes_vendor_cb, &rc_addr,
                                    rsp_status, num_attr, p_attrs);
        }
        break;
        case AVRC_PDU_GET_FOLDER_ITEMS:
        {
            btrc_status_t rsp_status;
            rsp_status = (btrc_status_t)pavrc_resp->get_items.status;
            uint32_t num_items = 0;
            btrc_folder_items_t *p_folders = NULL;
            if(BTRC_STS_NO_ERROR == rsp_status)
            {
                num_items = pavrc_resp->get_items.item_count;
                if(num_items > 0)
                {
                    size_t buf_size = num_items * sizeof(btrc_folder_items_t);
                    p_folders = (btrc_folder_items_t*)osi_calloc(buf_size);
                    for (int i = 0; i < num_items; i++) {
                        p_folders[i].item_type = pavrc_resp->get_items.p_item_list[i].item_type;
                         if (pavrc_resp->get_items.p_item_list[i].item_type == AVRC_ITEM_PLAYER)
                         {
                              p_folders[i].player.player_id = pavrc_resp->get_items.p_item_list[i].u.player.player_id;
                              p_folders[i].player.major_type = pavrc_resp->get_items.p_item_list[i].u.player.major_type;
                              p_folders[i].player.sub_type = pavrc_resp->get_items.p_item_list[i].u.player.sub_type;
                              p_folders[i].player.play_status = pavrc_resp->get_items.p_item_list[i].u.player.play_status;
                              for(int n=0; n < AVRC_FEATURE_MASK_SIZE; n++)
                                    p_folders[i].player.features[n] = pavrc_resp->get_items.p_item_list[i].u.player.features[n];

                              memset(p_folders[i].player.name, 0, BTRC_MAX_ATTR_STR_LEN * sizeof(uint8_t));
                              int len = BTRC_MAX_ATTR_STR_LEN - 1;
                              if(pavrc_resp->get_items.p_item_list[i].u.player.name.str_len < len)
                                len = pavrc_resp->get_items.p_item_list[i].u.player.name.str_len;
                              memcpy(p_folders[i].player.name, pavrc_resp->get_items.p_item_list[i].u.player.name.p_str, len);
                         }
                         else if(pavrc_resp->get_items.p_item_list[i].item_type == AVRC_ITEM_FOLDER)
                         {
                             p_folders[i].folder.type = pavrc_resp->get_items.p_item_list[i].u.folder.type;
                             p_folders[i].folder.playable = pavrc_resp->get_items.p_item_list[i].u.folder.playable;
                             for(int n=0; n < AVRC_UID_SIZE; n++)
                                   p_folders[i].folder.uid[n] = pavrc_resp->get_items.p_item_list[i].u.folder.uid[n];

                             memset(p_folders[i].folder.name, 0, BTRC_MAX_ATTR_STR_LEN * sizeof(uint8_t));
                             int len = BTRC_MAX_ATTR_STR_LEN - 1;
                             if(pavrc_resp->get_items.p_item_list[i].u.folder.name.str_len < len)
                               len = pavrc_resp->get_items.p_item_list[i].u.folder.name.str_len;
                             memcpy(p_folders[i].folder.name, pavrc_resp->get_items.p_item_list[i].u.folder.name.p_str, len);
                         }
                         else if(pavrc_resp->get_items.p_item_list[i].item_type == AVRC_ITEM_MEDIA)
                         {
                             int num_attrs = pavrc_resp->get_items.p_item_list[i].u.media.attr_count;
                             p_folders[i].media.num_attrs = num_attrs;
                             p_folders[i].media.type = pavrc_resp->get_items.p_item_list[i].u.media.type;

                             for(int n=0; n < AVRC_UID_SIZE; n++)
                                   p_folders[i].media.uid[n] = pavrc_resp->get_items.p_item_list[i].u.media.uid[n];

                             memset(p_folders[i].media.name, 0, BTRC_MAX_ATTR_STR_LEN * sizeof(uint8_t));
                             int len = BTRC_MAX_ATTR_STR_LEN - 1;
                             if(pavrc_resp->get_items.p_item_list[i].u.media.name.str_len < len)
                               len = pavrc_resp->get_items.p_item_list[i].u.media.name.str_len;
                             memcpy(p_folders[i].media.name, pavrc_resp->get_items.p_item_list[i].u.media.name.p_str, len);
                             if(num_attrs > 0)
                             {
                                 buf_size = num_attrs * sizeof(btrc_element_attr_val_t);
                                 p_folders[i].media.p_attrs = (btrc_element_attr_val_t*)osi_calloc(buf_size);
                                 for(int n =0; n < num_attrs; n++)
                                 {
                                     p_folders[i].media.p_attrs[n].attr_id = pavrc_resp->get_items.p_item_list[i].u.media.p_attr_list[n].attr_id;
                                     memset(p_folders[i].media.p_attrs[n].text, 0, BTRC_MAX_ATTR_STR_LEN * sizeof(uint8_t));
                                     int len = BTRC_MAX_ATTR_STR_LEN - 1;
                                     if(pavrc_resp->get_items.p_item_list[i].u.media.p_attr_list[n].name.str_len < len)
                                       len = pavrc_resp->get_items.p_item_list[i].u.media.p_attr_list[n].name.str_len;
                                     memcpy(p_folders[i].media.p_attrs[n].text, pavrc_resp->get_items.p_item_list[i].u.media.p_attr_list[n].name.p_str, len);
                                 }
                              }
                           }
                        }
                }
            }
            else
                BTIF_TRACE_IMP("%s AVRC_PDU_GET_FOLDER_ITEMS error: rsp_status:%x", __FUNCTION__, rsp_status);

            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,getfolderitems_cb, &rc_addr, startItem, endItem,
                                    rsp_status, num_items, p_folders);
        }
        break;

    }

#endif
}



/*******************************************************************************
**
** Function         btif_avk_rc_ctrl_upstreams_rsp_evt
**
** Description      Executes AVRC UPSTREAMS response events in btif context.
**
** Returns          void
**
*******************************************************************************/
static void btif_avk_rc_ctrl_upstreams_rsp_evt(uint16_t event, tAVRC_RESPONSE *pavrc_resp,
                                        uint8_t* p_buf, uint16_t buf_len, uint8_t rsp_type, uint8_t index)
{
    bt_bdaddr_t rc_addr;

    BTIF_TRACE_IMP("%s pdu: %s handle: 0x%x rsp_type:%x", __FUNCTION__,
        dump_rc_pdu(pavrc_resp->pdu), btif_avk_rc_cb[index].rc_handle, rsp_type);

    rc_addr = btif_avk_rc_cb[index].rc_addr;

#if (AVRC_CTLR_INCLUDED == TRUE)
    switch (event)
    {
        case AVRC_PDU_GET_CAPABILITIES:
        {
            BTIF_TRACE_DEBUG("%s: case AVRC_PDU_GET_CAPABILITIES ", __FUNCTION__);
            btif_avk_rc_supported_event_t *p_event;


            int xx = 0;
            if (pavrc_resp->get_caps.capability_id == AVRC_CAP_COMPANY_ID)
            {
                BTIF_TRACE_DEBUG("%s: case AVRC_PDU_GET_CAPABILITIES AVRC_CAP_COMPANY_ID ", __FUNCTION__);
                getcapabilities_cmd(index, AVRC_CAP_EVENTS_SUPPORTED);
                btif_avk_rc_cb[index].rc_caps_company.count = pavrc_resp->get_caps.count;
                for (xx = 0; xx < pavrc_resp->get_caps.count; xx++)
                    btif_avk_rc_cb[index].rc_caps_company.param.company_id[xx] = pavrc_resp->get_caps.param.company_id[xx];

                BTIF_TRACE_IMP("btif_avk_rc_ctrl_upstreams_rsp_evt HAL_CBACK AVRC_CAP_EVENTS_SUPPORTED");
                HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks, getcap_rsp_vendor_cb, &rc_addr,
                AVRC_CAP_COMPANY_ID,(uint32_t *)btif_avk_rc_cb[index].rc_caps_company.param.company_id,
                btif_avk_rc_cb[index].rc_caps_company.count, rsp_type);
            }
            else if (pavrc_resp->get_caps.capability_id == AVRC_CAP_EVENTS_SUPPORTED)
            {
                btif_avk_rc_cb[index].rc_caps_event.count = pavrc_resp->get_caps.count;            
                BTIF_TRACE_DEBUG("%s: case AVRC_PDU_GET_CAPABILITIES AVRC_CAP_EVENTS_SUPPORTED pavrc_resp->get_caps.count = %d", __FUNCTION__,pavrc_resp->get_caps.count);
                // if browsing is not enabled on DUT, then don't register for browsing related events
                if(enable_avrc_browsing)
                {
                    for (xx = 0; xx < pavrc_resp->get_caps.count; xx++)
                    {
                        /* Skip registering for Play position change notification */
                        if ((pavrc_resp->get_caps.param.event_id[xx] == AVRC_EVT_PLAY_STATUS_CHANGE)||
                            (pavrc_resp->get_caps.param.event_id[xx] == AVRC_EVT_TRACK_CHANGE)||
                            (pavrc_resp->get_caps.param.event_id[xx] == AVRC_EVT_APP_SETTING_CHANGE)||
                            (pavrc_resp->get_caps.param.event_id[xx] ==AVRC_EVT_ADDR_PLAYER_CHANGE)||
                            (pavrc_resp->get_caps.param.event_id[xx] ==AVRC_EVT_AVAL_PLAYERS_CHANGE)||
                            (pavrc_resp->get_caps.param.event_id[xx] ==AVRC_EVT_NOW_PLAYING_CHANGE))
                        {
                            BTIF_TRACE_DEBUG("%s: case AVRC_PDU_GET_CAPABILITIES AVRC_CAP_EVENTS_SUPPORTED event_id%d = 0x%x", __FUNCTION__,xx,pavrc_resp->get_caps.param.event_id[xx]);
                            p_event = (btif_avk_rc_supported_event_t *)osi_malloc(sizeof(btif_avk_rc_supported_event_t));
                            p_event->event_id = pavrc_resp->get_caps.param.event_id[xx];
                            p_event->status = eNOT_REGISTERED;
                            list_append(btif_avk_rc_cb[index].rc_supported_event_list, p_event);
                        }
                        btif_avk_rc_cb[index].rc_caps_event.param.event_id[xx] = pavrc_resp->get_caps.param.event_id[xx];
                    }
                }
                else
                {
                    for (xx = 0; xx < pavrc_resp->get_caps.count; xx++)
                    {
                        /* Skip registering for Play position change notification */
                        if ((pavrc_resp->get_caps.param.event_id[xx] == AVRC_EVT_PLAY_STATUS_CHANGE)||
                            (pavrc_resp->get_caps.param.event_id[xx] == AVRC_EVT_TRACK_CHANGE))
                        {
                            BTIF_TRACE_DEBUG("%s: case AVRC_PDU_GET_CAPABILITIES AVRC_CAP_EVENTS_SUPPORTED event_id%d = 0x%x ", __FUNCTION__,xx,pavrc_resp->get_caps.param.event_id[xx]);
                            p_event = (btif_avk_rc_supported_event_t *)osi_malloc(sizeof(btif_avk_rc_supported_event_t));
                            p_event->event_id = pavrc_resp->get_caps.param.event_id[xx];
                            p_event->status = eNOT_REGISTERED;
                            list_append(btif_avk_rc_cb[index].rc_supported_event_list, p_event);
                        }
                        btif_avk_rc_cb[index].rc_caps_event.param.event_id[xx] = pavrc_resp->get_caps.param.event_id[xx];
                    }
                }
                if (!list_is_empty(btif_avk_rc_cb[index].rc_supported_event_list))
                {
                    p_event = (btif_avk_rc_supported_event_t *)list_front(btif_avk_rc_cb[index].rc_supported_event_list);
                    if (p_event != NULL)
                    {
                         BTIF_TRACE_DEBUG("%s: case AVRC_PDU_GET_CAPABILITIES AVRC_CAP_EVENTS_SUPPORTED register_for_event_notification", __FUNCTION__);
                         register_for_event_notification(index, p_event);
                    }
                }

                BTIF_TRACE_IMP("btif_avk_rc_ctrl_upstreams_rsp_evt HAL_CBACK AVRC_CAP_EVENTS_SUPPORTED");
                HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks, getcap_rsp_vendor_cb, &rc_addr,
                AVRC_CAP_EVENTS_SUPPORTED,(uint32_t *)btif_avk_rc_cb[index].rc_caps_event.param.event_id,
                btif_avk_rc_cb[index].rc_caps_event.count, rsp_type);               
            }
        }
            break;
        case AVRC_PDU_LIST_PLAYER_APP_ATTR:
        {
            int xx = 0;
            uint8_t  *p_byte_array = NULL;
            if (pavrc_resp->list_app_attr.num_attr > 0)
            {
                p_byte_array = (uint8_t*)osi_malloc(pavrc_resp->list_app_attr.num_attr);
                if (p_byte_array == NULL)
                    return;
            }
            for (xx = 0; xx < pavrc_resp->list_app_attr.num_attr; xx++)
            {
                p_byte_array[xx] = pavrc_resp->list_app_attr.attrs[xx];
            }
            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,listplayerappsettingattrib_rsp_vendor_cb, &rc_addr,
                                 p_byte_array,pavrc_resp->list_app_attr.num_attr, rsp_type);
            if (p_byte_array != NULL)
                osi_free(p_byte_array);
        }
            break;
        case AVRC_PDU_LIST_PLAYER_APP_VALUES:
        {
            int xx = 0;
            uint8_t  *p_byte_array = NULL;
            if (pavrc_resp->list_app_values.num_val > 0)
            {
                p_byte_array = (uint8_t*)osi_malloc(pavrc_resp->list_app_values.num_val);
                if (p_byte_array == NULL)
                    return;
            }
            for (xx = 0; xx < pavrc_resp->list_app_values.num_val; xx++)
            {
                p_byte_array[xx] = pavrc_resp->list_app_values.vals[xx];
            }
            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,listplayerappsettingvalue_rsp_vendor_cb, &rc_addr,
                                    p_byte_array,pavrc_resp->list_app_values.num_val, rsp_type);
            if (p_byte_array != NULL)
                osi_free(p_byte_array);
        }
            break;
        case AVRC_PDU_GET_CUR_PLAYER_APP_VALUE:
        {
            int xx = 0;
            uint8_t  *p_supported_ids;
            uint8_t  *p_byte_array;

            if (pavrc_resp->get_cur_app_val.num_val <= 0)
            {
                HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,currentplayerappsetting_rsp_vendor_cb, &rc_addr,NULL,
                                        NULL,pavrc_resp->get_cur_app_val.num_val, rsp_type);
                break;
            }
            p_supported_ids = (uint8_t*)osi_malloc(pavrc_resp->get_cur_app_val.num_val);
            if (p_supported_ids == NULL)
                return;
            p_byte_array = (uint8_t*)osi_malloc(pavrc_resp->get_cur_app_val.num_val);
            if (p_byte_array == NULL)
            {
                if (p_supported_ids != NULL)
                    osi_free(p_supported_ids);
                return;
            }
            for (xx = 0; xx < pavrc_resp->get_cur_app_val.num_val; xx++)
            {
                p_supported_ids[xx] = pavrc_resp->get_cur_app_val.p_vals[xx].attr_id;
                p_byte_array[xx] = pavrc_resp->get_cur_app_val.p_vals[xx].attr_val;
            }
            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,currentplayerappsetting_rsp_vendor_cb, &rc_addr,p_supported_ids,
                                    p_byte_array,pavrc_resp->get_cur_app_val.num_val, rsp_type);
            osi_free(pavrc_resp->get_cur_app_val.p_vals);
            osi_free(p_byte_array);
            osi_free(p_supported_ids);
        }
            break;
        case AVRC_PDU_SET_PLAYER_APP_VALUE:
        {
            HAL_CBACK(btif_avk_rc_ctrl_callbacks,setplayerappsetting_rsp_cb, &rc_addr, rsp_type);
        }
            break;
        case AVRC_PDU_REGISTER_NOTIFICATION:
        {
            BTIF_TRACE_EVENT(" %s AVRC_PDU_REGISTER_NOTIFICATION %d", __FUNCTION__, buf_len);

            btrc_event_id_t event_id = (btrc_event_id_t)pavrc_resp->reg_notif.event_id;
            btrc_notification_type_t type = (btrc_notification_type_t)rsp_type;
            // map rsp_type to hal values for interim and changed.
            //If something else, send raw values.
            switch(rsp_type) {
                case AVRC_RSP_INTERIM:
                    type =  BTRC_NOTIFICATION_TYPE_INTERIM;
                break;
                case AVRC_RSP_CHANGED:
                    type = BTRC_NOTIFICATION_TYPE_CHANGED;
                break;
            }
            btrc_register_notification_t param;

            switch(event_id)
            {
                case AVRC_EVT_PLAY_STATUS_CHANGE:
                    param.play_status = (btrc_play_status_t)pavrc_resp->reg_notif.param.play_status;
                    break;

                case AVRC_EVT_TRACK_CHANGE:
                    for(int i=0; i < BTRC_UID_SIZE; i++)
                        param.track[i] = pavrc_resp->reg_notif.param.track[i];
                    break;

                case AVRC_EVT_APP_SETTING_CHANGE:
                    memcpy(&param.player_setting, &pavrc_resp->reg_notif.param.player_setting,
                        sizeof(btrc_register_notification_t));
                break;

                case AVRC_EVT_PLAY_POS_CHANGED:
                    param.song_pos = pavrc_resp->reg_notif.param.play_pos;
                break;

                case AVRC_EVT_NOW_PLAYING_CHANGE:
                break;

                case AVRC_EVT_AVAL_PLAYERS_CHANGE:
                break;

                case AVRC_EVT_ADDR_PLAYER_CHANGE:
                    param.addr_player_changed.player_id = pavrc_resp->reg_notif.param.addr_player.player_id;
                    param.addr_player_changed.uid_counter= pavrc_resp->reg_notif.param.addr_player.uid_counter;
                break;

                case AVRC_EVT_VOLUME_CHANGE:
                    param.volume = pavrc_resp->reg_notif.param.volume;
                break;

                default:
                break;
        }

            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,notification_rsp_vendor_cb, &rc_addr,
                                    event_id, type, &param);
        }
            break;
        case AVRC_PDU_GET_ELEMENT_ATTR:
        {
            uint8_t  *p_byte_array;

            if (pavrc_resp->get_attrs.num_attrs <= 0)
            {
                HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,getelementattrib_rsp_vendor_cb, &rc_addr,
                  pavrc_resp->get_attrs.num_attrs,NULL, rsp_type);
                break;
            }
            size_t buf_size = pavrc_resp->get_attrs.num_attrs * sizeof(btrc_element_attr_val_t);
            btrc_element_attr_val_t* p_attr =
                (btrc_element_attr_val_t*)osi_calloc(buf_size);

            if (p_attr == NULL)
                return;

            for (int i = 0; i < pavrc_resp->get_attrs.num_attrs; i++) {
                p_attr[i].attr_id = pavrc_resp->get_attrs.p_attrs[i].attr_id;
                 if (pavrc_resp->get_attrs.p_attrs[i].name.str_len > 0)
                 {
                      memset(p_attr[i].text, 0, BTRC_MAX_ATTR_STR_LEN * sizeof(uint8_t));
                      int len = BTRC_MAX_ATTR_STR_LEN - 1;
                      if(pavrc_resp->get_attrs.p_attrs[i].name.str_len < len)
                        len = pavrc_resp->get_attrs.p_attrs[i].name.str_len;
                      memcpy(p_attr[i].text, pavrc_resp->get_attrs.p_attrs[i].name.p_str, len);
                 }
            }
            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,getelementattrib_rsp_vendor_cb, &rc_addr,
              pavrc_resp->get_attrs.num_attrs, p_attr, rsp_type);
            osi_free(p_attr);
        }
            break;
        case AVRC_PDU_GET_PLAY_STATUS:
        {           
            btrc_play_status_t play_status;
            uint32_t song_len, song_pos;

            song_len = pavrc_resp->get_play_status.song_len;
            song_pos = pavrc_resp->get_play_status.song_pos;
            play_status = (btrc_play_status_t)pavrc_resp->get_play_status.play_status;

            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,getplaystatus_rsp_vendor_cb, &rc_addr,
                                    play_status,song_len, song_pos);

        }
            break;
        case AVRC_PDU_SET_ADDRESSED_PLAYER:
        {
            btrc_status_t rsp_status;
            rsp_status = (btrc_status_t)pavrc_resp->addr_player.status;
            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,setaddressedplayer_vendor_cb, &rc_addr,
                                    rsp_status);
        }
            break;
        case AVRC_PDU_PLAY_ITEM:
        {
            btrc_status_t rsp_status;
            rsp_status = (btrc_status_t)pavrc_resp->play_item.status;
            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,playitem_vendor_cb, &rc_addr,
                                    rsp_status);
        }
            break;
        case AVRC_PDU_ADD_TO_NOW_PLAYING:
        {
            btrc_status_t rsp_status;
            rsp_status = (btrc_status_t)pavrc_resp->add_to_play.status;
            HAL_CBACK(btif_avk_rc_ctrl_vendor_callbacks,addtonowplaying_vendor_cb, &rc_addr,
                                    rsp_status);
        }
            break;


        default:
            return;
    }
#endif
}
#endif

/************************************************************************************
**  AVRCP API Functions
************************************************************************************/

/*******************************************************************************
**
** Function         init_ctrl
**
** Description      Initializes the AVRC interface
**
** Returns          bt_status_t
**
*******************************************************************************/
static bt_status_t init_ctrl(btrc_ctrl_callbacks_t* callbacks )
{
    bt_status_t result = BT_STATUS_SUCCESS;

    BTIF_TRACE_EVENT("## sink side %s ##", __FUNCTION__);

    if (btif_avk_rc_ctrl_callbacks)
        return BT_STATUS_DONE;

    /* Controller is used only for Certification purposes.
     * In normal case AVRCP controller will not be used, hence
     * updating this is required.
     */
    btif_avk_rc_ctrl_callbacks = callbacks;
    memset (&btif_avk_rc_cb, 0, sizeof(btif_avk_rc_cb));
    btif_avk_rc_cb[BTIF_AVK_RC_DEFAULT_INDEX].rc_vol_label=MAX_LABEL;

    return result;
}

/*******************************************************************************
**
** Function         init_ctrl_vendor
**
** Description      Initializes the AVRC controller vendor interface
**
** Returns          bt_status_t
**
*******************************************************************************/
static bt_status_t init_ctrl_vendor(btrc_ctrl_vendor_callbacks_t* callbacks, int max_connections )
{
    int i;
    char value[PROPERTY_VALUE_MAX] = {'\0'};
    bt_status_t result = BT_STATUS_SUCCESS;
    btif_avk_rc_ctrl_vendor_callbacks = callbacks;
    btif_max_rc_clients = max_connections;
    memset (&btif_avk_rc_cb, 0, sizeof(btif_avk_rc_cb));
    for (i = 0; i < btif_max_rc_clients; i++)
    {
       btif_avk_rc_cb[i].rc_vol_label=MAX_LABEL;
       btif_avk_rc_cb[i].rc_supported_event_list = list_new(osi_free);
    }
    property_get("persist.bluetooth.disablebrowsing", value, "false");
    enable_avrc_browsing = (strcmp(value, "false") == 0);
    lbl_init();
    return result;
}

#if (AVRC_CTLR_INCLUDED == TRUE)
/***************************************************************************
 *
 * Function         clear_cmd_timeout
 *
 * Description      helper function to stop the command timeout timer
 * Returns          None
 *
 **************************************************************************/
static void clear_cmd_timeout(uint8_t label, uint8_t index) {
  BTIF_TRACE_DEBUG("clear_cmd_timeout");
  rc_transaction_t* p_txn;
  if (FALSE==btif_avk_rc_cb[index].device.transaction[label].in_use)
    p_txn = NULL;
  else
    p_txn = &(btif_avk_rc_cb[index].device.transaction[label]);
  if (p_txn == NULL) {
    BTIF_TRACE_ERROR("%s: Error in transaction label lookup", __func__);
    return;
  }

  if (p_txn->txn_timer != NULL) alarm_cancel(p_txn->txn_timer);
}

/***************************************************************************
 *
 * Function         handle_notification_response
 *
 * Description      Main handler for notification responses to registered events
 *                  1. Register for unregistered event(in interim response path)
 *                  2. Release transaction label if not interim response
 *                  3. Reregister for events on getting changed response
 * Returns          None
 *
 **************************************************************************/
static void handle_notification_response(tBTA_AVK_META_MSG* pmeta_msg,
                                         tAVRC_REG_NOTIF_RSP* p_rsp) {
    uint8_t index;
    index = btif_avk_rc_get_idx_by_rc_handle(pmeta_msg->rc_handle);
    btif_avk_rc_cb_t p_dev = btif_avk_rc_cb[index];

  //btif_rc_device_cb_t* p_dev =
 //     btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

 // if (p_dev == NULL) {
  //  BTIF_TRACE_ERROR("%s: p_dev NULL", __func__);
  //  return;
  //}


  if (pmeta_msg->code == AVRC_RSP_INTERIM) {
    btif_avk_rc_supported_event_t* p_event = NULL;
    list_node_t* node;

    BTIF_TRACE_DEBUG("%s: Interim response: 0x%2X ", __func__, p_rsp->event_id);


    list_foreach(p_dev.rc_supported_event_list,
                  avk_iterate_supported_event_list_for_interim_rsp, &p_rsp->event_id);

    node = list_begin(p_dev.rc_supported_event_list);

    while (node != NULL) {
      p_event = (btif_avk_rc_supported_event_t*)list_node(node);
      if ((p_event != NULL) && (p_event->status == eNOT_REGISTERED)) {
        register_for_event_notification(index, p_event);
        break;
      }
      node = list_next(node);
      p_event = NULL;
    }
  }else
  {
    BTIF_TRACE_DEBUG(" Releasing label = %d",pmeta_msg->label);
    release_transaction(pmeta_msg->label, index);
  }

  if (pmeta_msg->code == AVRC_RSP_CHANGED) {
    btif_avk_rc_supported_event_t* p_event = NULL;
    list_node_t* node;

    BTIF_TRACE_DEBUG("%s: Notification completed: 0x%2X ", __func__, p_rsp->event_id);

    node = list_begin(p_dev.rc_supported_event_list);

    while (node != NULL) {
      p_event = (btif_avk_rc_supported_event_t*)list_node(node);
      if (p_event != NULL && p_event->event_id == p_rsp->event_id) {
        p_event->status = eNOT_REGISTERED;
        BTIF_TRACE_DEBUG(" re-register for changed notification  ");
        register_for_event_notification(index, p_event);
        break;
      }
      node = list_next(node);
    }

}
}
/***************************************************************************
**
** Function         handle_avk_rc_metamsg_rsp
**
** Description      Handle RC metamessage response
**
** Returns          void
**
***************************************************************************/
static void handle_avk_rc_metamsg_rsp(tBTA_AVK_META_MSG *pmeta_msg)
{
    uint8_t index;
    tAVRC_RESPONSE    avrc_response = {0};
    uint8_t             scratch_buf[4096] = {0};// maximum size that can be used
    uint16_t            buf_len;
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    index = btif_avk_rc_get_idx_by_rc_handle(pmeta_msg->rc_handle);

    BTIF_TRACE_DEBUG(" %s opcode = %d rsp_code = %d  ",__FUNCTION__,
                        pmeta_msg->p_msg->hdr.opcode,pmeta_msg->code);

    status=AVRC_Ctrl_ParsResponse(pmeta_msg->p_msg, &avrc_response, scratch_buf, &buf_len);
    BTIF_TRACE_DEBUG(" pdu = %d rsp_status = %d",avrc_response.pdu,
                                pmeta_msg->p_msg->vendor.hdr.ctype);

    if((AVRC_OP_VENDOR==pmeta_msg->p_msg->hdr.opcode)&&
                (pmeta_msg->code >= AVRC_RSP_NOT_IMPL)&&
                (pmeta_msg->code <= AVRC_RSP_INTERIM))
    {

        if (avrc_response.pdu == AVRC_PDU_REGISTER_NOTIFICATION)
        {
            handle_notification_response(pmeta_msg, &avrc_response.reg_notif);
            if(pmeta_msg->code == AVRC_RSP_INTERIM)
            {
                BTIF_TRACE_DEBUG(" Don't release transaction label ");
                /* Don't free the transaction Id, but cancel the timer */
                clear_cmd_timeout(pmeta_msg->label, index);
            }
        }
        else
        {
            BTIF_TRACE_DEBUG(" Releasing label = %d",pmeta_msg->label);
            release_transaction(pmeta_msg->label, index);
        }
        BTIF_TRACE_DEBUG("%s: btif_avk_rc_ctrl_upstreams_rsp_evt ", __FUNCTION__);
        btif_avk_rc_ctrl_upstreams_rsp_evt((uint16_t)avrc_response.rsp.pdu, &avrc_response,
                               scratch_buf, buf_len,pmeta_msg->p_msg->vendor.hdr.ctype, index);
    }
    else if(AVRC_OP_BROWSE == pmeta_msg->p_msg->hdr.opcode)
    {
        BTIF_TRACE_DEBUG("%s AVRC_OP_BROWSE pdu %d", __func__, avrc_response.pdu);
        btif_avk_br_ctrl_upstreams_rsp_evt((uint16_t)avrc_response.rsp.pdu, &avrc_response,
            pmeta_msg->p_msg->vendor.hdr.ctype, index);
        BTIF_TRACE_DEBUG(" Releasing label = %d",pmeta_msg->label);
        release_transaction(pmeta_msg->label, index);
    }
    else
    {
        BTIF_TRACE_DEBUG("%s:Invalid Vendor Command  code: %d len: %d. Not processing it.",
        __FUNCTION__, pmeta_msg->code, pmeta_msg->len);
        return;
    }
}

/***************************************************************************
**
** Function         handle_avk_rc_metamsg_cmd
**
** Description      Handle RC metamessage response
**
** Returns          void
**
***************************************************************************/
static void handle_avk_rc_metamsg_cmd(tBTA_AVK_META_MSG *pmeta_msg)
{
    uint8_t index;  /*For RC it is zero*/
    tAVRC_COMMAND    avrc_cmd = {0};
    uint8_t             scratch_buf[4096] = {0};
    uint16_t            buf_len;
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    index = btif_avk_rc_get_idx_by_rc_handle(pmeta_msg->rc_handle);

    BTIF_TRACE_DEBUG(" %s opcode = %d rsp_code = %d  ",__FUNCTION__,
                       pmeta_msg->p_msg->hdr.opcode,pmeta_msg->code);
    if((AVRC_OP_VENDOR==pmeta_msg->p_msg->hdr.opcode)&&
                (pmeta_msg->code <= AVRC_CMD_GEN_INQ))
    {
        buf_len = sizeof(scratch_buf);
        status = AVRC_Ctrl_ParsCommand(pmeta_msg->p_msg, &avrc_cmd);
        BTIF_TRACE_DEBUG("Received vendor command.code,PDU and label: %d, %d,%d",pmeta_msg->code,
                           avrc_cmd.pdu, pmeta_msg->label);

        if (status != AVRC_STS_NO_ERROR)
        {
            /* return error */
            BTIF_TRACE_WARNING("%s: Error in parsing received metamsg command. status: 0x%02x",
                __FUNCTION__, status);
            send_reject_response(pmeta_msg->rc_handle, pmeta_msg->label, avrc_cmd.pdu, status);
        }
        else
        {
            if (avrc_cmd.pdu == AVRC_PDU_REGISTER_NOTIFICATION)
            {
                uint8_t event_id = avrc_cmd.reg_notif.event_id;
                BTIF_TRACE_EVENT("%s:New register notification received.event_id:%s,label:0x%x,code:%x"
                ,__FUNCTION__,dump_rc_notification_event_id(event_id), pmeta_msg->label,pmeta_msg->code);
                btif_avk_rc_cb[index].rc_notif[event_id-1].bNotify = TRUE;
                btif_avk_rc_cb[index].rc_notif[event_id-1].label = pmeta_msg->label;
            }
            else if (avrc_cmd.pdu == AVRC_PDU_SET_ABSOLUTE_VOLUME)
            {
                BTIF_TRACE_EVENT("%s:Abs Volume Cmd Recvd,label:0x%x,code:%x",
                __FUNCTION__, pmeta_msg->label,pmeta_msg->code);
                btif_avk_rc_cb[index].rc_vol_label = pmeta_msg->label;
            }
            btif_avk_rc_ctrl_upstreams_rsp_cmd(avrc_cmd.pdu, &avrc_cmd, pmeta_msg->label, index);
        }
    }
    else
    {
        BTIF_TRACE_DEBUG("%s:Invalid Vendor Command  code: %d len: %d. Not processing it.",
        __FUNCTION__, pmeta_msg->code, pmeta_msg->len);
        return;
    }
}
#endif

/***************************************************************************
**
** Function         cleanup_ctrl
**
** Description      Closes the AVRC Controller interface
**
** Returns          void
**
***************************************************************************/
static void cleanup_ctrl(void)
{
    BTIF_TRACE_EVENT("## %s ##", __FUNCTION__);

    if (btif_avk_rc_ctrl_callbacks)
    {
        btif_avk_rc_ctrl_callbacks = NULL;
    }
    //clear list
    for(int index = 0;index < btif_max_rc_clients; index++) {
        if (btif_avk_rc_cb[index].rc_supported_event_list != NULL) {
            list_clear(btif_avk_rc_cb[index].rc_supported_event_list);
            osi_free(btif_avk_rc_cb[index].rc_supported_event_list);
        }
    }
    memset(&btif_avk_rc_cb, 0, sizeof(btif_avk_rc_cb_t));
    lbl_destroy();
    BTIF_TRACE_EVENT("## %s ## completed", __FUNCTION__);
}

/***************************************************************************
**
** Function         cleanup_ctrl_vendor
**
** Description      Closes the AVRC Controller vendor interface
**
** Returns          void
**
***************************************************************************/
static void cleanup_ctrl_vendor(void)
{
    BTIF_TRACE_EVENT("## %s ##", __FUNCTION__);

    if (btif_avk_rc_ctrl_vendor_callbacks)
    {
        btif_avk_rc_ctrl_vendor_callbacks = NULL;
    }
    BTIF_TRACE_EVENT("## %s ## completed", __FUNCTION__);
}

/***************************************************************************
**
** Function         getcapabilities_cmd
**
** Description      GetCapabilties from Remote(Company_ID, Events_Supported)
**
** Returns          void
**
***************************************************************************/
static bt_status_t getcapabilities_cmd_vendor (bt_bdaddr_t *bd_addr, uint8_t cap_id)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
    bt_status_t tran_status;
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;

    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX


#if (AVRC_CTLR_INCLUDED == TRUE)
    BTIF_TRACE_DEBUG("%s: cap_id %d", __FUNCTION__, cap_id);

    if(btif_avk_rc_cb[index].rc_caps_company.count !=0)
    {
         BTIF_TRACE_ERROR("%s: found the catched caps values, will return these values directly",
                             __FUNCTION__);

        if(p_cap_rsp_timer != NULL)
            alarm_free(p_cap_rsp_timer);
        p_cap_rsp_timer = alarm_new("btif_avk_rc.cap_rsp_timer");
        BTIF_TRACE_DEBUG("%s: btif_avk_rc_status_cmd_timer_timeout ", __FUNCTION__);
        uint16_t* pData = (uint16_t *)osi_malloc(sizeof(uint16_t));
        *pData = ((((uint16_t)cap_id << 8) & 0xff00) | (index & 0x00ff));
        BTIF_TRACE_DEBUG("%s: avk_rc_get_caps_cmd_timer_timeout data=%d", __FUNCTION__,*pData);
        alarm_set_on_mloop(p_cap_rsp_timer,
                           BTIF_TIMEOUT_RC_INTERIM_RSP_MS,
                           btif_avk_rc_get_caps_cmd_timer_timeout, (void*)pData);

        status =  BT_STATUS_SUCCESS;
    }
    else
    {
         status = BT_STATUS_NOT_READY;
         BTIF_TRACE_ERROR("%s: no caps value catched, proviously get capabilities process is not finish yet",
                             __FUNCTION__);
    }
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         getcapabilities_cmd
**
** Description      GetCapabilties from Remote(Company_ID, Events_Supported)
**
** Returns          void
**
***************************************************************************/
static bt_status_t getcapabilities_cmd(int index, uint8_t cap_id)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
    bt_status_t tran_status;
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    BTIF_TRACE_DEBUG("%s: index %d cap_id %d", __FUNCTION__, index, cap_id);

    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX

#if (AVRC_CTLR_INCLUDED == TRUE)

    tran_status = get_transaction(&p_transaction, index);
    if(BT_STATUS_SUCCESS != tran_status || NULL==p_transaction)
        return BT_STATUS_FAIL;

    avrc_cmd.get_caps.opcode = AVRC_OP_VENDOR;
    avrc_cmd.get_caps.capability_id = cap_id;
    avrc_cmd.get_caps.pdu = AVRC_PDU_GET_CAPABILITIES;
    avrc_cmd.get_caps.status = AVRC_STS_NO_ERROR;
    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __FUNCTION__,p_transaction->lbl);
        if (p_msg != NULL)
        {
            BTA_AvkVendorCmd(btif_avk_rc_cb[index].rc_handle,p_transaction->lbl,AVRC_CMD_STATUS,
                data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                             __FUNCTION__, status);
     }
    if (p_msg != NULL)
        osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}


/***************************************************************************
**
** Function         list_player_app_setting_attrib_cmd
**
** Description      Get supported List Player Attributes
**
** Returns          void
**
***************************************************************************/
static bt_status_t list_player_app_setting_attrib_cmd_vendor(bt_bdaddr_t *bd_addr)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;

#if (AVRC_CTLR_INCLUDED == TRUE)
    bt_status_t tran_status;

    BTIF_TRACE_DEBUG("%s: ", __FUNCTION__);

    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX

    tran_status = get_transaction(&p_transaction, index);

    if(BT_STATUS_SUCCESS != tran_status || NULL==p_transaction)
        return BT_STATUS_FAIL;

    avrc_cmd.list_app_attr.opcode = AVRC_OP_VENDOR;
    avrc_cmd.list_app_attr.pdu = AVRC_PDU_LIST_PLAYER_APP_ATTR;
    avrc_cmd.list_app_attr.status = AVRC_STS_NO_ERROR;
    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __FUNCTION__,p_transaction->lbl);
        if(NULL!=p_msg)
        {
            BTA_AvkVendorCmd(btif_avk_rc_cb[index].rc_handle,p_transaction->lbl,AVRC_CMD_STATUS,
                data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __FUNCTION__, status);
     }
    if (p_msg != NULL)
        osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         list_player_app_setting_value_cmd
**
** Description      Get values of supported Player Attributes
**
** Returns          void
**
***************************************************************************/
static bt_status_t list_player_app_setting_value_cmd_vendor(bt_bdaddr_t *bd_addr, uint8_t attrib_id)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
#if (AVRC_CTLR_INCLUDED == TRUE)
    bt_status_t tran_status;
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;

    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX

    tran_status = get_transaction(&p_transaction, index);

    BTIF_TRACE_DEBUG("%s: attrib_id %d", __FUNCTION__, attrib_id);

    if(BT_STATUS_SUCCESS != tran_status || NULL==p_transaction)
        return BT_STATUS_FAIL;


    avrc_cmd.list_app_values.attr_id = attrib_id;
    avrc_cmd.list_app_values.opcode = AVRC_OP_VENDOR;
    avrc_cmd.list_app_values.pdu = AVRC_PDU_LIST_PLAYER_APP_VALUES;
    avrc_cmd.list_app_values.status = AVRC_STS_NO_ERROR;
    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __FUNCTION__,p_transaction->lbl);
        if (p_msg != NULL)
        {
            BTA_AvkVendorCmd(btif_avk_rc_cb[index].rc_handle,p_transaction->lbl,AVRC_CMD_STATUS,
                data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __FUNCTION__, status);
     }
    if (p_msg != NULL)
        osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         get_player_app_setting_cmd
**
** Description      Get current values of Player Attributes
**
** Returns          void
**
***************************************************************************/
static bt_status_t get_player_app_setting_cmd_vendor(bt_bdaddr_t *bd_addr, uint8_t num_attrib, uint8_t* attrib_ids)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
    int count  = 0;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;

    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX


    BTIF_TRACE_DEBUG("%s: num attrib_id %d", __FUNCTION__, num_attrib);

    tran_status = get_transaction(&p_transaction, index);
    if(BT_STATUS_SUCCESS != tran_status || NULL==p_transaction)
        return BT_STATUS_FAIL;

    avrc_cmd.get_cur_app_val.opcode = AVRC_OP_VENDOR;
    avrc_cmd.get_cur_app_val.status = AVRC_STS_NO_ERROR;
    avrc_cmd.get_cur_app_val.num_attr = num_attrib;
    avrc_cmd.get_cur_app_val.pdu = AVRC_PDU_GET_CUR_PLAYER_APP_VALUE;

    for (count = 0; count < num_attrib; count++)
    {
     avrc_cmd.get_cur_app_val.attrs[count] = attrib_ids[count];
    }
    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
                BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __FUNCTION__,p_transaction->lbl);
        if (p_msg != NULL)
        {
            BTA_AvkVendorCmd(btif_avk_rc_cb[index].rc_handle,p_transaction->lbl,AVRC_CMD_STATUS,
                data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __FUNCTION__, status);
     }
    if (p_msg != NULL)
        osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         set_player_app_setting_cmd
**
** Description      Set current values of Player Attributes
**
** Returns          void
**
***************************************************************************/
static bt_status_t set_player_app_setting_cmd(bt_bdaddr_t *bd_addr, uint8_t num_attrib, uint8_t* attrib_ids,
                   uint8_t* attrib_vals)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
    int count  = 0;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;

    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX

    BTIF_TRACE_DEBUG("%s: num attrib_id %d", __FUNCTION__, num_attrib);

    tran_status = get_transaction(&p_transaction, index);
    if(BT_STATUS_SUCCESS != tran_status || NULL==p_transaction)
        return BT_STATUS_FAIL;

    avrc_cmd.set_app_val.opcode = AVRC_OP_VENDOR;
    avrc_cmd.set_app_val.status = AVRC_STS_NO_ERROR;
    avrc_cmd.set_app_val.num_val = num_attrib;
    avrc_cmd.set_app_val.pdu = AVRC_PDU_SET_PLAYER_APP_VALUE;
    avrc_cmd.set_app_val.p_vals =
    (tAVRC_APP_SETTING*)osi_malloc(sizeof(tAVRC_APP_SETTING)*num_attrib);
    for (count = 0; count < num_attrib; count++)
    {
        avrc_cmd.set_app_val.p_vals[count].attr_id = attrib_ids[count];
        avrc_cmd.set_app_val.p_vals[count].attr_val = attrib_vals[count];
    }
    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __FUNCTION__,p_transaction->lbl);
        if (p_msg != NULL)
        {
            BTA_AvkVendorCmd(btif_avk_rc_cb[index].rc_handle,p_transaction->lbl,AVRC_CMD_CTRL,
                data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __FUNCTION__, status);
     }
    if (p_msg != NULL)
        osi_free(p_msg);
     osi_free(avrc_cmd.set_app_val.p_vals);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         get_element_attribute_cmd
**
** Description      Get Element Attribute for  attributeIds
**
** Returns          void
**
***************************************************************************/
static bt_status_t get_element_attribute_cmd_vendor (bt_bdaddr_t *bd_addr, uint8_t num_attribute, uint32_t* attribute_id)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
    int count  = 0;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;

    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX


    BTIF_TRACE_DEBUG("%s: num_attribute  %d attribute_id %d",
                   __FUNCTION__, num_attribute, attribute_id);

    tran_status = get_transaction(&p_transaction, index);
    if(BT_STATUS_SUCCESS != tran_status || NULL==p_transaction)
        return BT_STATUS_FAIL;
    avrc_cmd.get_elem_attrs.opcode = AVRC_OP_VENDOR;
    avrc_cmd.get_elem_attrs.status = AVRC_STS_NO_ERROR;
    avrc_cmd.get_elem_attrs.num_attr = num_attribute;
    avrc_cmd.get_elem_attrs.pdu = AVRC_PDU_GET_ELEMENT_ATTR;
    for(int x =0; x < num_attribute && x < AVRC_MAX_ELEM_ATTR_SIZE; x++)
      avrc_cmd.get_elem_attrs.attrs[x] = attribute_id[x];
    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __FUNCTION__,p_transaction->lbl);
        if (p_msg != NULL)
        {
            BTA_AvkVendorCmd(btif_avk_rc_cb[index].rc_handle,p_transaction->lbl,AVRC_CMD_STATUS,
                data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __FUNCTION__, status);
     }
    if (p_msg != NULL)
        osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         get_play_status_cmd
**
** Description      Get Element Attribute for  attributeIds
**
** Returns          void
**
***************************************************************************/
static bt_status_t get_play_status_cmd_vendor(bt_bdaddr_t *bd_addr)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;

    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX


    BTIF_TRACE_DEBUG("%s: ", __FUNCTION__);
    tran_status = get_transaction(&p_transaction, index);
    if(BT_STATUS_SUCCESS != tran_status || NULL==p_transaction)
        return BT_STATUS_FAIL;

    avrc_cmd.get_play_status.opcode = AVRC_OP_VENDOR;
    avrc_cmd.get_play_status.pdu = AVRC_PDU_GET_PLAY_STATUS;
    avrc_cmd.get_play_status.status = AVRC_STS_NO_ERROR;
    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __FUNCTION__,p_transaction->lbl);
        if (p_msg != NULL)
        {
            BTA_AvkVendorCmd(btif_avk_rc_cb[index].rc_handle,p_transaction->lbl,AVRC_CMD_STATUS,
                data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __FUNCTION__, status);
     }
    if (p_msg != NULL)
        osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}

static bt_status_t set_addressed_player_cmd_vendor(bt_bdaddr_t *bd_addr, uint16_t player_id)
{
    BTIF_TRACE_DEBUG("%s: id %d", __FUNCTION__, player_id);
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;

    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX
    CHECK_AVK_BR_CONNECTED_BY_IDX

    avrc_cmd.addr_player.pdu = AVRC_PDU_SET_ADDRESSED_PLAYER;
    avrc_cmd.addr_player.status = AVRC_STS_NO_ERROR;
    avrc_cmd.addr_player.player_id = player_id;

    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status != AVRC_STS_NO_ERROR) {
        BTIF_TRACE_ERROR("%s: failed to build command status %d", __func__, status);
        return BT_STATUS_FAIL;
    }

    tran_status = get_transaction(&p_transaction, index);

    if (tran_status != BT_STATUS_SUCCESS || p_transaction == NULL) {
        osi_free(p_msg);
        BTIF_TRACE_ERROR("%s: failed to obtain txn details. status: 0x%02x",
                         __func__, tran_status);
        return BT_STATUS_FAIL;
    }

    BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d", __func__,
                   p_transaction->lbl);
    BTA_AvkMetaCmd(btif_avk_rc_cb[index].rc_handle, p_transaction->lbl, AVRC_CMD_CTRL, p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return BT_STATUS_SUCCESS;;

}

static bt_status_t set_browsed_player_cmd_vendor(bt_bdaddr_t *bd_addr, uint16_t player_id)
{
    BTIF_TRACE_DEBUG("%s: id %d", __FUNCTION__, player_id);
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;

    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX
    CHECK_AVK_BR_CONNECTED_BY_IDX

    avrc_cmd.br_player.pdu = AVRC_PDU_SET_BROWSED_PLAYER;
    avrc_cmd.br_player.opcode = AVRC_OP_BROWSE;
    avrc_cmd.br_player.status = AVRC_STS_NO_ERROR;
    avrc_cmd.br_player.player_id = player_id;

    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status != AVRC_STS_NO_ERROR) {
        BTIF_TRACE_ERROR("%s: failed to build command status %d", __func__, status);
        return BT_STATUS_FAIL;
    }

    tran_status = get_transaction(&p_transaction, index);

    if (tran_status != BT_STATUS_SUCCESS || p_transaction == NULL) {
        osi_free(p_msg);
        BTIF_TRACE_ERROR("%s: failed to obtain txn details. status: 0x%02x",
                         __func__, tran_status);
        return BT_STATUS_FAIL;
    }

    BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d", __func__,
                   p_transaction->lbl);
    BTA_AvkMetaCmd(btif_avk_rc_cb[index].rc_handle, p_transaction->lbl, AVRC_CMD_CTRL, p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return BT_STATUS_SUCCESS;

}

/***************************************************************************
**
** Function         get_item_attributes_cmd_vendor
**
** Description      Get Item Attributes
**
** Returns          bt_status_t
**
***************************************************************************/
static bt_status_t get_item_attributes_cmd_vendor (bt_bdaddr_t *bd_addr,
uint8_t scope_id, uint64_t UID, uint16_t uid_counter, uint8_t num_attrb, btrc_media_attr_t *p_attrs)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
    int count  = 0;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;

    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    if (num_attrb > AVRC_MAX_ELEM_ATTR_SIZE)
    {
        BTIF_TRACE_DEBUG("%s: invalid number of attributes = 0x%02x", __FUNCTION__, num_attrb);
        return BT_STATUS_FAIL;
    }
    CHECK_AVK_RC_CONNECTED_BY_IDX
    CHECK_AVK_BR_CONNECTED_BY_IDX


    BTIF_TRACE_DEBUG("%s: scope_id: %d UID: %lld, uid_counter: 0x%x ,num_attribute  %d",
                   __FUNCTION__, scope_id, UID, uid_counter, num_attrb);

     /* Set the layer specific to point to browse although this should really
     * be done by lower layers and looking at the PDU
     */
    avrc_cmd.get_attrs.pdu = AVRC_PDU_GET_ITEM_ATTRIBUTES;
    avrc_cmd.get_attrs.status = AVRC_STS_NO_ERROR;
    avrc_cmd.get_attrs.scope = scope_id;
    memcpy(avrc_cmd.get_attrs.uid , &UID, sizeof(tAVRC_UID));
    avrc_cmd.get_attrs.uid_counter= uid_counter;
    avrc_cmd.get_attrs.attr_count = num_attrb;
    if(num_attrb != 255)
      avrc_cmd.get_attrs.p_attr_list = (uint32_t*)p_attrs;

    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status != AVRC_STS_NO_ERROR) {
      BTIF_TRACE_ERROR("%s failed to build command status %d", __func__, status);
      return BT_STATUS_FAIL;
    }

    tran_status = get_transaction(&p_transaction, index);
    if (tran_status != BT_STATUS_SUCCESS || p_transaction == NULL) {
      osi_free(p_msg);
      BTIF_TRACE_ERROR("%s: failed to obtain transaction details. status: 0x%02x",
                       __func__, tran_status);
      return BT_STATUS_FAIL;
    }

    BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d", __func__,
                     p_transaction->lbl);
    BTA_AvkMetaCmd(btif_avk_rc_cb[index].rc_handle, p_transaction->lbl, AVRC_CMD_CTRL, p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return BT_STATUS_SUCCESS;
}


static bt_status_t get_folder_items_cmd_vendor(bt_bdaddr_t *bd_addr, uint8_t scope_id, uint32_t start_item,
    uint32_t end_item, uint8_t num_attrb, uint32_t* attrib_ids)
{
    BTIF_TRACE_DEBUG("%s: scope_id 0x%02x start_item %d end_item %d num_attrb %d", __FUNCTION__,
        scope_id, start_item, end_item, num_attrb);
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;

    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    if ((num_attrb > AVRC_MAX_ELEM_ATTR_SIZE)&&(num_attrb != 255))
    {
        BTIF_TRACE_DEBUG("%s: invalid number of attributes = 0x%02x", __FUNCTION__, num_attrb);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX
    CHECK_AVK_BR_CONNECTED_BY_IDX

    /* Set the layer specific to point to browse although this should really
     * be done by lower layers and looking at the PDU
     */
    avrc_cmd.get_items.pdu = AVRC_PDU_GET_FOLDER_ITEMS;
    avrc_cmd.get_items.status = AVRC_STS_NO_ERROR;
    avrc_cmd.get_items.scope = scope_id;
    avrc_cmd.get_items.start_item = start_item;
    avrc_cmd.get_items.end_item = end_item;
    avrc_cmd.get_items.attr_count = num_attrb;

    if((num_attrb != 255)&&(num_attrb != 0))
         avrc_cmd.get_items.p_attr_list = (uint32_t*)attrib_ids;

    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status != AVRC_STS_NO_ERROR) {
      BTIF_TRACE_ERROR("%s failed to build command status %d", __func__, status);
      return BT_STATUS_FAIL;
    }

    tran_status = get_transaction(&p_transaction, index);
    if (tran_status != BT_STATUS_SUCCESS || p_transaction == NULL) {
      osi_free(p_msg);
      BTIF_TRACE_ERROR("%s: failed to obtain transaction details. status: 0x%02x",
                       __func__, tran_status);
      return BT_STATUS_FAIL;
    }

    BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d", __func__,
                     p_transaction->lbl);
    BTA_AvkMetaCmd(btif_avk_rc_cb[index].rc_handle, p_transaction->lbl, AVRC_CMD_CTRL, p_msg);
    startItem = start_item;
    endItem = end_item;
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return BT_STATUS_SUCCESS;


}


static bt_status_t change_folder_path_cmd_vendor(bt_bdaddr_t *bd_addr, uint8_t direction, uint8_t * uid)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;

    if(uid == NULL)
    {
        BTIF_TRACE_DEBUG("%s: uid is NULL", __FUNCTION__);
        return BT_STATUS_FAIL;
    }
    BTIF_TRACE_DEBUG("%s: direction 0x%02x uid %d", __FUNCTION__, direction, *uid);
    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX
    CHECK_AVK_BR_CONNECTED_BY_IDX

    avrc_cmd.chg_path.pdu = AVRC_PDU_CHANGE_PATH;
    avrc_cmd.chg_path.status = AVRC_STS_NO_ERROR;
    avrc_cmd.chg_path.uid_counter = 0;
    avrc_cmd.chg_path.direction = direction;
    memset(&(avrc_cmd.chg_path.folder_uid), 0, AVRC_UID_SIZE * sizeof(uint8_t));
    memcpy((avrc_cmd.chg_path.folder_uid), (uint64_t*)uid, AVRC_UID_SIZE * sizeof(uint8_t));

    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status != AVRC_STS_NO_ERROR) {
      BTIF_TRACE_ERROR("%s failed to build command status %d", __func__, status);
      return BT_STATUS_FAIL;
    }

    tran_status = get_transaction(&p_transaction, index);
    if (tran_status != BT_STATUS_SUCCESS || p_transaction == NULL) {
      osi_free(p_msg);
      BTIF_TRACE_ERROR("%s: failed to obtain transaction details. status: 0x%02x",
                       __func__, tran_status);
      return BT_STATUS_FAIL;
    }

    BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d", __func__,
                     p_transaction->lbl);
    BTA_AvkMetaCmd(btif_avk_rc_cb[index].rc_handle, p_transaction->lbl, AVRC_CMD_CTRL, p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return BT_STATUS_SUCCESS;

}

static bt_status_t search_cmd_vendor(bt_bdaddr_t *bd_addr, uint16_t length, uint8_t* string)
{
    BTIF_TRACE_DEBUG("%s: length %d string %s", __FUNCTION__, length, string);
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;

    if(length <= 0)
    {
        BTIF_TRACE_DEBUG("%s: invalid length", __FUNCTION__);
        return BT_STATUS_FAIL;
    }
    if(string == NULL)
    {
        BTIF_TRACE_DEBUG("%s: string is NULL", __FUNCTION__);
        return BT_STATUS_FAIL;
    }


    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX
    CHECK_AVK_BR_CONNECTED_BY_IDX

    avrc_cmd.search.pdu = AVRC_PDU_SEARCH;
    avrc_cmd.search.status = AVRC_STS_NO_ERROR;
    avrc_cmd.search.string.charset_id = 0x006A;
    avrc_cmd.search.string.str_len= length;
    if(length > 0)
    {
        avrc_cmd.search.string.p_str = (uint8_t *)osi_malloc(length+1);
        memset(avrc_cmd.search.string.p_str, 0, length+1);
        strlcpy((char*)avrc_cmd.search.string.p_str, (char*)string, length+1);
    }

    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    osi_free(avrc_cmd.search.string.p_str);
    if (status != AVRC_STS_NO_ERROR) {
      BTIF_TRACE_ERROR("%s failed to build command status %d", __func__, status);
      return BT_STATUS_FAIL;
    }

    tran_status = get_transaction(&p_transaction, index);
    if (tran_status != BT_STATUS_SUCCESS || p_transaction == NULL) {
      osi_free(p_msg);
      BTIF_TRACE_ERROR("%s: failed to obtain transaction details. status: 0x%02x",
                       __func__, tran_status);
      return BT_STATUS_FAIL;
    }

    BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d", __func__,
                     p_transaction->lbl);
    BTA_AvkMetaCmd(btif_avk_rc_cb[index].rc_handle, p_transaction->lbl, AVRC_CMD_CTRL, p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return BT_STATUS_SUCCESS;

}

static bt_status_t play_item_cmd_vendor(bt_bdaddr_t *bd_addr, uint8_t scope, uint8_t *uid, uint16_t uid_counter)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;

    if(uid == NULL)
    {
        BTIF_TRACE_DEBUG("%s: uid is NULL", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX


    BTIF_TRACE_DEBUG("%s: ", __FUNCTION__);
    tran_status = get_transaction(&p_transaction, index);
    if(BT_STATUS_SUCCESS != tran_status || NULL==p_transaction)
        return BT_STATUS_FAIL;

    avrc_cmd.play_item.opcode = AVRC_OP_VENDOR;
    avrc_cmd.play_item.pdu = AVRC_PDU_PLAY_ITEM;
    avrc_cmd.play_item.status = AVRC_STS_NO_ERROR;
    avrc_cmd.play_item.scope = scope;
    memcpy(avrc_cmd.play_item.uid, uid, AVRC_UID_SIZE);
    avrc_cmd.play_item.uid_counter = uid_counter;

    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __FUNCTION__,p_transaction->lbl);
        if (p_msg != NULL)
        {
            BTA_AvkVendorCmd(btif_avk_rc_cb[index].rc_handle,p_transaction->lbl,AVRC_CMD_CTRL,
                data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __FUNCTION__, status);
     }
    if (p_msg != NULL)
        osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}

static bt_status_t addto_now_playing_cmd_vendor(bt_bdaddr_t *bd_addr, uint8_t scope, uint64_t UID, uint16_t uid_counter)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    rc_transaction_t *p_transaction=NULL;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_COMMAND avrc_cmd = {0};
    BT_HDR *p_msg = NULL;
    bt_status_t tran_status;

    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

    CHECK_AVK_RC_CONNECTED_BY_IDX


    BTIF_TRACE_DEBUG("%s: ", __FUNCTION__);
    tran_status = get_transaction(&p_transaction, index);
    if(BT_STATUS_SUCCESS != tran_status || NULL==p_transaction)
        return BT_STATUS_FAIL;

    avrc_cmd.add_to_play.opcode = AVRC_OP_VENDOR;
    avrc_cmd.add_to_play.pdu = AVRC_PDU_ADD_TO_NOW_PLAYING;
    avrc_cmd.add_to_play.status = AVRC_STS_NO_ERROR;
    avrc_cmd.add_to_play.scope = scope;
    memcpy(avrc_cmd.add_to_play.uid , &UID, sizeof(tAVRC_UID));
    avrc_cmd.add_to_play.uid_counter = uid_counter;

    status = AVRC_BldCommand(&avrc_cmd, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __FUNCTION__,p_transaction->lbl);
        if (p_msg != NULL)
        {
            BTA_AvkVendorCmd(btif_avk_rc_cb[index].rc_handle,p_transaction->lbl,AVRC_CMD_CTRL,
                data_start, p_msg->len);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __FUNCTION__, status);
     }
    if (p_msg != NULL)
        osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}


/***************************************************************************
**
** Function         send_abs_vol_rsp
**
** Description      Rsp for SetAbsoluteVolume Command
**
** Returns          void
**
***************************************************************************/
static bt_status_t send_abs_vol_rsp(bt_bdaddr_t *bd_addr, uint8_t abs_vol, uint8_t label)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
#if (AVRC_CTLR_INCLUDED == TRUE)
    tAVRC_RESPONSE avrc_rsp;
    BT_HDR *p_msg = NULL;
    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }
    CHECK_AVK_RC_CONNECTED

    BTIF_TRACE_DEBUG("%s: abs_vol %d", __FUNCTION__, abs_vol);

    avrc_rsp.volume.opcode = AVRC_OP_VENDOR;
    avrc_rsp.volume.pdu = AVRC_PDU_SET_ABSOLUTE_VOLUME;
    avrc_rsp.volume.status = AVRC_STS_NO_ERROR;
    avrc_rsp.volume.volume = abs_vol;
    status = AVRC_BldResponse(btif_avk_rc_cb[index].rc_handle, &avrc_rsp, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __FUNCTION__,btif_avk_rc_cb[index].rc_vol_label);
        if (p_msg != NULL)
        {
            BTA_AvkVendorRsp(btif_avk_rc_cb[index].rc_handle,btif_avk_rc_cb[index].rc_vol_label,BTA_AVK_RSP_ACCEPT,
                  data_start,p_msg->len,0);
            status =  BT_STATUS_SUCCESS;
        }
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __FUNCTION__, status);
     }
    if (p_msg != NULL)
        osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         send_register_abs_vol_rsp
**
** Description      Rsp for Notification of Absolute Volume
**
** Returns          void
**
***************************************************************************/
static bt_status_t send_register_abs_vol_rsp(bt_bdaddr_t *bd_addr, btrc_notification_type_t rsp_type, uint8_t abs_vol,
                                                    uint8_t label1)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    uint8_t label = 0;
    tAVRC_RESPONSE avrc_rsp;
    BT_HDR *p_msg = NULL;
    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

#if (AVRC_CTLR_INCLUDED == TRUE)
    CHECK_AVK_RC_CONNECTED

    BTIF_TRACE_DEBUG("%s: rsp_type  %d abs_vol %d", __FUNCTION__, rsp_type, abs_vol);

    avrc_rsp.reg_notif.opcode = AVRC_OP_VENDOR;
    avrc_rsp.reg_notif.pdu = AVRC_PDU_REGISTER_NOTIFICATION;
    avrc_rsp.reg_notif.status = AVRC_STS_NO_ERROR;
    avrc_rsp.reg_notif.param.volume = abs_vol;
    avrc_rsp.reg_notif.event_id = AVRC_EVT_VOLUME_CHANGE;
    label = btif_avk_rc_cb[index].rc_notif[AVRC_EVT_VOLUME_CHANGE-1].label;
    if ((rsp_type == AVRC_RSP_CHANGED) && (btif_avk_rc_cb[index].rc_notif[AVRC_EVT_VOLUME_CHANGE-1].bNotify))
    {
        btif_avk_rc_cb[index].rc_notif[AVRC_EVT_VOLUME_CHANGE-1].bNotify = FALSE;
    }
    status = AVRC_BldResponse(btif_avk_rc_cb[index].rc_handle, &avrc_rsp, &p_msg);
    if (status == AVRC_STS_NO_ERROR)
    {
        uint8_t *data_start;
        BTIF_TRACE_DEBUG("%s msgreq being sent out with label %d",
                __FUNCTION__,label);
        if (p_msg != NULL)
        {
            data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
            BTA_AvkVendorRsp(btif_avk_rc_cb[index].rc_handle,label,
                    (rsp_type == BTRC_NOTIFICATION_TYPE_INTERIM)
                    ? AVRC_RSP_INTERIM
                    : AVRC_RSP_CHANGED, data_start,p_msg->len,0);
        }
        status =  BT_STATUS_SUCCESS;
    }
    else
    {
         BTIF_TRACE_ERROR("%s: failed to build command. status: 0x%02x",
                            __FUNCTION__, status);
    }
    if (p_msg != NULL)
        osi_free(p_msg);
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}

/*******************************************************************************
**
** Function         btif_avk_rc_idx_by_bdaddr
**
** Description      Get the rc index corresponding to BD addr
**
** Returns          UNIT8
**
*******************************************************************************/

static uint8_t btif_avk_rc_idx_by_bdaddr(RawAddress * bd_addr)
{
    int i;
    for (i = 0; i < btif_max_rc_clients; i++)
    {
        if (*bd_addr == btif_avk_rc_cb[i].rc_addr)
            return i;
    }
    return i;
}

/***************************************************************************
**
** Function         send_passthrough_cmd
**
** Description      Send Pass-Through command
**
** Returns          void
**
***************************************************************************/
static bt_status_t send_passthrough_cmd(bt_bdaddr_t *bd_addr, uint8_t key_code, uint8_t key_state)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    /* Controller is used only for Certification purposes.
     * In normal case AVRCP controller will not be used, hence
     * updating this is required.
     */
    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    }

#if (AVRC_CTLR_INCLUDED == TRUE)
    rc_transaction_t *p_transaction=NULL;
    CHECK_AVK_RC_CONNECTED

    BTIF_TRACE_DEBUG("%s: key-code: %d, key-state: %d", __FUNCTION__,
                                                    key_code, key_state);
    if (btif_avk_rc_cb[index].rc_features & BTA_AVK_FEAT_RCTG)
    {
        bt_status_t tran_status = get_transaction(&p_transaction, index);
        if(BT_STATUS_SUCCESS == tran_status && NULL != p_transaction)
        {
            BTA_AvkRemoteCmd(btif_avk_rc_cb[index].rc_handle, p_transaction->lbl,
                (tBTA_AVK_RC)key_code, (tBTA_AVK_STATE)key_state);
            status =  BT_STATUS_SUCCESS;
            BTIF_TRACE_DEBUG("%s: succesfully sent passthrough command to BTA", __FUNCTION__);
        }
        else
        {
            status =  BT_STATUS_FAIL;
            BTIF_TRACE_DEBUG("%s: error in fetching transaction", __FUNCTION__);
        }
    }
    else
    {
        status =  BT_STATUS_FAIL;
        BTIF_TRACE_DEBUG("%s: feature not supported", __FUNCTION__);
    }
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}

/***************************************************************************
**
** Function         send_groupnavigation_cmd
**
** Description      Send Pass-Through command
**
** Returns          void
**
***************************************************************************/
static bt_status_t send_groupnavigation_cmd(bt_bdaddr_t *bd_addr, uint8_t key_code,
                                            uint8_t key_state)
{
    tAVRC_STS status = BT_STATUS_UNSUPPORTED;
    int index = btif_avk_rc_idx_by_bdaddr(bd_addr);
    BTIF_TRACE_DEBUG("%s: index = %d ", __FUNCTION__, index);
    if (index >= btif_max_rc_clients)
    {
        BTIF_TRACE_DEBUG("%s: invalid index", __FUNCTION__);
        return BT_STATUS_FAIL;
    } 
#if (AVRC_CTLR_INCLUDED == TRUE)
    rc_transaction_t *p_transaction=NULL;
    BTIF_TRACE_DEBUG("%s: key-code: %d, key-state: %d", __FUNCTION__,
                                                    key_code, key_state);
    CHECK_AVK_RC_CONNECTED
    if (btif_avk_rc_cb[index].rc_features & BTA_AVK_FEAT_RCTG)
    {
        bt_status_t tran_status = get_transaction(&p_transaction, index);
        if ((BT_STATUS_SUCCESS == tran_status) && (NULL != p_transaction)) {
             uint8_t buffer[AVRC_PASS_THRU_GROUP_LEN] = {0};
             uint8_t* start = buffer;
             UINT24_TO_BE_STREAM(start, AVRC_CO_METADATA);
             *(start)++ = 0;
             UINT8_TO_BE_STREAM(start, key_code);
             BTA_AvRemoteVendorUniqueCmd(btif_avk_rc_cb[index].rc_handle,
                                         p_transaction->lbl,
                                         (tBTA_AVK_STATE)key_state, buffer,
                                         AVRC_PASS_THRU_GROUP_LEN);
             status =  BT_STATUS_SUCCESS;
             BTIF_TRACE_DEBUG("%s: succesfully sent group_navigation command to BTA",
                              __FUNCTION__);
        }
        else
        {
            status =  BT_STATUS_FAIL;
            BTIF_TRACE_DEBUG("%s: error in fetching transaction", __FUNCTION__);
        }
    }
    else
    {
        status =  BT_STATUS_FAIL;
        BTIF_TRACE_DEBUG("%s: feature not supported", __FUNCTION__);
    }
#else
    BTIF_TRACE_DEBUG("%s: feature not enabled", __FUNCTION__);
#endif
    return (bt_status_t)status;
}

static const btrc_ctrl_interface_t btif_avk_rc_ctrl_interface = {
    sizeof(btif_avk_rc_ctrl_interface),
    init_ctrl,
    send_passthrough_cmd,
    send_groupnavigation_cmd,
    set_player_app_setting_cmd,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    send_abs_vol_rsp,
    send_register_abs_vol_rsp,
    cleanup_ctrl,
};

static const btrc_ctrl_vendor_interface_t btif_avk_rc_ctrl_vendor_interface = {
    sizeof(btif_avk_rc_ctrl_vendor_interface),
    init_ctrl_vendor,
    getcapabilities_cmd_vendor,
    list_player_app_setting_attrib_cmd_vendor,
    list_player_app_setting_value_cmd_vendor,
    get_player_app_setting_cmd_vendor,
    register_notification_cmd_vendor,
    get_element_attribute_cmd_vendor,
    get_play_status_cmd_vendor,
    set_addressed_player_cmd_vendor,
    set_browsed_player_cmd_vendor,
    change_folder_path_cmd_vendor,
    get_folder_items_cmd_vendor,
    get_item_attributes_cmd_vendor,
    play_item_cmd_vendor,
    addto_now_playing_cmd_vendor,
    search_cmd_vendor,
    cleanup_ctrl_vendor,
};


/*******************************************************************************
**
** Function         btif_avk_rc_ctrl_get_interface
**
** Description      Get the AVRCP Controller callback interface
**
** Returns          btav_interface_t
**
*******************************************************************************/
const btrc_ctrl_interface_t *btif_avk_rc_ctrl_get_interface(void)
{
    BTIF_TRACE_EVENT("%s", __FUNCTION__);
    return &btif_avk_rc_ctrl_interface;
}

/*******************************************************************************
**
** Function         btif_avk_rc_ctrl_vendor_get_interface
**
** Description      Get the AVRCP Controller callback vendor interface
**
** Returns          btrc_ctrl_vendor_interface_t
**
*******************************************************************************/
const btrc_ctrl_vendor_interface_t *btif_avk_rc_ctrl_vendor_get_interface(void)
{
    BTIF_TRACE_EVENT("%s", __FUNCTION__);
    return &btif_avk_rc_ctrl_vendor_interface;
}

/*******************************************************************************
**      Function         initialize_transaction
**
**      Description    Initializes fields of the transaction structure
**
**      Returns          void
*******************************************************************************/
static void initialize_transaction(int lbl, int index)
{
    pthread_mutex_lock(&btif_avk_rc_cb[index].device.lbllock);
    if(lbl < MAX_TRANSACTIONS_PER_SESSION)
    {
       if (alarm_is_scheduled(btif_avk_rc_cb[index].device.transaction[lbl].txn_timer))
          clear_cmd_timeout(lbl, index);
       //free alarm before assigning null to it
       alarm_free(btif_avk_rc_cb[index].device.transaction[lbl].txn_timer);
       btif_avk_rc_cb[index].device.transaction[lbl].txn_timer = NULL;
       btif_avk_rc_cb[index].device.transaction[lbl].lbl = lbl;
       btif_avk_rc_cb[index].device.transaction[lbl].in_use=FALSE;
       btif_avk_rc_cb[index].device.transaction[lbl].handle=0;
    }
    pthread_mutex_unlock(&btif_avk_rc_cb[index].device.lbllock);
}

/*******************************************************************************
**      Function         lbl_init
**
**      Description    Initializes label structures and mutexes.
**
**      Returns         void
*******************************************************************************/
static void lbl_init()
{
    for(int index = 0; index < btif_max_rc_clients; index++)
    {
        memset(&btif_avk_rc_cb[index].device,0,sizeof(rc_device_t));
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutex_init(&(btif_avk_rc_cb[index].device.lbllock), &attr);
        pthread_mutexattr_destroy(&attr);
    }
    init_all_transactions();
}

/*******************************************************************************
**
** Function         init_all_transactions
**
** Description    Initializes all transactions
**
** Returns          void
*******************************************************************************/
static void init_all_transactions()
{
    uint8_t txn_indx, index=0;
    for(txn_indx=0; txn_indx < MAX_TRANSACTIONS_PER_SESSION; txn_indx++)
    {
        for(index=0; index < btif_max_rc_clients; index++)
        {
            initialize_transaction(txn_indx, index);
        }
    }
}

/*******************************************************************************
**
** Function         get_transaction_by_lbl
**
** Description    Will return a transaction based on the label. If not inuse
**                     will return an error.
**
** Returns          bt_status_t
*******************************************************************************/
static rc_transaction_t *get_transaction_by_lbl(uint8_t lbl, uint8_t index)
{
    rc_transaction_t *transaction = NULL;
    pthread_mutex_lock(&btif_avk_rc_cb[index].device.lbllock);

    /* Determine if this is a valid label */
    if (lbl < MAX_TRANSACTIONS_PER_SESSION)
    {
        if (FALSE==btif_avk_rc_cb[index].device.transaction[lbl].in_use)
        {
            transaction = NULL;
        }
        else
        {
            transaction = &(btif_avk_rc_cb[index].device.transaction[lbl]);
            BTIF_TRACE_DEBUG("%s: Got transaction.label: %d index: %d",__FUNCTION__,lbl, index);
        }
    }

    pthread_mutex_unlock(&btif_avk_rc_cb[index].device.lbllock);
    return transaction;
}

/*******************************************************************************
**
** Function         get_transaction
**
** Description    Obtains the transaction details.
**
** Returns          bt_status_t
*******************************************************************************/

static bt_status_t  get_transaction(rc_transaction_t **ptransaction, uint8_t index)
{
    bt_status_t result = BT_STATUS_NOMEM;
    uint8_t i=0;
    pthread_mutex_lock(&btif_avk_rc_cb[index].device.lbllock);

    // Check for unused transactions
    for (i=0; i<MAX_TRANSACTIONS_PER_SESSION; i++)
    {
        if (FALSE==btif_avk_rc_cb[index].device.transaction[i].in_use)
        {
            BTIF_TRACE_DEBUG("%s :Got transaction.label: %d index:%d", __FUNCTION__,btif_avk_rc_cb[index].device.transaction[i].lbl, index);
            btif_avk_rc_cb[index].device.transaction[i].in_use = TRUE;
            *ptransaction = &(btif_avk_rc_cb[index].device.transaction[i]);
            result = BT_STATUS_SUCCESS;
            break;
        }
    }

    pthread_mutex_unlock(&btif_avk_rc_cb[index].device.lbllock);
    return result;
}


/*******************************************************************************
**
** Function         release_transaction
**
** Description    Will release a transaction for reuse
**
** Returns          bt_status_t
*******************************************************************************/
static void release_transaction(uint8_t lbl, uint8_t index)
{
    rc_transaction_t *transaction = get_transaction_by_lbl(lbl, index);

    /* If the transaction is in use... */
    if (transaction != NULL)
    {
        BTIF_TRACE_DEBUG("%s: lbl: %d index: %d", __FUNCTION__, lbl, index);
        initialize_transaction(lbl, index);
    }
}

/*******************************************************************************
**
** Function         lbl_destroy
**
** Description    Cleanup of the mutex
**
** Returns          void
*******************************************************************************/
static void lbl_destroy()
{
    for(int index=0; index < btif_max_rc_clients; index++)
    {
        if (!pthread_mutex_destroy(&(btif_avk_rc_cb[index].device.lbllock)))
        {
            btif_avk_rc_cb[index].device.lbllock_destroyed = TRUE;
            BTIF_TRACE_EVENT(" %s: lbllock destroy success ", __FUNCTION__);
        }
    }
}

/*******************************************************************************
**      Function       sleep_ms
**
**      Description    Sleep the calling thread unconditionally for
**                     |timeout_ms| milliseconds.
**
**      Returns        void
*******************************************************************************/
static void sleep_ms(period_ms_t timeout_ms) {
    struct timespec delay;
    delay.tv_sec = timeout_ms / 1000;
    delay.tv_nsec = 1000 * 1000 * (timeout_ms % 1000);

    OSI_NO_INTR(nanosleep(&delay, &delay));
}
