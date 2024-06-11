/*
 * Copyright (C) 2016 The Linux Foundation. All rights reserved
 * Not a Contribution.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted (subject to the limitations in the
 * disclaimer below) provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 * * Neither the name of The Linux Foundation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
 * GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
 * HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
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
 * Changes from Qualcomm Innovation Center, Inc. are provided under the following license:
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 ******************************************************************************/

/************************************************************************************
 *
 *  Filename:      btif_vendor.cc
 *
 *  Description:   Vendor Bluetooth Interface
 *
 *
 ***********************************************************************************/

#include <hardware/vendor.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "bt_btif_vendor"

#include "osi/include/properties.h"
#ifdef SUPPORT_ESL_AP
#include <hardware/hardware.h>
#include <hardware/bluetooth.h>
#include <hardware/bt_gatt_client.h>
#endif
#include "bt_utils.h"
#include "btif_common.h"
#include "btif_util.h"
#include "btif_profile_queue.h"
#include "stack_manager.h"
#include "l2cdefs.h"
#include "l2c_api.h"
#include "stack_config.h"
#include "btm_api.h"
#include "profile_config.h"
#include "btif_tws_plus.h"
#include "btif_api.h"
#ifdef SUPPORT_ESL_AP
#include "bta_closure_api.h"
#include "device/include/controller.h"

using base::Bind;
#endif

#if TEST_APP_INTERFACE == TRUE
#include <bt_testapp.h>
#endif

#define BTA_SERVICE_ID_TO_SERVICE_MASK(id)  (1 << (id))
#define CALLBACK_TIMER_PERIOD_MS      (60000)
#define BTIF_VENDOR_BREDR_CLEANUP 1

typedef struct {
  RawAddress bd_addr; /* BD address peer device. */
  uint16_t error;
  uint16_t error_info;
  uint32_t event_mask;
  uint8_t power_level;
  uint8_t rssi;
  uint8_t link_quality;
  uint8_t lmp_ver;
  uint16_t lmp_subver;
  uint16_t manufacturer_id;
  bool is_valid;
} BTIF_VND_IOT_INFO_CB_DATA;

extern bt_status_t btif_in_execute_service_request(tBTA_SERVICE_ID service_id,
                                               bool b_enable);
extern bt_status_t btif_storage_get_remote_device_property(RawAddress *remote_bd_addr,
                                               bt_property_t *property);
extern tBTM_STATUS BTM_ReadRemoteVersion(const RawAddress& addr, uint8_t* lmp_version,
                                  uint16_t* manufacturer,
                                  uint16_t* lmp_sub_version);
extern bool interface_ready(void);
//extern void set_prop_callouts(bt_property_callout_t *callouts);
void btif_vendor_cleanup_iot_broadcast_timer(void);

btvendor_callbacks_t *bt_vendor_callbacks = NULL;
static alarm_t *broadcast_cb_timer = NULL;
static void btif_broadcast_timer_cb(UNUSED_ATTR void *data);
BTIF_VND_IOT_INFO_CB_DATA broadcast_cb_data;

#if TEST_APP_INTERFACE == TRUE
extern const btl2cap_interface_t *btif_l2cap_get_interface(void);
extern const btrfcomm_interface_t *btif_rfcomm_get_interface(void);
extern const btmcap_interface_t *btif_mcap_get_interface(void);
extern const btgatt_test_interface_t *btif_gatt_test_get_interface(void);
extern const btsmp_interface_t *btif_smp_get_interface(void);
extern const btgap_interface_t *btif_gap_get_interface(void);
#ifdef SUPPORT_ESL_AP
extern const BleTestScannerInterface* get_ble_test_scanner_instance(void);
extern const btgatt_client_interface_t *bt_btif_vendor_gattc_get_interface(void);
#endif
#endif

#ifdef SUPPORT_ESL_AP
extern void BTM_StartEncryption(RawAddress address, uint8_t *rand, uint16_t ediv, const BT_OCTET16& ltk, tBTM_SEC_CBACK* p_callback);
extern void BTM_Deregister_Bgdev_List(tGATT_IF gatt_if);
vendor_acl_state_changed_callback vendor_acl_state_changed_cb = NULL;
btgatt_client_callbacks_t  *vendor_gattc_callback = NULL;
#endif

/*******************************************************************************
** VENDOR INTERFACE FUNCTIONS
*******************************************************************************/

