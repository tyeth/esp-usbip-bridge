#ifndef DEVICE_NAMING_H
#define DEVICE_NAMING_H

#include "esp_err.h"

/**
 * @brief Maximum length of a friendly hostname / device name
 */
#define DEVICE_NAME_MAX_LEN 64

/**
 * @brief NVS namespace for storing device naming config
 */
#define DEVICE_NVS_NAMESPACE "device_names"

/**
 * @brief NVS key for the bridge's friendly hostname
 */
#define DEVICE_NVS_KEY_HOSTNAME "hostname"

/**
 * @brief NVS key for the DUT board ID
 */
#define DEVICE_NVS_KEY_BOARD_ID "board_id"

/**
 * @brief NVS key prefix for per-device friendly names (key = "name_<busid>")
 *        The busid is stored as a hex-encoded suffix to avoid characters that
 *        NVS keys cannot contain (like '-').
 */

/**
 * @brief Get the friendly hostname for this bridge.
 *
 * Reads from NVS.  If no friendly name is stored, returns the default
 * hostname "usbip-<chip>-<last 3 MAC bytes>" (e.g. "usbip-s31-d19bee",
 * "usbip-p4v3-0a1b2c") so the caller always gets a valid hostname.  A
 * stored name equal to the old default ("usbip-<last 3 MAC bytes>") is
 * cleared from NVS on first read and counts as unset.
 *
 * @param[out] buf       Buffer to write the hostname into.
 * @param[in]  buf_size  Size of the buffer (at least DEVICE_NAME_MAX_LEN).
 * @return ESP_OK on success.
 */
esp_err_t device_naming_get_hostname(char *buf, size_t buf_size);

/**
 * @brief Set the friendly hostname for this bridge.
 *
 * Stores in NVS and commits.  Passing an empty string clears the friendly
 * name, causing get_hostname() to fall back to the MAC-based default.
 *
 * @param[in] hostname  Friendly hostname (up to DEVICE_NAME_MAX_LEN - 1 chars).
 *                       Empty string to clear.
 * @return ESP_OK on success.
 */
esp_err_t device_naming_set_hostname(const char *hostname);

/**
 * @brief Get the board ID for the device under test.
 *
 * Reads from NVS.  Returns an empty string if unset.
 *
 * @param[out] buf       Buffer to write the board ID into.
 * @param[in]  buf_size  Size of the buffer (at least DEVICE_NAME_MAX_LEN).
 * @return ESP_OK on success.
 */
esp_err_t device_naming_get_board_id(char *buf, size_t buf_size);

/**
 * @brief Set the board ID for the device under test.
 *
 * Stores in NVS and commits.  Passing an empty string clears it.
 *
 * @param[in] board_id  Board ID string (up to DEVICE_NAME_MAX_LEN - 1 chars).
 *                       Empty string to clear.
 * @return ESP_OK on success.
 */
esp_err_t device_naming_set_board_id(const char *board_id);

/**
 * @brief Get the friendly name for a specific USB device (by busid).
 *
 * @param[in]  busid     USB device busid string (e.g. "1-3").
 * @param[out] buf       Buffer to write the friendly name into.
 * @param[in]  buf_size  Size of the buffer (at least DEVICE_NAME_MAX_LEN).
 * @return ESP_OK if a name was found.  ESP_ERR_NVS_NOT_FOUND if unset.
 */
esp_err_t device_naming_get_device_name(const char *busid, char *buf, size_t buf_size);

/**
 * @brief Set the friendly name for a specific USB device (by busid).
 *
 * @param[in] busid  USB device busid string (e.g. "1-3").
 * @param[in] name   Friendly name (up to DEVICE_NAME_MAX_LEN - 1 chars).
 *                    Empty string to clear.
 * @return ESP_OK on success.
 */
esp_err_t device_naming_set_device_name(const char *busid, const char *name);

/**
 * @brief Initialise the device naming subsystem.
 *
 * Called after nvs_flash_init().  Currently a no-op, but provides a
 * hook for future init work.
 *
 * @return ESP_OK.
 */
esp_err_t device_naming_init(void);

#endif
