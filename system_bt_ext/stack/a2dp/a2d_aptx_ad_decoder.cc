/******************************************************************************
    Copyright (c) 2016, The Linux Foundation. All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:
        * Redistributions of source code must retain the above copyright
          notice, this list of conditions and the following disclaimer.
        * Redistributions in binary form must reproduce the above
          copyright notice, this list of conditions and the following
          disclaimer in the documentation and/or other materials provided
          with the distribution.
        * Neither the name of The Linux Foundation nor the names of its
          contributors may be used to endorse or promote products derived
          from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
    WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
    ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
    BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
    BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
    WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
    OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
    IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 ******************************************************************************/

/******************************************************************************
 *
 *  Utility functions to help build and parse the aptX Codec Information
 *  Element and Media Payload.
 *
 ******************************************************************************/

#include "bt_target.h"

#include <string.h>
#include <dlfcn.h>
#include "osi/include/thread.h"
#include "bt_utils.h"
#include "a2dp_api.h"
#include "a2dp_int.h"
#include "a2d_aptx_ad_decoder.h"
#include "osi/include/log.h"


/******************************************************************************
**
** Function         A2D_BldAptxADDecoderInfo
**
******************************************************************************/
uint8_t A2D_BldAptxADDecoderInfo(uint8_t media_type, tA2DP_AVK_APTX_AD_CIE *p_ie, uint8_t *p_result)
{
    A2DP_TRACE_API("%s: - MediaType:%d", __func__, media_type);

    *p_result++ = A2D_AVK_APTX_AD_CODEC_LEN;
    *p_result++ = media_type << 4;
    *p_result++ = A2D_NON_A2DP_MEDIA_CT;
    *p_result++ = (uint8_t)(p_ie->vendorId & 0x000000FF);
    *p_result++ = (uint8_t)(p_ie->vendorId & 0x0000FF00)>> 8;
    *p_result++ = (uint8_t)(p_ie->vendorId & 0x00FF0000)>> 16;
    *p_result++ = (uint8_t)(p_ie->vendorId & 0xFF000000)>> 24;
    *p_result++ = (uint8_t)(p_ie->codecId & 0x00FF);
    *p_result++ = (uint8_t)(p_ie->codecId & 0xFF00) >> 8;
    *p_result++ = p_ie->sampleRate;
    *p_result++ = p_ie->channelMode;
    *p_result++ = p_ie->min_latency_ll;
    *p_result++ = p_ie->max_latency_ll;
    *p_result++ = p_ie->min_latency_hq;
    *p_result++ = p_ie->max_latency_hq;
    *p_result++ = p_ie->min_latency_tws;
    *p_result++ = p_ie->max_latency_tws;
    *p_result++ = p_ie->eoc0;
    *p_result++ = p_ie->eoc1;
    *p_result++ = p_ie->eoc2;
    memset(p_result, 0x0, sizeof(p_ie->reserved_data));
    p_result += sizeof(p_ie->reserved_data);

    return A2DP_SUCCESS;
}

/******************************************************************************
**
** Function         A2DP_ParseInfoAptxAdaptiveDecoder
**
******************************************************************************/
tA2DP_STATUS A2DP_ParseInfoAptxAdaptiveDecoder(tA2DP_AVK_APTX_AD_CIE *p_ie, uint8_t *p_info,
                                                    bool for_caps)
{
    tA2DP_STATUS status;
    uint8_t   losc;
    uint8_t   media_type;

    A2DP_TRACE_API("%s: - caps :%d", __func__, for_caps);

    if (p_ie == NULL || p_info == NULL)
    {
        A2DP_TRACE_ERROR("A2D_ParsAptxInfo - Invalid Params");
        return A2DP_INVALID_PARAMS;
    }

    losc    = *p_info++;
    media_type      = (*p_info++) >> 4;
    A2DP_TRACE_DEBUG("%s: losc %d, mmedia_typet %02x", __func__, losc, media_type);

    /* Check Codec Len and Media Type */
    if (losc != A2D_AVK_APTX_AD_CODEC_LEN || *p_info != A2D_NON_A2DP_MEDIA_CT) {
        A2DP_TRACE_ERROR("%s: wrong media type %02x", __func__, *p_info);
        return A2DP_WRONG_CODEC;
    }

    p_info++;
    p_ie->vendorId = (*p_info & 0x000000FF) |
                   (*(p_info + 1) << 8 & 0x0000FF00) |
                   (*(p_info + 2) << 16 & 0x00FF0000) |
                   (*(p_info + 3) << 24 & 0xFF000000);
    p_info += 4;

    p_ie->codecId = (*p_info & 0x00FF) | (*(p_info + 1) << 8 & 0xFF00);
    p_info += 2;

    if (p_ie->vendorId != A2DP_AVK_APTX_AD_VENDOR_ID ||
          p_ie->codecId != A2DP_AVK_APTX_AD_CODEC_ID_BLUETOOTH) {
      A2DP_TRACE_DEBUG("%s: This is not APTX-Adaptive: WRONG CODEC", __func__);
      return A2DP_WRONG_CODEC;
    }
    /* We don't need to check source type from src cap.
       We would check sampling rate */

    p_ie->sampleRate = *p_info & 0xF8;
    p_info++;

    p_ie->channelMode = *p_info & 0x1F;
    p_info++;

    A2DP_TRACE_DEBUG("%s: sampling_rate %02x, channel_mode %02x", __func__,p_ie->sampleRate, p_ie->channelMode);

    if (for_caps) return A2DP_SUCCESS;

    if (A2DP_BitsSet(p_ie->sampleRate) != A2DP_SET_ONE_BIT)
        return A2DP_BAD_SAMP_FREQ;
    if (A2DP_BitsSet(p_ie->channelMode) != A2DP_SET_ONE_BIT)
        return A2DP_BAD_CH_MODE;

    return A2DP_SUCCESS;

}

/*******************************************************************************
**
** Function         a2d_avk_aptx_ad_cfg_in_cap
**
** Description      This function checks whether an aptX codec configuration
**                  is allowable for the given codec capabilities.
**
** Returns          0 if ok, nonzero if error.
**
*******************************************************************************/
uint8_t a2d_avk_aptx_ad_cfg_in_cap(uint8_t *p_cfg, tA2DP_AVK_APTX_AD_CIE *p_cap)
{
    uint8_t           status = 0;
    tA2DP_AVK_APTX_AD_CIE   cfg_cie;

    A2DP_TRACE_API("%s", __func__);

    /* parse configuration */
    if ((status = A2DP_ParseInfoAptxAdaptiveDecoder(&cfg_cie, p_cfg, FALSE)) != 0)
    {
        A2DP_TRACE_ERROR("%s:, aptx parse failed", __func__);
        return status;
    }

    /* verify that each parameter is in range */

    /* sampling frequency */
    if ((cfg_cie.sampleRate & p_cap->sampleRate) == 0)
        status = A2DP_NS_SAMP_FREQ;
    /* channel mode */
    else if ((cfg_cie.channelMode & p_cap->channelMode) == 0)
        status = A2DP_NS_CH_MODE;

    return status;
}