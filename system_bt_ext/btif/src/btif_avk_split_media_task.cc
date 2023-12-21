/******************************************************************************
 *  Copyright (c) 2016-2017, The Linux Foundation. All rights reserved.
 *
 *  Not a contribution.
 ******************************************************************************/
/******************************************************************************
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

/******************************************************************************
 **
 **  Name:          btif_avk_media_task.c
 **
 **  Description:   This is the multimedia module for the BTIF system.  It
 **                 contains task implementations AV, HS and HF profiles
 **                 audio & video processing
 **
 ******************************************************************************/
//#define BT_AUDIO_SYSTRACE_LOG
#ifdef BT_AUDIO_SYSTRACE_LOG
#define ATRACE_TAG ATRACE_TAG_ALWAYS
#endif

#define LOG_TAG "bt_btif_avk_split_media"

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <errno.h>
#include <dlfcn.h>
#include "bt_utils.h"
#include "bt_target.h"
#include "bta_api.h"
#include "btu.h"
#include "bta_sys.h"
#include "bta_sys_int.h"

#include "bta_avk_api.h"
#include "a2dp_api.h"
#include "a2d_sbc.h"
#include "a2dp_int.h"
#include "a2d_aptx.h"
#include "a2d_aptx_hd.h"
#include "bta_avk_sbc.h"
#include "bta_avk_ci.h"
#include "l2c_api.h"

#include "btif_avk_co.h"
#include "btif_avk_media.h"

#include "osi/include/alarm.h"
#include "osi/include/log.h"
#include "osi/include/thread.h"
#include "osi/include/fixed_queue.h"

#if (BTA_AV_INCLUDED == TRUE)
#include "sbc_encoder.h"
#endif

#include <hardware/bluetooth.h>
#include "audio_a2dp_hw.h"
#include "btif_avk.h"
#include "btif_sm.h"
#include "btif_util.h"
#if (BTA_AV_SINK_INCLUDED == TRUE)
#include "oi_codec_sbc.h"
#include "oi_status.h"
#endif
#include "stdio.h"
#include <dlfcn.h>


#ifdef BT_AUDIO_SYSTRACE_LOG
#include <cutils/trace.h>
#define PERF_SYSTRACE 1
#endif

//#define DUMP_PCM_DATA TRUE
#if (defined(DUMP_PCM_DATA) && (DUMP_PCM_DATA == TRUE))
FILE *outputPcmSampleFile;
char outputFilename [50] = "/etc/bluetooth/output_sample.pcm";
#endif
/*****************************************************************************
 **  Constants
 *****************************************************************************/

#ifndef AUDIO_CHANNEL_OUT_MONO
#define AUDIO_CHANNEL_OUT_MONO 0x01
#endif

#ifndef AUDIO_CHANNEL_OUT_STEREO
#define AUDIO_CHANNEL_OUT_STEREO 0x03
#endif
#define A2DP_SINK_AUDIO_CODEC_PCM       0x40
#define A2DP_SINK_AUDIO_CODEC_SBC       0x00
#define A2DP_SINK_AUDIO_CODEC_AAC       0x02
#define A2DP_SINK_AUDIO_CODEC_APTX      0x08
#define A2DP_SINK_AUDIO_CODEC_APTX_AD   0x10
#define A2DP_SINK_AUDIO_CODEC_APTX_HD   0x20

#define AVDT_AVK_MEDIA_AUDIO            0x00


/* Media Task State for Split Mode */
enum {
    AVK_MEDIA_TASK_STATE_OFF = 0,
    AVK_MEDIA_TASK_STATE_ON = 1,
    AVK_MEDIA_TASK_STATE_SHUTTING_DOWN = 2
};


/* Middle quality quality setting @ 44.1 khz */
#define DEFAULT_SBC_BITRATE 328

#ifndef BTIF_A2DP_NON_EDR_MAX_RATE
#define BTIF_A2DP_NON_EDR_MAX_RATE 229
#endif

#if (BTA_AV_CO_CP_SCMS_T == TRUE)
/* A2DP header will contain a CP header of size 1 */
#define A2DP_HDR_SIZE               2
#else
#define A2DP_HDR_SIZE               1
#endif
#define MAX_SBC_HQ_FRAME_SIZE_44_1  119
#define MAX_SBC_HQ_FRAME_SIZE_48    115

#define USEC_PER_SEC 1000000L
#define TPUT_STATS_INTERVAL_US (3000*1000)


#define MAX_MEDIA_WORKQUEUE_COUNT 1024


