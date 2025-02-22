// Copyright 2015-2017 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _ESP_SPIFFS_LOBO_H_
#define _ESP_SPIFFS_LOBO_H_

#include <stdbool.h>
#include "esp_err.h"
#include "esp_spiffs.h"
#include "spiffs.h"
#include "spiffs_nucleus.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register and mount SPIFFS to VFS with given path prefix.
 *
 * @param   conf                      Pointer to esp_vfs_spiffs_conf_t configuration structure
 *
 * @return  
 *          - ESP_OK                  if success
 *          - ESP_ERR_NO_MEM          if objects could not be allocated
 *          - ESP_ERR_INVALID_STATE   if already mounted or partition is encrypted
 *          - ESP_ERR_NOT_FOUND       if partition for SPIFFS was not found
 *          - ESP_FAIL                if mount or format fails
 */
esp_err_t esp_vfs_spiffs_register_lobo(const esp_vfs_spiffs_conf_t * conf);

/**
 * Unregister and unmount SPIFFS from VFS
 *
 * @param partition_label  Optional, label of the partition to unregister.
 *                         If not specified, first partition with subtype=spiffs is used.
 *
 * @return  
 *          - ESP_OK if successful
 *          - ESP_ERR_INVALID_STATE already unregistered
 */
esp_err_t esp_vfs_spiffs_unregister_lobo(const char* partition_label);

/**
 * Check if SPIFFS is mounted
 *
 * @param partition_label  Optional, label of the partition to check.
 *                         If not specified, first partition with subtype=spiffs is used.
 *
 * @return  
 *          - true    if mounted
 *          - false   if not mounted
 */
bool esp_spiffs_mounted_lobo(const char* partition_label);

/**
 * Format the SPIFFS partition
 *
 * @param partition_label  Optional, label of the partition to format.
 *                         If not specified, first partition with subtype=spiffs is used.
 * @return  
 *          - ESP_OK      if successful
 *          - ESP_FAIL    on error
 */
esp_err_t esp_spiffs_format_lobo(const char* partition_label);

/**
 * Get information for SPIFFS
 *
 * @param partition_label           Optional, label of the partition to get info for.
 *                                  If not specified, first partition with subtype=spiffs is used.
 * @param[out] total_bytes          Size of the file system
 * @param[out] used_bytes           Current used bytes in the file system
 *
 * @return  
 *          - ESP_OK                  if success
 *          - ESP_ERR_INVALID_STATE   if not mounted
 */
esp_err_t esp_spiffs_info_lobo(const char* partition_label, size_t *total_bytes, size_t *used_bytes);

#ifdef __cplusplus
}
#endif

#endif /* _ESP_SPIFFS_H_ */
