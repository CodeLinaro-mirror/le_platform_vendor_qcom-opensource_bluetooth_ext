/******************************************************************************
 *
 *  Copyright (C) 2003-2012 Broadcom Corporation
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
 *
 *  This is the interface file for advanced audio/video call-out functions.
 *
 ******************************************************************************/
#ifndef BTA_AVK_CO_H
#define BTA_AVK_CO_H

#include "l2c_api.h"
#include "bta_avk_api.h"
#include "a2d_sbc.h"
#include "bt_utils.h"
extern const tA2D_SBC_CIE bta_avk_co_sbc_caps;
#if defined(AAC_DECODER_INCLUDED) && (AAC_DECODER_INCLUDED == TRUE)
#include "a2d_aac.h"
#include "bta_avk_aac.h"
extern const tA2D_AAC_CIE bta_avk_co_aac_caps;
#endif
#if defined(MP3_DECODER_INCLUDED) && (MP3_DECODER_INCLUDED == TRUE)
#include "a2d_mp3.h"
#include "bta_avk_mp3.h"
extern const tA2D_MP3_CIE bta_avk_co_mp3_caps;
#endif
#if defined(APTX_CLASSIC_DECODER_INCLUDED) && (APTX_CLASSIC_DECODER_INCLUDED == TRUE)
#include "a2d_aptx.h"
extern const tA2D_APTX_CIE bta_avk_co_aptx_caps;
#endif
#if defined(APTX_HD_DECODER_INCLUDED) && (APTX_HD_DECODER_INCLUDED == TRUE)
#include "a2d_aptx_hd.h"
extern const tA2D_APTX_HD_CIE bta_avk_co_aptx_hd_caps;
#endif
#if defined(APTX_AD_DECODER_INCLUDED) && (APTX_AD_DECODER_INCLUDED == TRUE)
#include "a2d_aptx_ad_decoder.h"
extern tA2DP_AVK_APTX_AD_CIE bta_avk_co_aptx_ad_caps;
extern tA2DP_AVK_APTX_AD_CIE btif_avk_aptx_ad_default_config;
#endif

/*****************************************************************************
**  Constants and data types
*****************************************************************************/

/* TRUE to use SCMS-T content protection */
#ifndef BTA_AVK_CO_CP_SCMS_T
#define BTA_AVK_CO_CP_SCMS_T     FALSE
#endif

/* the content protection IDs assigned by BT SIG */
#define BTA_AVK_CP_SCMS_T_ID     0x0002
#define BTA_AVK_CP_DTCP_ID       0x0001

#define BTA_AVK_CP_LOSC                  2
#define BTA_AVK_CP_INFO_LEN              3

#define BTA_AVK_CP_SCMS_COPY_MASK        3
#define BTA_AVK_CP_SCMS_COPY_FREE        2
#define BTA_AVK_CP_SCMS_COPY_ONCE        1
#define BTA_AVK_CP_SCMS_COPY_NEVER       0

#define BTA_AVK_CO_DEFAULT_AUDIO_OFFSET      AVDT_MEDIA_OFFSET

enum
{
    BTIF_SV_AVK_AA_SBC_INDEX = 0,
#if defined(AAC_DECODER_INCLUDED) && (AAC_DECODER_INCLUDED == TRUE)
    BTIF_SV_AVK_AA_AAC_INDEX,
#endif
#if defined(MP3_DECODER_INCLUDED) && (MP3_DECODER_INCLUDED == TRUE)
    BTIF_SV_AVK_AA_MP3_INDEX,
#endif
#if defined(APTX_AD_DECODER_INCLUDED) && (APTX_AD_DECODER_INCLUDED == TRUE)
    BTIF_SV_AVK_AA_APTX_AD_INDEX,
#endif
#if defined(APTX_CLASSIC_DECODER_INCLUDED) && (APTX_CLASSIC_DECODER_INCLUDED == TRUE)
    BTIF_SV_AVK_AA_APTX_INDEX,
#endif
#if defined(APTX_HD_DECODER_INCLUDED) && (APTX_HD_DECODER_INCLUDED == TRUE)
    BTIF_SV_AVK_AA_APTX_HD_INDEX,
#endif
    BTIF_SV_AVK_AA_SEP_INDEX,  /* Last index */
};

typedef struct
{
    uint8_t codec_type;                   /* peer SEP codec type */
    /* codec and vendor id used to differntiate between Vendor Specific Codecs */
    uint16_t codec_id;
    uint8_t vendor_id;
    union {
        tA2D_SBC_CIE sbc_caps;
        tA2D_AAC_CIE aac_caps;
#if defined(MP3_DECODER_INCLUDED) && (MP3_DECODER_INCLUDED == TRUE)
        tA2D_MP3_CIE mp3_caps;
#endif
        tA2D_APTX_CIE aptx_caps;
        tA2D_APTX_HD_CIE aptx_hd_caps;
        tA2DP_AVK_APTX_AD_CIE aptx_ad_caps;
    } codec_cap;
} tBTA_AVK_CO_CODEC_CAP_LIST;

/*******************************************************************************
**
** Function         bta_avk_co_audio_init
**
** Description      This callout function is executed by AV when it is
**                  started by calling BTA_AvkEnable().  This function can be
**                  used by the phone to initialize audio paths or for other
**                  initialization purposes.
**
**
** Returns          Stream codec and content protection capabilities info.
**
*******************************************************************************/
extern bool bta_avk_co_audio_init(uint8_t *p_codec_type, uint8_t *p_codec_info,
                                    uint8_t *p_num_protect, uint8_t *p_protect_info, uint8_t index);