typedef struct
{
#if (BTA_AV_INCLUDED == TRUE)
    uint16_t TxAaMtuSize;
    uint16_t offset;
    void* av_sm_hdl;
    uint8_t a2dp_cmd_pending; /* we can have max one command pending */
    uint32_t  sample_rate;
    uint8_t   channel_count;
#endif

} tBTIF_AVK_MEDIA_CB;

static uint64_t last_frame_us = 0;

//static void btif_a2dp_data_cb(tUIPC_CH_ID ch_id, tUIPC_EVENT event);
static void btif_a2dp_ctrl_cb(tUIPC_CH_ID ch_id, tUIPC_EVENT event);
static void btif_avk_media_task_aa_rx_flush(void);

static uint32_t get_frame_length();
static uint8_t check_for_max_number_of_frames_per_packet();
static const char *dump_media_event(uint16_t event);
static void btif_avk_media_thread_init(void *context);
static void btif_avk_uipc_open(void *context);
static void btif_avk_media_thread_cleanup(void *context);
static void btif_avk_media_thread_handle_cmd(fixed_queue_t *queue, void *context);


extern uint32_t btif_avk_media_fetch_pcm_data(uint16_t codec_type, uint8_t *data, uint32_t size);
void btif_avk_media_task_decode(void);

static tBTIF_AVK_MEDIA_CB btif_avk_media_cb;
static int avk_media_task_running = AVK_MEDIA_TASK_STATE_OFF;

static fixed_queue_t *btif_avk_media_cmd_msg_queue;
static thread_t *avk_worker_thread;

// no messags implemented as of now for split
UNUSED_ATTR static const char *dump_media_event(uint16_t event)
{
    return "NO MSG IMPL";
}


/*****************************************************************************
 **  A2DP CTRL PATH
 *****************************************************************************/

static const char* dump_a2dp_ctrl_event(uint8_t event)
{
    switch(event)
    {
        CASE_RETURN_STR(A2DP_AVK_CTRL_CMD_NONE)
        CASE_RETURN_STR(A2DP_AVK_CTRL_CMD_CHECK_SOCKET)
        CASE_RETURN_STR(A2DP_AVK_CTRL_CMD_CHECK_READY)
        CASE_RETURN_STR(A2DP_AVK_CTRL_CMD_START_CAPTURE)
        CASE_RETURN_STR(A2DP_AVK_CTRL_CMD_STOP_CAPTURE)
        CASE_RETURN_STR(A2DP_AVK_CTRL_GET_CODEC_CONFIG)
        CASE_RETURN_STR(A2DP_AVK_CTRL_SESSION_SETUP_COMPLETE)
        default:
            return "UNKNOWN MSG ID";
    }
}

static void a2dp_cmd_acknowledge(uint8_t status)
{
    uint8_t ack = status;

    APPL_TRACE_IMP("## a2dp ack : %s, status %d ##",
          dump_a2dp_ctrl_event(btif_avk_media_cb.a2dp_cmd_pending), status);

    /* sanity check */
    if (btif_avk_media_cb.a2dp_cmd_pending == A2DP_CTRL_CMD_NONE)
    {
        APPL_TRACE_ERROR("warning : no command pending, ignore ack");
        return;
    }

    /* clear pending */
    btif_avk_media_cb.a2dp_cmd_pending = A2DP_CTRL_CMD_NONE;

    /* acknowledge start request */
    UIPC_AVK_Send(UIPC_CH_ID_AVK_CTRL, 0, &ack, 1);
}

void btif_update_cmd_status(uint8_t cmd,uint8_t status)
{
    APPL_TRACE_DEBUG(" %s pending_cmd %d  update for cmd %d ",__func__,
                                    btif_avk_media_cb.a2dp_cmd_pending,cmd);
    if(btif_avk_media_cb.a2dp_cmd_pending == cmd)
    {
        a2dp_cmd_acknowledge(status);
    }
    else
        APPL_TRACE_ERROR("pending command missmatch");

    APPL_TRACE_DEBUG("exit btif_update_cmd_status a2dp_cmd_pending : %d", btif_avk_media_cb.a2dp_cmd_pending);
}

