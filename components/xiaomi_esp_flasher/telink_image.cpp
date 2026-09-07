#include "telink_image.h"
#include <cstring>

namespace esphome {
namespace xiaomi_esp_flasher {

uint16_t crc16_modbus(const uint8_t *data, size_t len, uint16_t crc) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      bool odd = crc & 1;
      crc >>= 1;
      if (odd)
        crc ^= 0xA001;
    }
  }
  return crc;
}

static uint32_t crc32_table_entry(uint32_t i) {
  uint32_t tmp = i;
  for (int k = 0; k < 8; k++)
    tmp = (tmp & 1) ? (0xEDB88320u ^ (tmp >> 1)) : (tmp >> 1);
  return tmp;
}

uint32_t crc32_telink_update(uint32_t crc, const uint8_t *data, size_t len) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++)
      table[i] = crc32_table_entry(i);
    init = true;
  }
  for (size_t i = 0; i < len; i++)
    crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
  return crc;  // no final xor – matches TelinkMiFlasher.html crc32()
}

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

std::string validate_telink_image(size_t file_size, const ImageReader &reader, TelinkImageInfo &info,
                                  size_t max_size) {
  info = TelinkImageInfo{};
  info.file_size = file_size;
  if (file_size < 1024)
    return "Wrong binary Telink OTA firmware size!";
  uint8_t head[0x40];
  if (!reader(0, head, sizeof(head)))
    return "Read error";
  if (rd32(head) == 0x0beef11e) {
    info.zigbee_container = true;
    return "Zigbee OTA container is not supported";
  }
  if (file_size > max_size)
    return "Size firmware is more " + std::to_string(max_size) + " bytes!";
  if (rd32(head + 8) != 0x544c4e4b)  // 'KNLT'
    return "Incorrect head in Telink OTA binary firmware";
  uint32_t hsize = rd32(head + 0x18);
  info.size_in_header = hsize;
  if (hsize > file_size || (hsize & 0x0f) != 4)
    return "Invalid size pointer in Telink OTA binary firmware!";
  uint32_t crc = crc32_telink_init();
  uint8_t buf[256];
  size_t pos = 0;
  size_t end = hsize - 4;
  while (pos < end) {
    size_t n = end - pos;
    if (n > sizeof(buf))
      n = sizeof(buf);
    if (!reader(pos, buf, n))
      return "Read error";
    crc = crc32_telink_update(crc, buf, n);
    pos += n;
  }
  uint8_t crcb[4];
  if (!reader(hsize - 4, crcb, 4))
    return "Read error";
  info.crc_calc = crc;
  info.crc_stored = rd32(crcb);
  if (info.crc_calc != info.crc_stored)
    return "Incorrect CRC in Telink OTA binary firmware!";
  if (hsize < file_size && file_size - hsize > 1024) {
    uint8_t sig[12];
    static const uint8_t SIGN[12] = {0x00, 0x00, 0x00, 0x00, 0x4d, 0x49, 0xef, 0x54, 0x46, 0x4f, 0x54, 0x41};
    if (reader(hsize, sig, 12) && memcmp(sig, SIGN, 12) == 0)
      info.signed_original = true;
  }
  return "ok";
}

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
