#pragma once
// FirmwareStore: firmware images in the raw "fwstore" flash partition (never in NVS / never in the app image).
// Layout: N slots of SLOT_SIZE (128 KiB) = 4 KiB header sector + data. An image larger than one slot's data area
// occupies slot 0 and spans into the following slot(s) (those become unusable while it is stored).
#include "esphome/core/defines.h"
#ifdef USE_ESP32
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <esp_partition.h>
#include "telink_image.h"
#include "compatibility.h"

namespace esphome {
namespace xiaomi_esp_flasher {

struct StoreHeader {
  uint32_t magic;       // 'XFW2'
  uint32_t size;        // image bytes
  uint32_t crc32;       // pvvx crc32 of the whole file (validation)
  uint32_t seq;         // write counter (oldest = smallest)
  uint8_t kind;         // ImageKind
  uint8_t span;         // number of slots occupied
  uint8_t reserved[2];
  uint64_t hw_ids;      // bitmap of pvvx hw ids 0..63 the image is for
  char name[40];
  char version[16];
  char source[64];
} __attribute__((packed));

struct StoredImage {
  uint8_t slot;
  StoreHeader hdr;
};

class FirmwareStore {
 public:
  static constexpr size_t SLOT_SIZE = 0x20000;   // 128 KiB
  static constexpr size_t DATA_OFFSET = 0x1000;  // header sector
  static constexpr uint32_t MAGIC = 0x32574658;  // 'XFW2'
  static constexpr uint8_t MAX_SLOTS = 8;

  bool init();  // locate the partition and read the headers
  bool available() const { return this->partition_ != nullptr; }
  uint8_t slot_count() const { return this->slots_; }
  size_t capacity() const { return this->partition_ ? this->partition_->size - DATA_OFFSET : 0; }
  size_t slot_capacity() const { return SLOT_SIZE - DATA_OFFSET; }
  const std::vector<StoredImage> &images() const { return this->images_; }
  bool has_image() const { return !this->images_.empty(); }
  bool find(const std::string &name, StoredImage &out) const;
  bool get_info(const StoredImage &img, FirmwareInfo &out) const;
  bool erase_image(const std::string &name);

  // streaming write (called from the HTTP task). The slot is chosen here: same name -> replace, else a free
  // slot, else the oldest image; images bigger than a slot take slot 0 (+following) and evict what is there.
  bool begin_write(size_t total, const std::string &name, std::string &err);
  bool write_chunk(const uint8_t *data, size_t len, std::string &err);
  bool finish_write(const std::string &name, const std::string &version, ImageKind kind, uint64_t hw_ids,
                    const std::string &source, std::string &err);  // validates the Telink image
  void abort_write();

  ImageReader reader(uint8_t slot) const;
  bool read(uint8_t slot, size_t offset, uint8_t *buf, size_t len) const;

 protected:
  bool flush_();
  void rescan_();
  const esp_partition_t *partition_{nullptr};
  uint8_t slots_{0};
  std::vector<StoredImage> images_;
  bool writing_{false};
  uint8_t write_slot_{0};
  uint8_t write_span_{1};
  size_t write_total_{0};
  size_t write_pos_{0};
  uint32_t next_seq_{1};
  uint8_t buf_[4096];
  size_t buf_len_{0};
};

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