static void btif_split_recv_ctrl_data(void)
{
    uint8_t cmd = 0;
    int n;
    uint8_t index = 0;
    n = UIPC_AVK_Read(UIPC_CH_ID_AVK_CTRL, NULL, &cmd, 1);

    /* detach on ctrl channel means audioflinger process was terminated */
    if (n == 0)
    {
        APPL_TRACE_IMP("%s CTRL CH DETACHED",__func__);
        /* we can operate only on datachannel, if af client wants to
           do send additional commands the ctrl channel would be reestablished */
        //btif_audiopath_detached();
        return;
    }

    APPL_TRACE_IMP(" %s a2dp-ctrl-cmd : %s",__func__, dump_a2dp_ctrl_event(cmd));

    btif_avk_media_cb.a2dp_cmd_pending = cmd;

    //to do
    switch(cmd)
    {
        case A2DP_AVK_CTRL_CMD_CHECK_SOCKET:
            a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
            break;
        case A2DP_AVK_CTRL_CMD_CHECK_READY:
            btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_CHECK_A2DP_READY, NULL, 0);
            break;
        case A2DP_AVK_CTRL_CMD_START_CAPTURE:
            if(is_streaming_bda_empty())
            {
                //if streaming_bda is empty send failure ack
                BTIF_TRACE_DEBUG("%s streaming_bda is empty",__func__);
                a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
                break;
            }
            btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_START_CAPTURE, NULL, 0);
            break;
        case A2DP_AVK_CTRL_CMD_STOP_CAPTURE:
            // stop capture should be linked to device which has VSC_STATE = STARTED.
            if (!btif_avk_split_get_started_device_idx(&index))
            {
                // there is no device matching started/suspending/HDL_VSC state.
                // ack from here itself
                BTIF_TRACE_DEBUG("%s no device in started/suspending/HDL_VSC",__func__);
                a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
                break;
            }
            BTIF_TRACE_DEBUG("%s found device @ index %d",__func__,index);
            btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_STOP_CAPTURE, &index,
                                       sizeof(uint8_t));
            break;
        case A2DP_AVK_CTRL_SESSION_SETUP_COMPLETE:
        {
            uint8_t latency[8];
            memset(latency,0,8);
            UIPC_AVK_Read(UIPC_CH_ID_AVK_CTRL, NULL, latency, 8);
            BTIF_TRACE_DEBUG(" Latency %d %d %d %d %d %d ",latency[0], latency[1],latency[2],latency[3],latency[4],latency[5]);
            btif_avk_split_dispatch_sm_event(BTIF_AVK_SPLIT_SINK_SETUP_COMPLETE, &latency ,8);
            break;
        }
        case A2DP_AVK_CTRL_GET_CODEC_CONFIG:
        {
            uint8_t param[MAX_CODEC_CFG_SIZE],idx,bta_hdl,codec_id = 0, vendor_codec_id = 0;
            uint32_t sampling_freq = 0;
            uint8_t len = 0;
            memset(param,0,MAX_CODEC_CFG_SIZE);
            codec_id = bta_avk_get_current_codec();
            a2dp_cmd_acknowledge(A2DP_CTRL_ACK_SUCCESS);
            BTIF_TRACE_DEBUG("codec_id = %x",codec_id);
            if (codec_id == A2DP_SINK_AUDIO_CODEC_SBC)
            {
                //audio doesn't need sbc params, can read from header
                tA2D_SBC_CIE codec_cfg;
                param[0] = 7;//length
                param[1] = 0;//media type: audio
                param[2] = codec_id;
                bta_avk_co_audio_get_sbc_config(&codec_cfg);
                sampling_freq = btif_avk_a2dp_get_sbc_track_frequency(codec_cfg.samp_freq);
                param[3] = uint8_t(sampling_freq & 0xFF);
                param[4] = uint8_t(((sampling_freq & 0xFF00) >> 8) & 0xFF);
                param[5] = uint8_t(((sampling_freq & 0xFF0000) >> 16) & 0xFF);
                param[6] = uint8_t(((sampling_freq & 0xFF000000) >> 24) & 0xFF);
                param[7] = btif_avk_a2dp_get_sbc_track_channel_count(codec_cfg.ch_mode);
            }
            else if(codec_id == A2DP_SINK_AUDIO_CODEC_AAC) {
                tA2D_AAC_CIE aac_cfg;
                bta_avk_co_audio_get_aac_config(&aac_cfg);
                A2D_BldAacInfo(AVDT_AVK_MEDIA_AUDIO,&aac_cfg,&param[0]);
            }
            else if(codec_id == A2D_NON_A2DP_MEDIA_CT) {
                vendor_codec_id = bta_avk_get_current_vendor_codec();
#if defined(APTX_CLASSIC_DECODER_INCLUDED) && (APTX_CLASSIC_DECODER_INCLUDED == TRUE)
                if(vendor_codec_id == A2DP_SINK_AUDIO_CODEC_APTX) {
                     tA2D_APTX_CIE aptx_cfg;
                     bta_avk_co_audio_get_aptx_config(&aptx_cfg);
                     A2D_BldAptxInfo(AVDT_AVK_MEDIA_AUDIO,&aptx_cfg,&param[0]);
                }
#endif
#if defined(APTX_HD_DECODER_INCLUDED) && (APTX_HD_DECODER_INCLUDED == TRUE)
                if(vendor_codec_id == A2DP_SINK_AUDIO_CODEC_APTX_HD) {
                     tA2D_APTX_HD_CIE aptx_hd_cfg;
                     bta_avk_co_audio_get_aptx_hd_config(&aptx_hd_cfg);
                     A2D_BldAptx_hdInfo(AVDT_AVK_MEDIA_AUDIO,&aptx_hd_cfg,&param[0]);
                }
#endif
#if defined(APTX_AD_DECODER_INCLUDED) && (APTX_AD_DECODER_INCLUDED == TRUE)
                if(vendor_codec_id == A2DP_SINK_AUDIO_CODEC_APTX_AD) {
                     tA2DP_AVK_APTX_AD_CIE aptxad_cfg;
                     bta_avk_co_audio_get_aptx_ad_config(&aptxad_cfg);
                     A2D_BldAptxADDecoderInfo(AVDT_AVK_MEDIA_AUDIO,&aptxad_cfg,&param[0]);
                }
#endif
            }
            len = param[0]+1;
            UIPC_AVK_Send(UIPC_CH_ID_AVK_CTRL, 0, &len, 1);
            UIPC_AVK_Send(UIPC_CH_ID_AVK_CTRL, 0, (uint8_t*)&param, len);
            break;
        }
        default:
            APPL_TRACE_ERROR("UNSUPPORTED CMD (%d)", cmd);
            a2dp_cmd_acknowledge(A2DP_CTRL_ACK_FAILURE);
            break;
    }
    APPL_TRACE_IMP("Sink a2dp-ctrl-cmd : %s DONE", dump_a2dp_ctrl_event(cmd));
}

