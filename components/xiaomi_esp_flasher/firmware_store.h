#pragma once
// FirmwareStore: one staging slot for a firmware image (uploaded via HTTP or downloaded from GitHub)
// in the raw "fwstore" flash partition.  Never stored in NVS/preferences.
#include "esphome/core/defines.h"
#ifdef USE_ESP32
#include <cstdint>
#include <string>
#include <functional>
#include <esp_partition.h>
#include "telink_image.h"
#include "compatibility.h"

namespace esphome {
namespace xiaomi_esp_flasher {

struct StoreHeader {
  uint32_t magic;       // 'XFW1'
  uint32_t size;        // image bytes
  uint32_t crc32;       // pvvx crc32 of the whole file (validation)
  uint8_t kind;         // ImageKind
  uint8_t reserved[3];
  uint64_t hw_ids;      // bitmap of pvvx hw ids 0..63 the image is for
  char name[40];
  char version[16];
  char source[64];
} __attribute__((packed));

class FirmwareStore {
 public:
  bool init();  // locate the partition and read the header
  bool available() const { return this->partition_ != nullptr; }
  bool has_image() const { return this->valid_; }
  size_t capacity() const { return this->partition_ ? this->partition_->size - DATA_OFFSET : 0; }
  const StoreHeader &header() const { return this->hdr_; }
  bool get_info(FirmwareInfo &out) const;

  // streaming write (called from the HTTP task)
  bool begin_write(size_t total, std::string &err);
  bool write_chunk(const uint8_t *data, size_t len, std::string &err);
  bool finish_write(const std::string &name, const std::string &version, ImageKind kind, uint64_t hw_ids,
                    const std::string &source, std::string &err);  // validates the Telink image
  void abort_write();
  bool erase();

  ImageReader reader() const;
  bool read(size_t offset, uint8_t *buf, size_t len) const;

  static constexpr size_t DATA_OFFSET = 0x1000;
  static constexpr uint32_t MAGIC = 0x31574658;  // 'XFW1' little endian

 protected:
  bool flush_();
  const esp_partition_t *partition_{nullptr};
  StoreHeader hdr_{};
  bool valid_{false};
  bool writing_{false};
  size_t write_total_{0};
  size_t write_pos_{0};
  uint8_t buf_[4096];
  size_t buf_len_{0};
};

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
