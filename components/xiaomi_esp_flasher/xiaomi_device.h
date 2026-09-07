#pragma once
// XiaomiDevice: everything known about one thermometer – identity, hardware, firmware, configuration,
// live measurements (from advertising or connection) and update eligibility.
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "compatibility.h"
#include "device_config.h"
#include "errors.h"

namespace esphome {
namespace xiaomi_esp_flasher {

enum class DeviceStatus : uint8_t {
  DISCOVERED = 0,  // seen in advertising only
  IDENTIFYING,
  IDENTIFIED,
  CONNECTING,
  CONNECTED,
  UPDATING,
  UPDATED,
  FAILED,
  OFFLINE,
};
const char *device_status_name(DeviceStatus s);

enum class AdvFormat : uint8_t { NONE = 0, BTHOME_V2, PVVX_CUSTOM, ATC1441, MIJIA, BTHOME_V1, UNKNOWN };
const char *adv_format_name(AdvFormat f);
std::string json_escape(const std::string &s);

struct AdvMeasurement {
  bool valid{false};
  float temperature{NAN};
  float humidity{NAN};
  float battery_pct{NAN};
  float battery_mv{NAN};
  uint8_t counter{0};
  AdvFormat format{AdvFormat::NONE};
  bool encrypted{false};
};

// Parses the three pvvx custom formats + Mi + BTHome.  Returns true when something was decoded.
bool parse_advertisement(uint16_t uuid16, const uint8_t *data, size_t len, AdvMeasurement &out, std::string &model_hint,
                         std::string &mi_fw_hint);

struct XiaomiDevice {
  uint64_t address{0};
  char mac[18]{};
  uint8_t addr_type{0};
  std::string name;   // advertised BLE name
  std::string alias;  // user-defined
  int rssi{0};
  uint32_t last_seen_ms{0};
  uint32_t last_seen_epoch{0};
  DeviceStatus status{DeviceStatus::DISCOVERED};
  AdvMeasurement adv;
  std::string model_hint;   // from advertising (LYWSD03MMC via Mi id / name)
  std::string mi_fw_hint;

  // identification (after GATT connection)
  bool identified{false};
  uint32_t identified_epoch{0};
  HardwareInfo hw;
  std::vector<std::string> services;
  std::string dis_model, dis_serial, dis_firmware, dis_hardware, dis_software, dis_manufacturer;
  DevId dev_id;
  DeviceConfig cfg;
  ComfortConfig comfort;
  SensorConfig sensor_cfg;
  TriggerConfig trigger;
  Measurement live;  // last measurement received in connected mode
  std::string device_name;
  uint32_t device_time{0};
  uint32_t device_time_set{0};
  std::string mac_from_device;
  bool pin_required{false};

  // update eligibility (recomputed by the hub whenever hw or the firmware list changes)
  bool update_available{false};
  std::string latest_version;
  std::string latest_firmware_id;
  CompatResult compat;
  Error last_error{Error::NONE};
  std::string last_error_message;
  std::string ota_result;  // "success 4.7 -> 5.9 @ epoch" etc.
  uint32_t last_ota_epoch{0};
  bool user_added{false};

  const std::string &display_name() const { return this->alias.empty() ? (this->name.empty() ? this->dis_model : this->name) : this->alias; }
  std::string installed_version() const;
  bool is_custom_fw() const { return this->hw.kind == FirmwareKind::CUSTOM_PVVX; }
  bool is_stock_fw() const { return this->hw.kind == FirmwareKind::STOCK_XIAOMI; }
  void set_error(Error e, const std::string &msg) { this->last_error = e; this->last_error_message = msg; }
};

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
