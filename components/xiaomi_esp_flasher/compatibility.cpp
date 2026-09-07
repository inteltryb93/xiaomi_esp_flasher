#include "compatibility.h"
#include "telink_image.h"
#include <cstdlib>
#include <cstring>

namespace esphome {
namespace xiaomi_esp_flasher {

const char *firmware_kind_name(FirmwareKind k) {
  switch (k) {
    case FirmwareKind::CUSTOM_PVVX: return "pvvx custom";
    case FirmwareKind::STOCK_XIAOMI: return "Xiaomi original";
    case FirmwareKind::ATC1441: return "atc1441";
    case FirmwareKind::OTHER_TELINK: return "Telink (unknown)";
    default: return "unknown";
  }
}
const char *image_kind_name(ImageKind k) {
  switch (k) {
    case ImageKind::CUSTOM: return "custom";
    case ImageKind::BETA: return "beta";
    case ImageKind::ORIGINAL: return "original";
    case ImageKind::SIGNED: return "signed";
    default: return "upload";
  }
}

// TelinkMiFlasher.html const hw_version_str (index == pvvx hw id); LYWSD03MMC entries annotated with revision
static const char *const HW_NAMES[] = {
    "LYWSD03MMC B1.4", "MHO-C401(old)", "CGG1-M(2020,2021)", "LYWSD03MMC B1.9", "LYWSD03MMC B1.6",
    "LYWSD03MMC B1.7/B2.0", "CGDK2", "CGG1-M(2022)", "MHO-C401(2022)", "MJWSD05MMC(ch)",
    "LYWSD03MMC B1.5", "MHO-C122", "MJWSD05MMC(en)", "MJWSD06MMC", "LYWSD03MMC B1.1 (new B1.6)", "ID15",
    "TB03F", "TS0201", "TNKS", "THB2", "BTH01", "TH05", "TH03Z", "THB1", "TH05D", "TH05F", "THB3",
    "ZTH01", "ZTH02", "PLM1", "TH03(DIY)", "LKTMZL02", "KEY2", "ZTH05", "TH04", "CB3S", "HS09",
    "ZY-ZTH02", "ZY-ZTH02/03-Pro", "ZG-227Z", "TS0202_PIR1", "TS0202_PIR2", "HDP16", "TN_6ATAG3",
    "ZG-303Z", "ZBeacon-TH01", "ZBeaconMC", "ZBeaconMC2", "RSH_HS03", "LYWSD02MMC", "ZG204ZL",
    "ZG204ZV", "TS0201_WING", "DIY-SCD41"};

const char *hw_id_name(int hw_id) {
  if (hw_id < 0 || hw_id >= (int) (sizeof(HW_NAMES) / sizeof(HW_NAMES[0])))
    return "Unknown";
  return HW_NAMES[hw_id];
}

// bin/README.md + app.c set_hw_version(): id2hwver = {'4','0','5','9','6','7','1','0'}
int lywsd03mmc_hw_id_from_string(const std::string &hw) {
  std::string s = hw.substr(0, 4);
  if (s == "B1.4" || s == "0000") return 0;
  if (s == "B1.5") return 10;
  if (s == "B1.6") return 4;
  if (s == "B1.7" || s == "B2.0") return 5;
  if (s == "B1.9") return 3;
  if (s == "B1.1") return 14;
  return HW_UNKNOWN;
}
const char *lywsd03mmc_hw_string_from_id(int id) {
  switch (id) {
    case 0: return "B1.4";
    case 10: return "B1.5";
    case 4: return "B1.6";
    case 5: return "B1.7/B2.0";
    case 3: return "B1.9";
    case 14: return "B1.1";
    default: return "?";
  }
}
bool is_lywsd03mmc_id(int id) { return id == 0 || id == 3 || id == 4 || id == 5 || id == 10 || id == 14; }

int lywsd03mmc_class_from_i2c(uint8_t sensor, uint8_t lcd) {
  // addresses are reported shifted (addr<<1) by CMD_ID_GDEVS: SHTC3 0x70->0xE0, SHT4x 0x44->0x88, LCD 0x3C->0x78, 0x3E->0x7C
  bool shtc3 = (sensor >> 1) == 0x70;
  bool sht4x = (sensor >> 1) == 0x44 || (sensor >> 1) == 0x45 || (sensor >> 1) == 0x46;
  uint8_t l = lcd >> 1;
  if (l == 0x3C) return shtc3 ? 0 : (sht4x ? 5 : HW_UNKNOWN);
  if (l == 0x3E) return sht4x ? 3 : HW_UNKNOWN;
  if (lcd == 0) return shtc3 ? 10 : (sht4x ? 4 : HW_UNKNOWN);  // UART/SPI (no i2c lcd)
  return HW_UNKNOWN;
}

static void split_numbers(const std::string &s, std::vector<long> &out) {
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && !isdigit((unsigned char) s[i])) i++;
    if (i >= s.size()) break;
    char *end;
    long v = strtol(s.c_str() + i, &end, 10);
    out.push_back(v);
    i = end - s.c_str();
  }
}

