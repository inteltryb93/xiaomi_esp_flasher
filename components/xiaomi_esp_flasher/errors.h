#pragma once
#include <cstdint>

namespace esphome {
namespace xiaomi_esp_flasher {

// Error codes exposed verbatim by the HTTP API, the OTA log and the Home Assistant status sensors.
enum class Error : uint8_t {
  NONE = 0,
  BLE_TIMEOUT,
  DEVICE_NOT_FOUND,
  DEVICE_UNSUPPORTED,
  HW_UNKNOWN,
  FW_UNKNOWN,
  ACTIVATION_FAILED,
  ACTIVATION_REQUIRED,
  INCOMPATIBLE_FIRMWARE,
  OTA_WRITE_FAILED,
  OTA_VERIFY_FAILED,
  OTA_DEVICE_ERROR,
  DEVICE_DISCONNECTED,
  NOT_ENOUGH_MEMORY,
  NOT_ENOUGH_STORAGE,
  INVALID_IMAGE,
  BUSY,
  INVALID_REQUEST,
  CONFIG_MISMATCH,
  DOWNLOAD_FAILED,
  INTERNAL_ERROR,
};

inline const char *error_to_string(Error e) {
  switch (e) {
    case Error::NONE: return "NONE";
    case Error::BLE_TIMEOUT: return "BLE_TIMEOUT";
    case Error::DEVICE_NOT_FOUND: return "DEVICE_NOT_FOUND";
    case Error::DEVICE_UNSUPPORTED: return "DEVICE_UNSUPPORTED";
    case Error::HW_UNKNOWN: return "HW_UNKNOWN";
    case Error::FW_UNKNOWN: return "FW_UNKNOWN";
    case Error::ACTIVATION_FAILED: return "ACTIVATION_FAILED";
    case Error::ACTIVATION_REQUIRED: return "ACTIVATION_REQUIRED";
    case Error::INCOMPATIBLE_FIRMWARE: return "INCOMPATIBLE_FIRMWARE";
    case Error::OTA_WRITE_FAILED: return "OTA_WRITE_FAILED";
    case Error::OTA_VERIFY_FAILED: return "OTA_VERIFY_FAILED";
    case Error::OTA_DEVICE_ERROR: return "OTA_DEVICE_ERROR";
    case Error::DEVICE_DISCONNECTED: return "DEVICE_DISCONNECTED";
    case Error::NOT_ENOUGH_MEMORY: return "NOT_ENOUGH_MEMORY";
    case Error::NOT_ENOUGH_STORAGE: return "NOT_ENOUGH_STORAGE";
    case Error::INVALID_IMAGE: return "INVALID_IMAGE";
    case Error::BUSY: return "BUSY";
    case Error::INVALID_REQUEST: return "INVALID_REQUEST";
    case Error::CONFIG_MISMATCH: return "CONFIG_MISMATCH";
    case Error::DOWNLOAD_FAILED: return "DOWNLOAD_FAILED";
    case Error::INTERNAL_ERROR: return "INTERNAL_ERROR";
  }
  return "UNKNOWN";
}

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