/*******************************************************************************
**
** Function         btif_vendor_init
**
** Description     initializes the vendor interface
**
** Returns         bt_status_t
**
*******************************************************************************/
static bt_status_t init( btvendor_callbacks_t* callbacks)
{
    bt_vendor_callbacks = callbacks;
    broadcast_cb_timer = alarm_new("btif_vnd.cb_timer");
    LOG_INFO(LOG_TAG,"init");
    LOG_INFO(LOG_TAG,"init done");
    return BT_STATUS_SUCCESS;
}

static bool vendor_interface_ready(void) {
  return bt_vendor_callbacks != NULL;
}

static void btif_vendor_bredr_cleanup_event(uint16_t event, char *p_param)
{
    tBTA_SERVICE_MASK service_mask;
    uint32_t i;
    service_mask = btif_get_enabled_services_mask();
    for (i = 0; i <= BTA_MAX_SERVICE_ID; i++)
    {
        if (i != BTA_BLE_SERVICE_ID && (service_mask &
              (tBTA_SERVICE_MASK)(BTA_SERVICE_ID_TO_SERVICE_MASK(i))))
        {
            btif_in_execute_service_request(i, FALSE);
        }
    }
    btif_queue_release();
    //btif_dm_bredr_disable();
    HAL_CBACK(bt_vendor_callbacks, bredr_cleanup_cb, true);
}

static void btif_vendor_send_iot_info_cb(uint16_t event, char *p_param)
{
    broadcast_cb_data.is_valid = false;
    HAL_CBACK(bt_vendor_callbacks, iot_device_broadcast_cb,
            &broadcast_cb_data.bd_addr, broadcast_cb_data.error,
            broadcast_cb_data.error_info, broadcast_cb_data.event_mask,
            broadcast_cb_data.lmp_ver, broadcast_cb_data.lmp_subver,
            broadcast_cb_data.manufacturer_id, broadcast_cb_data.power_level,
            broadcast_cb_data.rssi, broadcast_cb_data.link_quality);
}

void btif_broadcast_timer_cb(UNUSED_ATTR void *data) {
    btif_transfer_context(btif_vendor_send_iot_info_cb, 1, NULL, 0, NULL);
}

void btif_vendor_cleanup_iot_broadcast_timer()
{
    if(broadcast_cb_timer) {
        alarm_free(broadcast_cb_timer);
        broadcast_cb_timer = NULL;
    }
}

void btif_vendor_iot_device_broadcast_event(RawAddress* bd_addr,
                uint16_t error, uint16_t error_info, uint32_t event_mask,
                uint8_t power_level, uint8_t rssi, uint8_t link_quality)
{
    uint8_t lmp_ver;
    uint16_t lmp_subver, manufacturer_id;

    BTM_ReadRemoteVersion(*bd_addr, &lmp_ver, &manufacturer_id, &lmp_subver);
    if(error == BT_SOC_A2DP_GLITCH || error == BT_HOST_A2DP_GLITCH)
    {
        if(broadcast_cb_data.is_valid)
        {
            broadcast_cb_data.error_info = broadcast_cb_data.error_info|error_info;
            if(error == BT_SOC_A2DP_GLITCH && broadcast_cb_data.error_info == BT_HOST_A2DP_GLITCH)
            {
                broadcast_cb_data.event_mask = broadcast_cb_data.event_mask|event_mask;
                broadcast_cb_data.power_level = power_level;
                broadcast_cb_data.rssi = rssi;
                broadcast_cb_data.link_quality = link_quality;
            }
            return;
        }
        else if(broadcast_cb_timer)
        {
            broadcast_cb_data.bd_addr = *bd_addr;
            broadcast_cb_data.error = error;
            broadcast_cb_data.error_info = error_info;
            broadcast_cb_data.event_mask = event_mask;
            broadcast_cb_data.power_level = power_level;
            broadcast_cb_data.rssi = rssi;
            broadcast_cb_data.link_quality = link_quality;
            broadcast_cb_data.lmp_ver = lmp_ver;
            broadcast_cb_data.lmp_subver = lmp_subver;
            broadcast_cb_data.manufacturer_id = manufacturer_id;
            broadcast_cb_data.is_valid = true;

            alarm_set(broadcast_cb_timer, CALLBACK_TIMER_PERIOD_MS, btif_broadcast_timer_cb, NULL);
            return;
        }
    }

    HAL_CBACK(bt_vendor_callbacks, iot_device_broadcast_cb, bd_addr,
            error, error_info, event_mask, lmp_ver, lmp_subver,
            manufacturer_id, power_level, rssi, link_quality);
}
static void bredrcleanup(void)
{
    LOG_INFO(LOG_TAG,"bredrcleanup");
    btif_transfer_context(btif_vendor_bredr_cleanup_event,BTIF_VENDOR_BREDR_CLEANUP,
                          NULL, 0, NULL);
}