static void btif_a2dp_ctrl_cb(tUIPC_CH_ID ch_id, tUIPC_EVENT event)
{
    APPL_TRACE_IMP("A2DP-CTRL-CHANNEL EVENT %s", dump_avk_uipc_event(event));

    switch(event)
    {
        case UIPC_AVK_Open_EVT:
            break;

        case UIPC_AVK_Close_EVT:
            /* restart ctrl server unless we are shutting down */
            if (avk_media_task_running == AVK_MEDIA_TASK_STATE_ON) {
               thread_post(avk_worker_thread, btif_avk_uipc_open, NULL);
            }
            break;

        case UIPC_AVK_RX_DATA_READY_EVT:
            btif_split_recv_ctrl_data();
            break;

        default :
            APPL_TRACE_ERROR("### A2DP-CTRL-CHANNEL EVENT %d NOT HANDLED ###", event);
            break;
    }
}

/*****************************************************************************
 **  BTIF ADAPTATION
 *****************************************************************************/

bool btif_avk_split_a2dp_is_media_task_stopped(void)
{
    if (avk_media_task_running != AVK_MEDIA_TASK_STATE_OFF)
    {
        APPL_TRACE_ERROR("%s: media task state = %d",__func__, avk_media_task_running);
        return false;
    }
    return true;
}

bool btif_avk_split_a2dp_start_media_task(void)
{
    APPL_TRACE_IMP("## A2DP START MEDIA THREAD ##");
    if (avk_media_task_running != AVK_MEDIA_TASK_STATE_OFF)
    {
        APPL_TRACE_ERROR("warning : media task state: %d",
                                            avk_media_task_running);
        return false;
    }

    btif_avk_media_cmd_msg_queue = fixed_queue_new(SIZE_MAX);
    if (btif_avk_media_cmd_msg_queue == NULL)
        goto error_exit;

    /* start a2dp media task */
    avk_worker_thread = thread_new_sized("media_worker", MAX_MEDIA_WORKQUEUE_COUNT);
    if (avk_worker_thread == NULL)
        goto error_exit;

    fixed_queue_register_dequeue(btif_avk_media_cmd_msg_queue,
        thread_get_reactor(avk_worker_thread),
        btif_avk_media_thread_handle_cmd,
        NULL);

    thread_post(avk_worker_thread, btif_avk_media_thread_init, NULL);

    APPL_TRACE_IMP("## %s - ##",__func__);

    return true;

 error_exit:;
    APPL_TRACE_ERROR("%s unable to start up media thread", __func__);
    return false;
}

