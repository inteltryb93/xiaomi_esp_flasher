#pragma once
// Compatibility matrix: the single authority deciding whether a firmware image may be flashed to a device.
// See docs/compatibility.md for the rule set and its sources.
#include <cstdint>
#include <string>
#include <vector>
#include "errors.h"

namespace esphome {
namespace xiaomi_esp_flasher {

constexpr int HW_UNKNOWN = -1;

enum class FirmwareKind : uint8_t { UNKNOWN = 0, CUSTOM_PVVX, STOCK_XIAOMI, ATC1441, OTHER_TELINK };
const char *firmware_kind_name(FirmwareKind k);

enum class ImageKind : uint8_t { CUSTOM = 0, BETA, ORIGINAL, SIGNED, USER_UPLOAD };
const char *image_kind_name(ImageKind k);

struct HardwareInfo {
  std::string model;      // "LYWSD03MMC", "MHO-C401", ...
  int hw_id{HW_UNKNOWN};  // pvvx numeric hardware id (bin/README.md)
  std::string hw_string;  // DIS 0x2A27 e.g. "B1.7"
  FirmwareKind kind{FirmwareKind::UNKNOWN};
  std::string fw_version;   // "4.7" (custom, from cfg ver) or "1.0.0_0130" (stock DIS)
  uint8_t fw_ver_byte{0};   // custom BCD version byte, 0 if unknown
  uint8_t i2c_sensor{0};    // CMD_ID_GDEVS (addr<<1), 0 = unknown
  uint8_t i2c_lcd{0};
  bool big_ota{false};      // 208 KiB ext OTA supported
  bool requires_cloud_token{false};  // stock fw 2.1.1_0159 etc.
};

struct FirmwareInfo {
  std::string id;          // "custom", "beta", "original", "upload:<name>"
  std::string name;        // file name e.g. ATC_v59.bin
  std::string version;     // "5.9" or "1.0.0_0130"
  int version_num{0};      // comparable number: custom BCD byte (0x59 = 89); 0 = not comparable
  ImageKind kind{ImageKind::CUSTOM};
  std::vector<int> hw_ids; // hardware ids from firmware.json index positions
  size_t size{0};
  uint32_t crc32{0};
  std::string source;      // url or "bundled"/"upload"
  bool for_all_lywsd03mmc{false};
};

struct CompatResult {
  bool ok{false};
  Error code{Error::NONE};
  std::string message;
  std::vector<std::string> warnings;
};

// LYWSD03MMC hardware table helpers
int lywsd03mmc_hw_id_from_string(const std::string &hw);  // "B1.4" -> 0, ... , unknown -> HW_UNKNOWN
const char *hw_id_name(int hw_id);                         // pvvx hw_version_str table
const char *lywsd03mmc_hw_string_from_id(int hw_id);       // 0 -> "B1.4"
bool is_lywsd03mmc_id(int hw_id);
int lywsd03mmc_class_from_i2c(uint8_t sensor_addr, uint8_t lcd_addr);  // set_hw_version() table

// Semantic version compare for "update available": returns <0, 0, >0. Handles "4.7" vs "5.9", "1.10" vs "1.9",
// and stock strings like "1.0.0_0130".
int compare_versions(const std::string &a, const std::string &b);

CompatResult check_compatibility(const HardwareInfo &hw, const FirmwareInfo &fw);
bool is_firmware_compatible(const HardwareInfo &hw, const FirmwareInfo &fw);

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