#if HCI_RAW_CMD_INCLUDED == TRUE
// Callback invoked on receiving HCI event
static void btif_vendor_hci_event_callback ( tBTM_RAW_CMPL *p)
{
  BTIF_TRACE_DEBUG("btif_vendor_hci_event_callback");
  if((p != NULL) && (bt_vendor_callbacks!= NULL)
      && (bt_vendor_callbacks->hci_event_recv_cb != NULL)) {

    BTIF_TRACE_DEBUG("btif_vendor_hci_event_callback-sending HAL_CBACK");
    HAL_CBACK(bt_vendor_callbacks, hci_event_recv_cb, p->event_code, p->p_param_buf,
                                                                p->param_len);
  }
}
#endif

int hci_cmd_send(uint16_t opcode, uint8_t* buf, uint8_t len)
{
    BTIF_TRACE_DEBUG("hci_cmd_send");
    return BTA_DmHciRawCommand(opcode, len, buf, btif_vendor_hci_event_callback);
}

#ifdef SUPPORT_ESL_AP
static hci_event_cb vsc_cb;
static bool hci_event_cb_needed;

using hci_cmd_cb = base::Callback<void(uint8_t* /* return_parameters */, uint16_t /* return_parameters_length*/)>;
extern void btu_hcif_send_cmd_with_cb(const tracked_objects::Location& posted_from,
                                      uint16_t opcode, uint8_t* params, uint8_t params_len, hci_cmd_cb cb);

static void btif_vendor_hci_event_callback_entry(unsigned char* param, unsigned short param_len)
{
  BTIF_TRACE_DEBUG("%s, callback:%d, %d", __FUNCTION__, hci_event_cb_needed, vsc_cb.is_null());
  if((param != NULL) && hci_event_cb_needed) {
    vsc_cb.Run(param, param_len);
  }
}

int hci_cmd_send_with_cb(uint16_t opcode, uint8_t* buf, uint8_t len, hci_event_cb cb)
{
    BTIF_TRACE_DEBUG("hci_cmd_send_with_cb: opcode=%d", opcode);
    if (opcode != 0) {
        vsc_cb = cb;
        hci_event_cb_needed = true;
        btu_hcif_send_cmd_with_cb(FROM_HERE, opcode, buf, len, base::Bind(&btif_vendor_hci_event_callback_entry));
        return HCI_SUCCESS;
    }
    else {
        hci_event_cb_needed = false;
        return HCI_SUCCESS;
    }
}

static hci_status_cb vsc_status_cb;
static bool hci_status_cb_needed;

static void btif_vendor_hci_status_callback_entry (tBTM_RAW_STATUS *p)
{
  BTIF_TRACE_DEBUG("%s, callback:%d, %d", __FUNCTION__, hci_status_cb_needed, vsc_status_cb.is_null());
  if((p != NULL) && hci_status_cb_needed) {
    vsc_status_cb.Run(p->opcode, p->status);
  }
}

int hci_cmd_send_with_status_cb(uint16_t opcode, uint8_t* buf, uint8_t len, hci_status_cb cb)
{
    BTIF_TRACE_DEBUG("hci_cmd_send_with_status_cb: opcode=%d", opcode);
    if (opcode != 0) {
        vsc_status_cb = cb;
        hci_status_cb_needed = true;
        BTM_Hci_Raw_Command_with_status_cb(opcode, len, buf, (void*)btif_vendor_hci_status_callback_entry);
        return HCI_SUCCESS;
    }
    else {
        hci_status_cb_needed = false;
        return HCI_SUCCESS;
    }
}

int hci_clear_white_list(uint8_t gatt_if)
{
  BTM_Deregister_Bgdev_List((tGATT_IF)gatt_if);
  return HCI_SUCCESS;
}

int hci_get_white_list_size(void)
{
    return controller_get_interface()->get_ble_white_list_size();
}
#endif

