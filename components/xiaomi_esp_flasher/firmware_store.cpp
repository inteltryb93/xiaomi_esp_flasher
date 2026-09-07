#include "firmware_store.h"
#ifdef USE_ESP32
#include "esphome/core/log.h"
#include <cstring>

namespace esphome {
namespace xiaomi_esp_flasher {

static const char *const TAG = "xiaomi_flasher.store";

bool FirmwareStore::init() {
  this->partition_ = esp_partition_find_first((esp_partition_type_t) 0x40, (esp_partition_subtype_t) 0x00, "fwstore");
  if (this->partition_ == nullptr) {
    ESP_LOGE(TAG, "partition 'fwstore' not found – uploads disabled");
    return false;
  }
  ESP_LOGI(TAG, "fwstore partition at 0x%06X, %u bytes", (unsigned) this->partition_->address, (unsigned) this->partition_->size);
  if (esp_partition_read(this->partition_, 0, &this->hdr_, sizeof(this->hdr_)) != ESP_OK)
    return false;
  this->valid_ = this->hdr_.magic == MAGIC && this->hdr_.size > 1024 && this->hdr_.size <= this->capacity();
  if (this->valid_) {
    // re-validate the image so a half-written store is never offered
    TelinkImageInfo info;
    std::string r = validate_telink_image(this->hdr_.size, this->reader(), info, MAX_EXT_OTA_SIZE);
    if (r != "ok" || info.crc_stored != this->hdr_.crc32) {
      ESP_LOGW(TAG, "stored image invalid (%s) – ignoring", r.c_str());
      this->valid_ = false;
    } else {
      this->hdr_.name[sizeof(this->hdr_.name) - 1] = 0;
      this->hdr_.version[sizeof(this->hdr_.version) - 1] = 0;
      this->hdr_.source[sizeof(this->hdr_.source) - 1] = 0;
      ESP_LOGI(TAG, "stored image: %s v%s %u bytes", this->hdr_.name, this->hdr_.version, (unsigned) this->hdr_.size);
    }
  }
  return true;
}

bool FirmwareStore::get_info(FirmwareInfo &out) const {
  if (!this->valid_)
    return false;
  out = FirmwareInfo{};
  out.id = std::string("store:") + this->hdr_.name;
  out.name = this->hdr_.name;
  out.version = this->hdr_.version;
  out.kind = (ImageKind) this->hdr_.kind;
  out.size = this->hdr_.size;
  out.crc32 = this->hdr_.crc32;
  out.source = this->hdr_.source;
  for (int i = 0; i < 64; i++)
    if (this->hdr_.hw_ids & (1ULL << i))
      out.hw_ids.push_back(i);
  return true;
}

bool FirmwareStore::erase() {
  if (this->partition_ == nullptr)
    return false;
  this->valid_ = false;
  return esp_partition_erase_range(this->partition_, 0, this->partition_->size) == ESP_OK;
}

bool FirmwareStore::begin_write(size_t total, std::string &err) {
  if (this->partition_ == nullptr) { err = "NOT_ENOUGH_STORAGE: no fwstore partition"; return false; }
  if (total < 1024 || total > this->capacity()) { err = "NOT_ENOUGH_STORAGE: image size " + std::to_string(total) + " exceeds " + std::to_string(this->capacity()); return false; }
  this->valid_ = false;
  // erase header + enough data sectors
  size_t need = DATA_OFFSET + ((total + 4095) & ~4095);
  if (esp_partition_erase_range(this->partition_, 0, need) != ESP_OK) { err = "INTERNAL_ERROR: erase failed"; return false; }
  this->writing_ = true;
  this->write_total_ = total;
  this->write_pos_ = 0;
  this->buf_len_ = 0;
  return true;
}

bool FirmwareStore::flush_() {
  if (this->buf_len_ == 0)
    return true;
  size_t len = (this->buf_len_ + 3) & ~3;  // 4-byte aligned writes
  if (len > this->buf_len_)
    memset(this->buf_ + this->buf_len_, 0xFF, len - this->buf_len_);
  esp_err_t e = esp_partition_write(this->partition_, DATA_OFFSET + this->write_pos_, this->buf_, len);
  this->write_pos_ += this->buf_len_;
  this->buf_len_ = 0;
  return e == ESP_OK;
}

bool FirmwareStore::write_chunk(const uint8_t *data, size_t len, std::string &err) {
  if (!this->writing_) { err = "INVALID_REQUEST: not writing"; return false; }
  if (this->write_pos_ + this->buf_len_ + len > this->write_total_) { err = "INVALID_REQUEST: more data than announced"; return false; }
  while (len > 0) {
    size_t room = sizeof(this->buf_) - this->buf_len_;
    size_t n = len < room ? len : room;
    memcpy(this->buf_ + this->buf_len_, data, n);
    this->buf_len_ += n;
    data += n;
    len -= n;
    if (this->buf_len_ == sizeof(this->buf_) && !this->flush_()) { err = "INTERNAL_ERROR: flash write failed"; this->writing_ = false; return false; }
  }
  return true;
}

void FirmwareStore::abort_write() {
  this->writing_ = false;
  this->buf_len_ = 0;
}

bool FirmwareStore::finish_write(const std::string &name, const std::string &version, ImageKind kind, uint64_t hw_ids,
                                 const std::string &source, std::string &err) {
  if (!this->writing_) { err = "INVALID_REQUEST: not writing"; return false; }
  this->writing_ = false;
  if (!this->flush_()) { err = "INTERNAL_ERROR: flash write failed"; return false; }
  if (this->write_pos_ != this->write_total_) { err = "INVALID_REQUEST: short upload (" + std::to_string(this->write_pos_) + "/" + std::to_string(this->write_total_) + ")"; return false; }
  TelinkImageInfo info;
  std::string r = validate_telink_image(this->write_total_, this->reader(), info, MAX_EXT_OTA_SIZE);
  if (r != "ok" && r != "sign") { err = "INVALID_IMAGE: " + r; return false; }
  StoreHeader h{};
  h.magic = MAGIC;
  h.size = this->write_total_;
  h.crc32 = info.crc_stored;
  h.kind = (uint8_t) kind;
  h.hw_ids = hw_ids;
  strncpy(h.name, name.c_str(), sizeof(h.name) - 1);
  strncpy(h.version, version.c_str(), sizeof(h.version) - 1);
  strncpy(h.source, source.c_str(), sizeof(h.source) - 1);
  if (esp_partition_write(this->partition_, 0, &h, sizeof(h)) != ESP_OK) { err = "INTERNAL_ERROR: header write failed"; return false; }
  this->hdr_ = h;
  this->valid_ = true;
  ESP_LOGI(TAG, "stored %s v%s (%u bytes, crc 0x%08X)", h.name, h.version, (unsigned) h.size, (unsigned) h.crc32);
  return true;
}

bool FirmwareStore::read(size_t offset, uint8_t *buf, size_t len) const {
  if (this->partition_ == nullptr)
    return false;
  return esp_partition_read(this->partition_, DATA_OFFSET + offset, buf, len) == ESP_OK;
}

ImageReader FirmwareStore::reader() const {
  return [this](size_t off, uint8_t *buf, size_t len) { return this->read(off, buf, len); };
}

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
