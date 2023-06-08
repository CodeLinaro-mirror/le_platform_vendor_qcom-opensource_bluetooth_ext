/*
 * Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef ANDROID_INCLUDE_BT_HH_VENDOR_H
#define ANDROID_INCLUDE_BT_HH_VENDOR_H

#include <hardware/bluetooth.h>

#define BT_PROFILE_HID_VENDOR_ID "hid_vendor"
#ifdef USE_MUSL
#ifdef __cplusplus
extern "C" {
#endif
#else
__BEGIN_DECLS
#endif

/** Callback to send raw hid data .*/
typedef void (*btvendor_raw_hid_data_callback)(const RawAddress& bda, uint8_t* rpt,
                                      uint16_t len,bool rpt_id_flag);

/** Callback invoked when the MTU for a given connection changes */
typedef void (*cfg_mtu_callback)(const RawAddress& bda, uint16_t mtu, uint8_t status);

/** Callback invoked when the connection parameters for a given connection
 * changes */
typedef void (*conn_params_updated_callback)(const RawAddress& bda, uint16_t interval,
                                      uint16_t latency, uint16_t timeout,
                                      uint8_t status);

/** BT-HH vendor callback structure. */
typedef struct {
    /** set to sizeof(BtHhVendorCallbacks) */
    size_t      size;
    btvendor_raw_hid_data_callback raw_hid_data_cb;
    cfg_mtu_callback cfg_mtu_cb;
    conn_params_updated_callback conn_params_cb;
} bthh_vendor_callbacks_t;

/** Represents the standard BT-HH Vendor interface. */
typedef struct {

    /** set to sizeof(BtHhVendorCallbacks) */
    size_t size;

    /** Register the BtHh vendor callbacks */
    bt_status_t (*init_vendor)(bthh_vendor_callbacks_t* callbacks);

    /** Configure the MTU for a given connection */
    bt_status_t (*configure_mtu)(const RawAddress& bd_addr, int mtu);

    /** Request a connection parameter update */
    bt_status_t (*conn_parameter_update)(const RawAddress& bd_addr,
                                        uint16_t min_interval, uint16_t max_interval,
                                        uint16_t latency, uint16_t timeout,
                                        uint16_t min_ce_len, uint16_t max_ce_len);

    /** Closes the hf client vendor interface. */
    void  (*cleanup_vendor)( void );
} bthh_vendor_interface_t;

#ifdef USE_MUSL
#ifdef __cplusplus
}
#endif
#else
__END_DECLS
#endif

#endif /* ANDROID_INCLUDE_BT_HH_VENDOR_H */
