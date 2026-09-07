#pragma once
// Telink OTA image validation + CRC helpers.
// Port of testOTAFirmware(), crc16_modbus() and crc32() from TelinkMiFlasher.html (see docs/telink_ota_protocol.md §5).
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

namespace esphome {
namespace xiaomi_esp_flasher {

// Modbus CRC-16 (init 0xFFFF, reflected poly 0xA001) – used for every OTA data packet.
uint16_t crc16_modbus(const uint8_t *data, size_t len, uint16_t crc = 0xFFFF);

// pvvx "crc32": table CRC-32 (poly 0xEDB88320), init 0xFFFFFFFF, NO final inversion.
uint32_t crc32_telink_update(uint32_t crc, const uint8_t *data, size_t len);
inline uint32_t crc32_telink_init() { return 0xFFFFFFFFu; }

struct TelinkImageInfo {
  size_t file_size{0};
  uint32_t size_in_header{0};
  uint32_t crc_stored{0};
  uint32_t crc_calc{0};
  bool signed_original{false};  // extra signature block after the image ("sign" in pvvx)
  bool zigbee_container{false};
};

// Reads the image through `reader(offset, buf, len)` so it works for RAM buffers and flash partitions.
// Returns "ok" or an error string identical to the pvvx JavaScript messages.
using ImageReader = std::function<bool(size_t offset, uint8_t *buf, size_t len)>;
std::string validate_telink_image(size_t file_size, const ImageReader &reader, TelinkImageInfo &info,
                                  size_t max_size);

constexpr size_t MAX_BLE_OTA_SIZE = 0x20000;  // 128 KiB (TelinkMiFlasher.html)
constexpr size_t MAX_EXT_OTA_SIZE = 0x34000;  // 208 KiB

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