static void set_wifi_state(bool status)
{
    LOG_INFO(LOG_TAG,"setWifiState :%d", status);
    BTA_DmSetWifiState(status);
}

static bool get_profile_info(profile_t profile, profile_info_t feature_name)
{
    LOG_INFO(LOG_TAG,"get_profile_info :%d", profile);
    return profile_feature_fetch(profile,feature_name);
}

/*static void set_property_callouts(bt_property_callout_t* property_callouts)
{
    set_prop_callouts(property_callouts);
}*/

static void cleanup(void)
{
    LOG_INFO(LOG_TAG,"cleanup");
    if (bt_vendor_callbacks)
        bt_vendor_callbacks = NULL;
}

static void set_voip_network_type_wifi_hci_cmd_complete(tBTM_VSC_CMPL* p_data)
{
    LOG_INFO(LOG_TAG,"In set_voip_network_type_wifi_hci_cmd_complete");
    uint8_t         *stream,  status, subcmd;
    uint16_t        opcode, length;

    if (p_data && (stream = (uint8_t*)p_data->p_param_buf))
    {
        opcode = p_data->opcode;
        length = p_data->param_len;
        STREAM_TO_UINT8(status, stream);
        STREAM_TO_UINT8(subcmd, stream);
        BTIF_TRACE_DEBUG("%s opcode = 0x%04X, length = %d, status = %d, subcmd = %d",
                __FUNCTION__, opcode, length, status, subcmd);
        if (status == HCI_SUCCESS)
        {
            BTIF_TRACE_DEBUG("btm_SetVoipNetworkTypeWifi status success");
        }
    }
}

/*******************************************************************************
**
** Function         voip_network_type_wifi
**
** Description      BT app updates the connectivity network used for VOIP as Wifi
**
** Returns          bt_status_t
**
*******************************************************************************/
bt_status_t voip_network_type_wifi(bthf_voip_state_t isVoipStarted,
                                           bthf_voip_call_network_type_t isNetworkWifi)
{
    LOG_INFO(LOG_TAG,"In voip_network_type_wifi");
    uint8_t           cmd[3], *p_cursor;
    uint8_t           sub_cmd = HCI_VSC_SUBCODE_VOIP_NETWORK_WIFI;

    p_cursor = cmd;
    memset(cmd, 0, 3);

    *p_cursor++ = sub_cmd;
    *p_cursor++ = isVoipStarted;
    *p_cursor++ = isNetworkWifi;

    BTM_VendorSpecificCommand(HCI_VSC_VOIP_NETWORK_WIFI_OCF, sizeof(cmd),
            cmd, set_voip_network_type_wifi_hci_cmd_complete);
    return BT_STATUS_SUCCESS;
}

#ifdef SUPPORT_ESL_AP
int register_vendor_acl_callback(vendor_acl_state_changed_callback cb)
{
    vendor_acl_state_changed_cb = cb;
    return 0;
}

int register_vendor_gattc_callback(void *cb)
{
    vendor_gattc_callback = (btgatt_client_callbacks_t*) cb;
    return 0;
}

vendor_vs_evt_callback vs_event_cb = NULL;
void hcif_vs_event_callback(uint8_t* p, uint8_t len)
{
    BTIF_TRACE_WARNING("%s : enter, len:%d, code:%x", __func__, len, p[0]);
    if (vs_event_cb != NULL && len != 0) {
        std::vector<uint8_t> vs_data(p, p+len);
        do_in_jni_thread(Bind(vs_event_cb, std::move(vs_data)));
    }
}

void register_vs_event_callback(vendor_vs_evt_callback p_cb) {
    vs_event_cb = p_cb;
}

tBTM_SEC_CBACK *btm_sec_cb = NULL;
void btm_sec_callback(const RawAddress* bd_addr, tBT_TRANSPORT trasnport,
                             void* p_ref_data, tBTM_STATUS result)
{
    BTIF_TRACE_WARNING("%s : enter, result:%d", __func__, result);
    if (btm_sec_cb != NULL) {
        do_in_jni_thread(Bind(btm_sec_cb, bd_addr, trasnport, p_ref_data, result));
    }
}

void bta_startEncryption(RawAddress address, std::vector<uint8_t> rand, uint16_t ediv, const BT_OCTET16& ltk, tBTM_SEC_CBACK* p_callback)
{
    BTM_StartEncryption(address, rand.data(), ediv, ltk, p_callback);
}


