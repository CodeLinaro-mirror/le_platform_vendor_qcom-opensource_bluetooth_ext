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

/*
 *
 *  Copyright (C) 2016 The Android Open Source Project
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

#define LOG_TAG "bt_btif_scanner_vendor"

#include <base/bind.h>
#include <base/threading/thread.h>
#include <errno.h>
#include <hardware/bluetooth.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_set>
#include "device/include/controller.h"

#include "btif_common.h"
#include "btif_util.h"

#include <hardware/bt_gatt.h>

#include "advertise_data_parser.h"
#include "bta_api.h"
#include "bta_closure_api.h"
#include "bta_gatt_api.h"
#include "btif_config.h"
#include "btif_dm.h"
#include "btif_gatt.h"
#include "btif_gatt_util.h"
#include "btif_storage.h"
#include "osi/include/log.h"
#include "stack_manager.h"
#include "bt_testapp.h"

using base::Bind;
using base::Owned;
using std::vector;

static BleTestScannerInterface::ScanResultCallback passive_scan_result_callback;

extern void BTM_GetScanStatus(bool *running, uint8_t *scan_type);

namespace {

// all access to this variable should be done on the jni thread
static std::set<RawAddress> remote_bdaddr_cache;
static std::queue<RawAddress> remote_bdaddr_cache_ordered;
const size_t remote_bdaddr_cache_max_size = 1024;

void btif_address_cache_add(const RawAddress& p_bda, uint8_t addr_type) {
  // Remove the oldest entries
  while (remote_bdaddr_cache.size() >= remote_bdaddr_cache_max_size) {
    const RawAddress& raw_address = remote_bdaddr_cache_ordered.front();
    remote_bdaddr_cache.erase(raw_address);
    remote_bdaddr_cache_ordered.pop();
  }
  remote_bdaddr_cache.insert(p_bda);
  remote_bdaddr_cache_ordered.push(p_bda);
}

bool btif_address_cache_find(const RawAddress& p_bda) {
  return (remote_bdaddr_cache.find(p_bda) != remote_bdaddr_cache.end());
}

void btif_address_cache_init(void) {
  remote_bdaddr_cache.clear();
  remote_bdaddr_cache_ordered = {};
}



void bta_passive_scan_results_cb_impl(RawAddress bd_addr, tBT_DEVICE_TYPE device_type,
                              int8_t rssi, uint8_t addr_type,
                              uint16_t ble_evt_type, uint8_t ble_primary_phy,
                              uint8_t ble_secondary_phy,
                              uint8_t ble_advertising_sid, int8_t ble_tx_power,
                              uint16_t ble_periodic_adv_int,
                              vector<uint8_t> value) {
  uint8_t remote_name_len;
  bt_device_type_t dev_type;
  bt_property_t properties;

  const uint8_t* p_eir_remote_name = AdvertiseDataParser::GetFieldByType(
      value, BTM_EIR_COMPLETE_LOCAL_NAME_TYPE, &remote_name_len);

  if (p_eir_remote_name == NULL) {
    p_eir_remote_name = AdvertiseDataParser::GetFieldByType(
        value, BT_EIR_SHORTENED_LOCAL_NAME_TYPE, &remote_name_len);
  }

  if ((addr_type != BLE_ADDR_RANDOM) || (p_eir_remote_name)) {
    if (!btif_address_cache_find(bd_addr)) {
      btif_address_cache_add(bd_addr, addr_type);

      if (p_eir_remote_name) {
        if (remote_name_len > BD_NAME_LEN + 1 ||
            (remote_name_len == BD_NAME_LEN + 1 &&
             p_eir_remote_name[BD_NAME_LEN] != '\0')) {
          LOG_INFO(LOG_TAG,
                   "%s dropping invalid packet - device name too long: %d",
                   __func__, remote_name_len);
          return;
        }

        bt_bdname_t bdname;
        memcpy(bdname.name, p_eir_remote_name, remote_name_len);
        if (remote_name_len < BD_NAME_LEN + 1)
          bdname.name[remote_name_len] = '\0';

        LOG_VERBOSE(LOG_TAG, "%s BLE device name=%s len=%d dev_type=%d",
                    __func__, bdname.name, remote_name_len, device_type);
        btif_dm_update_ble_remote_properties(bd_addr, bdname.name, device_type);
      }
    }
  }

  dev_type = (bt_device_type_t)device_type;
  BTIF_STORAGE_FILL_PROPERTY(&properties, BT_PROPERTY_TYPE_OF_DEVICE,
                             sizeof(dev_type), &dev_type);
  btif_storage_set_remote_device_property(&(bd_addr), &properties);

  btif_storage_set_remote_addr_type(&bd_addr, addr_type);
  if (passive_scan_result_callback)
      passive_scan_result_callback.Run(ble_evt_type, addr_type,
            &bd_addr, ble_primary_phy, ble_secondary_phy, ble_advertising_sid,
            ble_tx_power, rssi, ble_periodic_adv_int, std::move(value));
}



void bta_passive_scan_results_cb(tBTA_DM_SEARCH_EVT event, tBTA_DM_SEARCH* p_data) {
  uint8_t len;

  BTIF_TRACE_WARNING("%s : enter", __func__);
  if (event == BTA_DM_INQ_CMPL_EVT) {
    BTIF_TRACE_DEBUG("%s  BLE observe complete. Num Resp %d", __func__,
                     p_data->inq_cmpl.num_resps);
    return;
  }

  if (event != BTA_DM_INQ_RES_EVT) {
    BTIF_TRACE_WARNING("%s : Unknown event 0x%x", __func__, event);
    return;
  }

  vector<uint8_t> value;
  if (p_data->inq_res.p_eir) {
    value.insert(value.begin(), p_data->inq_res.p_eir,
                 p_data->inq_res.p_eir + p_data->inq_res.eir_len);

    if (AdvertiseDataParser::GetFieldByType(
            value, BTM_EIR_COMPLETE_LOCAL_NAME_TYPE, &len)) {
      p_data->inq_res.remt_name_not_required = true;
    }
  }

  tBTA_DM_INQ_RES* r = &p_data->inq_res;
  do_in_jni_thread(Bind(bta_passive_scan_results_cb_impl, r->bd_addr, r->device_type,
                        r->rssi, r->ble_addr_type, r->ble_evt_type,
                        r->ble_primary_phy, r->ble_secondary_phy,
                        r->ble_advertising_sid, r->ble_tx_power,
                        r->ble_periodic_adv_int, std::move(value)));
}


class BleTestScannerInterfaceImpl : public BleTestScannerInterface{
  ~BleTestScannerInterfaceImpl(){};

  void ScanWithCb(bool start, ScanResultCallback cb) override {
    BTIF_TRACE_WARNING("%s : enter", __func__);
    if (!stack_manager_get_interface()->get_stack_is_running()) return;
    passive_scan_result_callback = cb;
    do_in_jni_thread(Bind(
        [](bool start) {
          if (!start) {
            do_in_bta_thread(FROM_HERE,
                             Bind(&BTA_DmBleObserve, false, 0, nullptr));
            return;
          }

          BTIF_TRACE_WARNING("ScanWithCb_callback : enable");
          btif_address_cache_init();

          do_in_bta_thread(
              FROM_HERE, Bind(&BTA_DmBleObserve, true, 0, bta_passive_scan_results_cb));
        },
        start));
  }

  void SetScanParameters(int scan_phy, std::vector<uint32_t> scan_interval,
                         std::vector<uint32_t> scan_window, uint8_t scan_type,
                         Callback cb) override {
    if (!stack_manager_get_interface()->get_stack_is_running()) return;
    do_in_bta_thread(
        FROM_HERE, base::Bind(&BTM_BleSetScanParams, scan_phy, scan_interval,
                              scan_window, scan_type,
                              jni_thread_wrapper(FROM_HERE, std::move(cb))));
  }

  void GetScanStatus(bool *running, uint8_t *scan_type) {
    BTM_GetScanStatus(running, scan_type);
  }
};

BleTestScannerInterface * btLeTestScannerInstance = nullptr;

}  // namespace

BleTestScannerInterface * get_ble_test_scanner_instance() {
  if (btLeTestScannerInstance == nullptr)
    btLeTestScannerInstance = new BleTestScannerInterfaceImpl();

  return btLeTestScannerInstance;
}