int compare_versions(const std::string &a, const std::string &b) {
  std::vector<long> va, vb;
  split_numbers(a, va);
  split_numbers(b, vb);
  size_t n = va.size() > vb.size() ? va.size() : vb.size();
  for (size_t i = 0; i < n; i++) {
    long x = i < va.size() ? va[i] : 0;
    long y = i < vb.size() ? vb[i] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

CompatResult check_compatibility(const HardwareInfo &hw, const FirmwareInfo &fw) {
  CompatResult r;
  // Rule 2: hardware must be known
  if (hw.hw_id == HW_UNKNOWN) {
    r.code = Error::HW_UNKNOWN;
    r.message = "Unknown / unsupported hardware revision. Firmware update blocked for safety.";
    return r;
  }
  if (hw.kind == FirmwareKind::UNKNOWN) {
    r.code = Error::FW_UNKNOWN;
    r.message = "Installed firmware could not be identified. Update blocked.";
    return r;
  }
  // Rule 5: stock firmware needing Mi cloud registration
  if (hw.requires_cloud_token) {
    r.code = Error::ACTIVATION_REQUIRED;
    r.message = "Installed original firmware " + hw.fw_version +
                " requires Mi-Home registration (token) before OTA. Update blocked.";
    return r;
  }
  // Rule 1: the image must list this hardware id
  bool listed = false;
  for (int id : fw.hw_ids)
    if (id == hw.hw_id) listed = true;
  if (!listed && fw.for_all_lywsd03mmc && is_lywsd03mmc_id(hw.hw_id))
    listed = true;
  if (!listed) {
    r.code = Error::INCOMPATIBLE_FIRMWARE;
    r.message = "Firmware " + fw.name + " is not built for hardware id " + std::to_string(hw.hw_id) + " (" +
                hw_id_name(hw.hw_id) + ").";
    return r;
  }
  // Rule 3: I2C topology cross-check (custom fw only) – warn on mismatch of the class inside the LYWSD03MMC family
  if (hw.kind == FirmwareKind::CUSTOM_PVVX && is_lywsd03mmc_id(hw.hw_id) && (hw.i2c_sensor || hw.i2c_lcd)) {
    int cls = lywsd03mmc_class_from_i2c(hw.i2c_sensor, hw.i2c_lcd);
    if (cls == HW_UNKNOWN) {
      r.code = Error::HW_UNKNOWN;
      r.message = "I2C topology (sensor 0x" + std::to_string(hw.i2c_sensor >> 1) + ", lcd 0x" +
                  std::to_string(hw.i2c_lcd >> 1) + ") does not match any LYWSD03MMC revision. Update blocked.";
      return r;
    }
    if (cls != hw.hw_id)
      r.warnings.push_back(std::string("pvvx hw id ") + std::to_string(hw.hw_id) + " (" +
                           lywsd03mmc_hw_string_from_id(hw.hw_id) + ") differs from I2C-detected class " +
                           lywsd03mmc_hw_string_from_id(cls) + "; both use the same ATC image.");
  }
  if (!hw.hw_string.empty() && is_lywsd03mmc_id(hw.hw_id)) {
    int from_str = lywsd03mmc_hw_id_from_string(hw.hw_string);
    if (from_str != HW_UNKNOWN && from_str != hw.hw_id)
      r.warnings.push_back("DIS hardware string " + hw.hw_string + " maps to id " + std::to_string(from_str) +
                           " while the firmware reports id " + std::to_string(hw.hw_id) + ".");
  }
  // Rule 4: size limit
  size_t limit = hw.big_ota ? MAX_EXT_OTA_SIZE : MAX_BLE_OTA_SIZE;
  if (fw.size == 0 || fw.size > limit) {
    r.code = Error::INCOMPATIBLE_FIRMWARE;
    r.message = "Image size " + std::to_string(fw.size) + " exceeds the device OTA limit of " +
                std::to_string(limit) + " bytes.";
    return r;
  }
  // Rule 6: original images are a deliberate conversion, flagged but allowed only for stock-capable ids
  if (fw.kind == ImageKind::ORIGINAL || fw.kind == ImageKind::SIGNED) {
    if (hw.hw_id == 14) {
      r.code = Error::INCOMPATIBLE_FIRMWARE;
      r.message = "Original firmware for hw id 14 (new B1.6) requires Mi-Home registration afterwards; blocked.";
      return r;
    }
    r.warnings.push_back("Flashing an ORIGINAL image erases custom settings; re-flashing custom firmware will require Mi activation.");
  }
  if (hw.kind == FirmwareKind::STOCK_XIAOMI)
    r.warnings.push_back("Device runs original firmware: Mi activation will be performed before OTA.");
  r.ok = true;
  r.code = Error::NONE;
  r.message = "Compatibility check: PASSED";
  return r;
}

bool is_firmware_compatible(const HardwareInfo &hw, const FirmwareInfo &fw) { return check_compatibility(hw, fw).ok; }

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