void btif_avk_split_a2dp_stop_media_task(void)
{
    APPL_TRACE_IMP("## %s + ##",__func__);
    if (avk_media_task_running != AVK_MEDIA_TASK_STATE_ON)
    {
        APPL_TRACE_ERROR("warning: media task cleanup state: %d",
                                        avk_media_task_running);
        return;
    }
    /* make sure no channels are restarted while shutting down */
    avk_media_task_running = AVK_MEDIA_TASK_STATE_SHUTTING_DOWN;

    // Exit thread
    fixed_queue_free(btif_avk_media_cmd_msg_queue, NULL);
    thread_post(avk_worker_thread, btif_avk_media_thread_cleanup, NULL);
    thread_free(avk_worker_thread);

    avk_worker_thread = NULL;
    btif_avk_media_cmd_msg_queue = NULL;
    APPL_TRACE_IMP("## A2DP MEDIA THREAD STOPPED ##");
}

/*****************************************************************************
**
** Function        btif_avk_split_a2dp_on_idle
**
** Description
**
** Returns
**
*******************************************************************************/

void btif_avk_split_a2dp_on_idle(void)
{
    APPL_TRACE_IMP("## %s  ##",__func__);
    bta_avk_co_init();
}

static void btif_avk_uipc_open(UNUSED_ATTR void *context) {
    APPL_TRACE_IMP(" %s ++",__func__);
    bool status = false;
    status = UIPC_AVK_Open(UIPC_CH_ID_AVK_CTRL , btif_a2dp_ctrl_cb);
    APPL_TRACE_IMP(" %s --, status:%d",__func__,status);
}

static void btif_avk_media_thread_init(UNUSED_ATTR void *context) {
  bool status = false;
  APPL_TRACE_IMP(" %s  split mode +",__func__);
  memset(&btif_avk_media_cb, 0, sizeof(btif_avk_media_cb));
  UIPC_AVK_Init(NULL);

#if (BTA_AV_INCLUDED == TRUE)
  status = UIPC_AVK_Open(UIPC_CH_ID_AVK_CTRL , btif_a2dp_ctrl_cb);
#endif

  raise_priority_a2dp(TASK_HIGH_MEDIA);
  avk_media_task_running = AVK_MEDIA_TASK_STATE_ON;
  APPL_TRACE_IMP(" %s split mode - , status:%d",__func__,status);
}

static void btif_avk_media_thread_cleanup(UNUSED_ATTR void *context) {
  APPL_TRACE_IMP(" btif_avk_media_thread_cleanup");

  /* this calls blocks until uipc is fully closed */
  UIPC_AVK_Close(UIPC_CH_ID_ALL);

  /* Clear media task flag */
  avk_media_task_running = AVK_MEDIA_TASK_STATE_OFF;
  APPL_TRACE_IMP(" btif_avk_media_thread_cleanup complete");
}

/* Apis to handle the messages posted to media task thread */
static void btif_avk_media_thread_handle_cmd(fixed_queue_t *queue, UNUSED_ATTR void *context)
{
    uint32_t size;
    BT_HDR *p_msg = (BT_HDR *)fixed_queue_dequeue(queue);
    if(p_msg == NULL)
        return;
    APPL_TRACE_IMP("%s  : %d %s",__func__, p_msg->event,
             dump_media_event(p_msg->event));

    osi_free(p_msg);
    APPL_TRACE_IMP("%s:DONE", __func__);
}

#if (BTA_AV_INCLUDED == TRUE)

int btif_avk_split_a2dp_get_sbc_track_frequency(uint8_t frequency) {
    int freq = 48000;
    switch (frequency) {
        case A2D_SBC_IE_SAMP_FREQ_16:
            freq = 16000;
            break;
        case A2D_SBC_IE_SAMP_FREQ_32:
            freq = 32000;
            break;
        case A2D_SBC_IE_SAMP_FREQ_44:
            freq = 44100;
            break;
        case A2D_SBC_IE_SAMP_FREQ_48:
            freq = 48000;
            break;
    }
    return freq;
}

int btif_avk_split_a2dp_get_sbc_track_channel_count(uint8_t channeltype) {
    int count = 1;
    switch (channeltype) {
        case A2D_SBC_IE_CH_MD_MONO:
            count = 1;
            break;
        case A2D_SBC_IE_CH_MD_DUAL:
        case A2D_SBC_IE_CH_MD_STEREO:
        case A2D_SBC_IE_CH_MD_JOINT:
            count = 2;
            break;
    }
    return count;
}


static uint64_t time_now_us()
{
    struct timespec ts_now;
    clock_gettime(CLOCK_BOOTTIME, &ts_now);
    return ((uint64_t)ts_now.tv_sec * USEC_PER_SEC) + ((uint64_t)ts_now.tv_nsec / 1000);
}


#endif /* BTA_AV_INCLUDED == TRUE */

