/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted (subject to the limitations in the
 * disclaimer below) provided that the following conditions are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *    * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *  NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
 *  GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
 *  HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 *  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 *  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 *  IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef INCLUDE_VENDOR_AP_H
#define INCLUDE_VENDOR_AP_H
#ifdef SUPPORT_VENDOR_AP
#include "raw_address.h"
#include <bluetooth/uuid.h>
#include <hardware/bluetooth.h>
#include <dlfcn.h>

#define VENDOR_AP_CLIENT_UUID "1122A00C-0000-0000-0123-456789ABCDEF"
#define VENDOR_AP_INTERFACE_STRING "vendorApInterface"

/******************************************************************************
 *
 * BLE AP
 *
 * ***************************************************************************/
#ifndef VENDOR_AP_IS_SUPPORTED
#define VENDOR_AP_IS_SUPPORTED TRUE
#endif

#define VENDOR_AP_MODULE_ID          "vendorap"
#define CERT_CMD_ARG_SIZE            200
#define CERT_CMD_PARAMETER_COUNT_MIN 1 //cert cmd self

//msg must use new to create
typedef void (*post_to_bt_app)(void *msg);

typedef struct {

       /** set to sizeof(vendor_ap_interface_t) */
       size_t size;

       int (*init)(const bt_interface_t* itf, post_to_bt_app post_to_bt);

       int (*deInit)(void);

       int (*cert)(uint8_t parameter_count, char user_cmd[][CERT_CMD_ARG_SIZE]);
#if 0
       bool (*bondStateChange_cb)(bt_status_t status,
              RawAddress* remote_bd_addr,
              bt_bond_state_t state);

       bool (*acl_state_changed_cb)(bt_status_t status,
              RawAddress *remote_bd_addr,
              bt_acl_state_t state);

       bool (*gattConnected_cb)(int conn_id,
              int status,
              int client_if,
              const RawAddress& remote_bd_addr);

       void (*register_cli_cb)(int status,
              int client_if,
              const Uuid& app_uuid);

       void (*read_char_cb)(uint16_t conn_id, uint8_t status,
                            uint16_t handle, uint16_t len, uint8_t* value,
                            void* data);
#endif
} vendor_ap_interface_t;

typedef struct {
    struct hw_device_t common;
    const vendor_ap_interface_t* (*get_ap_interface)();
} vendor_ap_device_t;

typedef vendor_ap_device_t ap_module_t;
#endif
#endif
