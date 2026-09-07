#include "firmware_store.h"
#ifdef USE_ESP32
#include "esphome/core/log.h"
#include <cstring>
#include <algorithm>

namespace esphome {
namespace xiaomi_esp_flasher {

static const char *const TAG = "xiaomi_flasher.store";

bool FirmwareStore::init() {
  this->partition_ = esp_partition_find_first((esp_partition_type_t) 0x40, (esp_partition_subtype_t) 0x00, "fwstore");
  if (this->partition_ == nullptr) {
    ESP_LOGE(TAG, "partition 'fwstore' not found – uploads disabled");
    return false;
  }
  this->slots_ = std::min<size_t>(this->partition_->size / SLOT_SIZE, MAX_SLOTS);
  ESP_LOGI(TAG, "fwstore partition at 0x%06X, %u bytes, %u slots of %u KiB", (unsigned) this->partition_->address,
           (unsigned) this->partition_->size, this->slots_, (unsigned) (SLOT_SIZE / 1024));
  this->rescan_();
  return true;
}

void FirmwareStore::rescan_() {
  this->images_.clear();
  this->next_seq_ = 1;
  for (uint8_t s = 0; s < this->slots_; s++) {
    StoreHeader h;
    if (esp_partition_read(this->partition_, (size_t) s * SLOT_SIZE, &h, sizeof(h)) != ESP_OK)
      continue;
    if (h.magic != MAGIC || h.size < 1024 || h.span == 0 || s + h.span > this->slots_)
      continue;
    if (h.size > (size_t) h.span * SLOT_SIZE - DATA_OFFSET)
      continue;
    h.name[sizeof(h.name) - 1] = 0;
    h.version[sizeof(h.version) - 1] = 0;
    h.source[sizeof(h.source) - 1] = 0;
    // re-validate the image so a half-written slot is never offered
    TelinkImageInfo info;
    std::string r = validate_telink_image(h.size, this->reader(s), info, MAX_EXT_OTA_SIZE);
    if ((r != "ok" && r != "sign") || info.crc_stored != h.crc32) {
      ESP_LOGW(TAG, "slot %u: stored image invalid (%s) – ignoring", s, r.c_str());
      continue;
    }
    this->images_.push_back(StoredImage{s, h});
    if (h.seq >= this->next_seq_)
      this->next_seq_ = h.seq + 1;
    ESP_LOGI(TAG, "slot %u: %s v%s %u bytes (span %u)", s, h.name, h.version, (unsigned) h.size, h.span);
    s += h.span - 1;
  }
}

bool FirmwareStore::find(const std::string &name, StoredImage &out) const {
  for (auto &img : this->images_)
    if (name == img.hdr.name) {
      out = img;
      return true;
    }
  return false;
}

bool FirmwareStore::get_info(const StoredImage &img, FirmwareInfo &out) const {
  const StoreHeader &h = img.hdr;
  out = FirmwareInfo{};
  out.id = std::string("store:") + h.name;
  out.name = h.name;
  out.version = h.version;
  out.kind = (ImageKind) h.kind;
  out.size = h.size;
  out.crc32 = h.crc32;
  out.source = h.source;
  for (int i = 0; i < 64; i++)
    if (h.hw_ids & (1ULL << i))
      out.hw_ids.push_back(i);
  return true;
}

bool FirmwareStore::erase_image(const std::string &name) {
  StoredImage img;
  if (!this->find(name, img))
    return false;
  bool ok = esp_partition_erase_range(this->partition_, (size_t) img.slot * SLOT_SIZE, 4096) == ESP_OK;  // kill the header
  this->rescan_();
  return ok;
}

bool FirmwareStore::begin_write(size_t total, const std::string &name, std::string &err) {
  if (this->partition_ == nullptr) { err = "NOT_ENOUGH_STORAGE: no fwstore partition"; return false; }
  if (total < 1024 || total > this->capacity()) {
    err = "NOT_ENOUGH_STORAGE: image size " + std::to_string(total) + " exceeds " + std::to_string(this->capacity());
    return false;
  }
  uint8_t span = (total + DATA_OFFSET + SLOT_SIZE - 1) / SLOT_SIZE;
  if (span > this->slots_) { err = "NOT_ENOUGH_STORAGE: image needs more slots than available"; return false; }
  int slot = -1;
  if (span == 1) {
    // same name -> replace in place
    for (auto &img : this->images_)
      if (name == img.hdr.name && img.hdr.span == 1) slot = img.slot;
    if (slot < 0) {  // first free slot
      for (uint8_t s = 0; s < this->slots_ && slot < 0; s++) {
        bool used = false;
        for (auto &img : this->images_)
          if (s >= img.slot && s < img.slot + img.hdr.span) used = true;
        if (!used) slot = s;
      }
    }
    if (slot < 0) {  // evict the oldest single-slot image (or the spanning one)
      uint32_t oldest = 0xFFFFFFFF;
      for (auto &img : this->images_)
        if (img.hdr.seq < oldest) { oldest = img.hdr.seq; slot = img.slot; }
    }
  } else {
    slot = 0;  // spanning images always start at slot 0 and evict whatever is there
  }
  if (slot < 0) { err = "NOT_ENOUGH_STORAGE: no slot"; return false; }
  size_t need = DATA_OFFSET + ((total + 4095) & ~4095);
  if (esp_partition_erase_range(this->partition_, (size_t) slot * SLOT_SIZE, need) != ESP_OK) { err = "INTERNAL_ERROR: erase failed"; return false; }
  // also invalidate headers of slots that get overrun by a spanning image
  for (uint8_t s = slot + 1; s < slot + span; s++)
    esp_partition_erase_range(this->partition_, (size_t) s * SLOT_SIZE, 4096);
  this->writing_ = true;
  this->write_slot_ = slot;
  this->write_span_ = span;
  this->write_total_ = total;
  this->write_pos_ = 0;
  this->buf_len_ = 0;
  ESP_LOGI(TAG, "writing %u bytes into slot %d (span %u)", (unsigned) total, slot, span);
  return true;
}

bool FirmwareStore::flush_() {
  if (this->buf_len_ == 0)
    return true;
  size_t len = (this->buf_len_ + 3) & ~3;  // 4-byte aligned writes
  if (len > this->buf_len_)
    memset(this->buf_ + this->buf_len_, 0xFF, len - this->buf_len_);
  esp_err_t e = esp_partition_write(this->partition_, (size_t) this->write_slot_ * SLOT_SIZE + DATA_OFFSET + this->write_pos_, this->buf_, len);
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
  this->rescan_();
}

bool FirmwareStore::finish_write(const std::string &name, const std::string &version, ImageKind kind, uint64_t hw_ids,
                                 const std::string &source, std::string &err) {
  if (!this->writing_) { err = "INVALID_REQUEST: not writing"; return false; }
  this->writing_ = false;
  if (!this->flush_()) { err = "INTERNAL_ERROR: flash write failed"; return false; }
  if (this->write_pos_ != this->write_total_) { err = "INVALID_REQUEST: short upload (" + std::to_string(this->write_pos_) + "/" + std::to_string(this->write_total_) + ")"; return false; }
  TelinkImageInfo info;
  std::string r = validate_telink_image(this->write_total_, this->reader(this->write_slot_), info, MAX_EXT_OTA_SIZE);
  if (r != "ok" && r != "sign") { err = "INVALID_IMAGE: " + r; this->rescan_(); return false; }
  StoreHeader h{};
  h.magic = MAGIC;
  h.size = this->write_total_;
  h.crc32 = info.crc_stored;
  h.seq = this->next_seq_++;
  h.kind = (uint8_t) kind;
  h.span = this->write_span_;
  h.hw_ids = hw_ids;
  strncpy(h.name, name.c_str(), sizeof(h.name) - 1);
  strncpy(h.version, version.c_str(), sizeof(h.version) - 1);
  strncpy(h.source, source.c_str(), sizeof(h.source) - 1);
  if (esp_partition_write(this->partition_, (size_t) this->write_slot_ * SLOT_SIZE, &h, sizeof(h)) != ESP_OK) { err = "INTERNAL_ERROR: header write failed"; return false; }
  ESP_LOGI(TAG, "stored %s v%s in slot %u (%u bytes, crc 0x%08X)", h.name, h.version, this->write_slot_, (unsigned) h.size, (unsigned) h.crc32);
  this->rescan_();
  return true;
}

bool FirmwareStore::read(uint8_t slot, size_t offset, uint8_t *buf, size_t len) const {
  if (this->partition_ == nullptr)
    return false;
  return esp_partition_read(this->partition_, (size_t) slot * SLOT_SIZE + DATA_OFFSET + offset, buf, len) == ESP_OK;
}

ImageReader FirmwareStore::reader(uint8_t slot) const {
  return [this, slot](size_t off, uint8_t *buf, size_t len) { return this->read(slot, off, buf, len); };
}

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