/*******************************************************************************
**
** Function         bta_avk_co_audio_disc_res
**
** Description      This callout function is executed by AV to report the
**                  number of stream end points (SEP) were found during the
**                  AVDT stream discovery process.
**
**
** Returns          void.
**
*******************************************************************************/
extern void bta_avk_co_audio_disc_res(tBTA_AVK_HNDL hndl, uint8_t num_seps,
                    uint8_t num_snk, uint8_t num_src, const RawAddress& addr, uint16_t uuid_local);

/*******************************************************************************
**
** Function         bta_avk_co_audio_getconfig
**
** Description      This callout function is executed by AV to retrieve the
**                  desired codec and content protection configuration for the
**                  audio stream.
**
**
** Returns          Stream codec and content protection configuration info.
**
*******************************************************************************/
extern uint8_t bta_avk_co_audio_getconfig(tBTA_AVK_HNDL hndl, tBTA_AVK_CODEC codec_type,
                                       uint8_t *p_codec_info, uint8_t *p_sep_info_idx, uint8_t seid,
                                       uint8_t *p_num_protect, uint8_t *p_protect_info);

/*******************************************************************************
**
** Function         bta_avk_co_audio_setconfig
**
** Description      This callout function is executed by AV to set the
**                  codec and content protection configuration of the audio stream.
**
**
** Returns          void
**
*******************************************************************************/
extern void bta_avk_co_audio_setconfig(tBTA_AVK_HNDL hndl, tBTA_AVK_CODEC codec_type,
                                        uint8_t *p_codec_info, uint8_t seid, const RawAddress& addr,
                                        uint8_t num_protect, uint8_t *p_protect_info,uint8_t t_local_sep, uint8_t avdt_handle);


/*******************************************************************************
**
** Function         bta_avk_co_audio_open
**
** Description      This function is called by AV when the audio stream connection
**                  is opened.
**                  BTA-AV maintains the MTU of A2DP streams.
**                  If this is the 2nd audio stream, mtu is the smaller of the 2
**                  streams.
**
** Returns          void
**
*******************************************************************************/
extern void bta_avk_co_audio_open(tBTA_AVK_HNDL hndl,
                                 tBTA_AVK_CODEC codec_type, uint8_t *p_codec_info,
                                 uint16_t mtu);

/*******************************************************************************
**
** Function         bta_avk_co_audio_close
**
** Description      This function is called by AV when the audio stream connection
**                  is closed.
**                  BTA-AV maintains the MTU of A2DP streams.
**                  When one stream is closed and no other audio stream is open,
**                  mtu is reported as 0.
**                  Otherwise, the MTU remains open is reported.
**
** Returns          void
**
*******************************************************************************/
extern void bta_avk_co_audio_close(tBTA_AVK_HNDL hndl, tBTA_AVK_CODEC codec_type,
                                  uint16_t mtu);

/*******************************************************************************
**
** Function         bta_avk_co_audio_start
**
** Description      This function is called by AV when the audio streaming data
**                  transfer is started.
**
**
** Returns          void
**
*******************************************************************************/
extern void bta_avk_co_audio_start(tBTA_AVK_HNDL hndl, tBTA_AVK_CODEC codec_type,
                                  uint8_t *p_codec_info, bool *p_no_rtp_hdr);

/*******************************************************************************
**
** Function         bta_avk_co_audio_stop
**
** Description      This function is called by AV when the audio streaming data
**                  transfer is stopped.
**
**
** Returns          void
**
*******************************************************************************/
extern void bta_avk_co_audio_stop(tBTA_AVK_HNDL hndl, tBTA_AVK_CODEC codec_type);

/*******************************************************************************
 **
 ** Function         bta_avk_co_audio_sink_data_path
 **
 ** Description      Dummy Function, Required just because of co fuctions structure definition
 **
 ** Returns          NULL
 **
 *******************************************************************************/
extern void * bta_avk_co_audio_sink_data_path(tBTA_AVK_CODEC codec_type,
                                                    uint32_t *p_len, uint32_t *p_timestamp);

/*******************************************************************************
**
** Function         bta_avk_co_audio_delay
**
** Description      Dummy Function, Required just because of co-fuctions structure definition
**
**
** Returns          void
**
*******************************************************************************/
extern void bta_avk_co_audio_delay(tBTA_AVK_HNDL hndl, uint16_t delay);

/*******************************************************************************
**
** Function         bta_avk_co_get_cp_flag
**
** Description      Required to get cp flag
**
**
** Returns          void
**
*******************************************************************************/
extern uint8_t bta_avk_co_get_cp_flag(void);

/*******************************************************************************
**
** Function         bta_avk_co_set_cp_flag
**
** Description      Required to set cp flag
**
**
** Returns          void
**
*******************************************************************************/
extern bool bta_avk_co_set_cp_flag(uint8_t flag);

/*******************************************************************************
**
** Function         bta_avk_co_cp_is_active
**
** Description      reguired to whether CP is active or not
**
**
** Returns          bool
**
*******************************************************************************/
extern bool bta_avk_co_cp_is_active();

uint16_t get_codec_id (uint8_t * p_codec_info);
uint32_t get_vendor_id (uint8_t * p_codec_info);
uint8_t get_vendor_specific_codec(uint8_t * p_codec_info);
uint8_t get_codec_type (uint8_t * p_codec_info);

#endif /* BTA_AVK_CO_H */