void start_ble_encryption(RawAddress address, uint8_t *rand, uint16_t ediv, const uint8_t *ltk, tBTM_SEC_CBACK* p_callback)
{
    btm_sec_cb = p_callback;
    BT_OCTET16 ltk_val;
    STREAM_TO_ARRAY(ltk_val, ltk, BT_OCTET16_LEN);
    std::vector<uint8_t> rand_val(rand, rand + BT_OCTET8_LEN);
    bta_startEncryption(address, std::move(rand_val), ediv, ltk_val, btm_sec_callback);
    //do_in_bta_thread(FROM_HERE, Bind(bta_startEncryption, address, std::move(rand_val), ediv, ltk_val, btm_sec_callback));
}

void set_conn_scan_parameter_impl(uint32_t scan_interval, uint32_t scan_window) {

    BTA_DmSetBleConnScanParams(scan_interval, scan_window);
}

bt_status_t set_conn_scan_parameter(uint32_t scan_interval, uint32_t scan_window) {
//  CHECK_BTGATT_INIT();
  return do_in_jni_thread(Bind(
      base::IgnoreResult(&set_conn_scan_parameter_impl), scan_interval, scan_window));
}
#endif

/*******************************************************************************
**
** Function         get_testapp_interface
**
** Description      Get the Test interface
**
** Returns          btvendor_interface_t
**
*******************************************************************************/
#if TEST_APP_INTERFACE == TRUE
static const void* get_testapp_interface(int test_app_profile)
{
    if (interface_ready() == FALSE) {
        return NULL;
    }
    ALOGI("get_testapp_interface %d", test_app_profile);
#ifdef SUPPORT_ESL_AP
    BTIF_TRACE_EVENT("%s %d", __FUNCTION__, test_app_profile);
#endif
    switch(test_app_profile) {
        case TEST_APP_L2CAP:
            return btif_l2cap_get_interface();
        case TEST_APP_RFCOMM:
            return btif_rfcomm_get_interface();
        case TEST_APP_MCAP:
           return btif_mcap_get_interface();
        case TEST_APP_GATT:
           return btif_gatt_test_get_interface();
        case TEST_APP_SMP:
           return btif_smp_get_interface();
        case TEST_APP_GAP:
           return btif_gap_get_interface();
#ifdef SUPPORT_ESL_AP
        case TEST_APP_SCANNER:
           return get_ble_test_scanner_instance();
        case TEST_APP_GATTC:
           return bt_btif_vendor_gattc_get_interface();
#endif
        default:
            return NULL;
    }
    return NULL;
}
#endif

static void get_role_req(const RawAddress* bd_addr)
{
    /* sanity check */
    if (vendor_interface_ready() == FALSE)
       return 0;

    return btif_dm_get_role_req(bd_addr);
}

static int switch_role_req(const RawAddress *bd_addr, uint8_t new_role)
{
    /* sanity check */
    if (vendor_interface_ready() == FALSE)
        return 0;

    return btif_dm_switch_role_req(bd_addr, new_role);
}

static const btvendor_interface_t btvendorInterface = {
    sizeof(btvendorInterface),
    init,
    hci_cmd_send,
#ifdef SUPPORT_ESL_AP
    hci_cmd_send_with_cb,
    hci_cmd_send_with_status_cb,
    hci_clear_white_list,
    hci_get_white_list_size,
    register_vendor_acl_callback,
    register_vendor_gattc_callback,
    register_vs_event_callback,
    start_ble_encryption,
    set_conn_scan_parameter,
#endif
#if TEST_APP_INTERFACE == TRUE
    get_testapp_interface,
#else
    NULL,
#endif
    bredrcleanup,
    set_wifi_state,
    get_profile_info,
    NULL,//set_property_callouts,
    cleanup,
    voip_network_type_wifi,
    get_role_req,
    switch_role_req,
};

/*******************************************************************************
** LOCAL FUNCTIONS
*******************************************************************************/

/*******************************************************************************
**
** Function         btif_vendor_get_interface
**
** Description      Get the vendor callback interface
**
** Returns          btvendor_interface_t
**
*******************************************************************************/
const btvendor_interface_t *btif_vendor_get_interface()
{
    BTIF_TRACE_EVENT("%s", __FUNCTION__);
    return &btvendorInterface;
}
