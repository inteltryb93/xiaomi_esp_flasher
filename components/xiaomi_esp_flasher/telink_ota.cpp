#include "telink_ota.h"
#ifdef USE_ESP32
#include "telink_image.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <cstring>

namespace esphome {
namespace xiaomi_esp_flasher {

static const char *const TAG = "xiaomi_flasher.ota";

// Telink SDK ble_ll_ota.h
static const uint16_t CMD_OTA_FW_VERSION = 0xff00;
static const uint16_t CMD_OTA_START = 0xff01;
static const uint16_t CMD_OTA_END = 0xff02;
static const char *const OTA_ERRORS[] = {"Success",  "Lost one or more packets", "CRC error in data", "Writing data to flash",
                                         "Lost last one or more packets", "Timeout", "Firmware CRC check"};

const char *ota_state_name(OtaState s) {
  switch (s) {
    case OtaState::IDLE: return "idle";
    case OtaState::STARTING: return "starting";
    case OtaState::WRITING: return "writing";
    case OtaState::CHECKING: return "checking";
    case OtaState::FINISHING: return "finishing";
    case OtaState::DONE: return "done";
    case OtaState::FAILED: return "failed";
  }
  return "?";
}

const char *TelinkOtaProtocol::device_status_name(uint8_t s) {
  if (s < sizeof(OTA_ERRORS) / sizeof(OTA_ERRORS[0]))
    return OTA_ERRORS[s];
  return "Unknown";
}

void TelinkOtaProtocol::report_() {
  this->progress_.elapsed_ms = millis() - this->progress_.started_ms;
  if (this->progress_cb_)
    this->progress_cb_(this->progress_);
}

void TelinkOtaProtocol::begin(TelinkBleClient *client, uint16_t ota_handle, ImageReader reader, size_t image_size,
                              ProgressCallback progress, DoneCallback done) {
  this->client_ = client;
  this->handle_ = ota_handle;
  this->reader_ = std::move(reader);
  this->progress_cb_ = std::move(progress);
  this->done_cb_ = std::move(done);
  this->progress_ = OtaProgress{};
  this->progress_.bytes_total = image_size;
  this->progress_.blocks_total = (image_size + 15) / 16;  // pad last block to 16 bytes (pvvx: padHex)
  this->progress_.started_ms = millis();
  this->progress_.state = OtaState::STARTING;
  this->stage_ = 0;
  this->write_retries_ = 0;
  ESP_LOGI(TAG, "OTA begin: %u bytes, %u blocks, handle 0x%04X", (unsigned) image_size,
           (unsigned) this->progress_.blocks_total, ota_handle);
  // updateBegin(): 500 ms pause, then 00ff, 01ff, 300 ms pause, first block
  this->settle_until_ = millis() + 500;
  this->report_();
}

void TelinkOtaProtocol::abort(const std::string &reason) {
  if (!this->active())
    return;
  this->fail_(Error::INTERNAL_ERROR, reason);
}

void TelinkOtaProtocol::fail_(Error e, const std::string &msg) {
  ESP_LOGE(TAG, "OTA failed: %s (%s)", msg.c_str(), error_to_string(e));
  this->progress_.state = OtaState::FAILED;
  this->progress_.error = e;
  this->progress_.message = msg;
  this->report_();
  auto cb = std::move(this->done_cb_);
  this->done_cb_ = nullptr;
  if (cb)
    cb(false, e, msg);
}

void TelinkOtaProtocol::loop() {
  if (this->progress_.state == OtaState::STARTING && this->settle_until_ && (int32_t) (millis() - this->settle_until_) >= 0) {
    this->settle_until_ = 0;
    if (this->stage_ == 0) {
      this->send_start_();
    } else {
      // 300 ms after 01ff: start sending blocks
      this->progress_.state = OtaState::WRITING;
      this->progress_.started_ms = millis();
      this->send_block_();
    }
  }
}

void TelinkOtaProtocol::send_start_() {
  // 00ff (CMD_OTA_FW_VERSION) then 01ff (CMD_OTA_START), both write-without-response
  this->stage_ = 1;
  uint8_t ver[2] = {(uint8_t) (CMD_OTA_FW_VERSION & 0xff), (uint8_t) (CMD_OTA_FW_VERSION >> 8)};
  this->client_->write(this->handle_, ver, 2, false, [this](int st) {
    if (st != ESP_GATT_OK) {
      this->fail_(Error::OTA_WRITE_FAILED, "write CMD_OTA_FW_VERSION failed (" + std::to_string(st) + ")");
      return;
    }
    uint8_t start[2] = {(uint8_t) (CMD_OTA_START & 0xff), (uint8_t) (CMD_OTA_START >> 8)};
    this->client_->write(this->handle_, start, 2, false, [this](int st2) {
      if (st2 != ESP_GATT_OK) {
        this->fail_(Error::OTA_WRITE_FAILED, "write CMD_OTA_START failed (" + std::to_string(st2) + ")");
        return;
      }
      ESP_LOGI(TAG, "OTA started (00ff, 01ff sent)");
      this->settle_until_ = millis() + 300;
    });
  });
}

void TelinkOtaProtocol::send_block_() {
  if (this->progress_.state != OtaState::WRITING)
    return;
  uint32_t idx = this->progress_.blocks_sent;
  if (idx >= this->progress_.blocks_total) {
    this->send_end_();
    return;
  }
  // packet: [idx LE16][16 data bytes][crc16 LE16]   (sendOTAblock + getHexCRC)
  memset(this->pkt_ + 2, 0xFF, 16);
  size_t off = idx * 16;
  size_t n = this->progress_.bytes_total - off;
  if (n > 16)
    n = 16;
  if (!this->reader_(off, this->pkt_ + 2, n)) {
    this->fail_(Error::INTERNAL_ERROR, "image read failed at " + std::to_string(off));
    return;
  }
  this->pkt_[0] = idx & 0xff;
  this->pkt_[1] = idx >> 8;
  uint16_t crc = crc16_modbus(this->pkt_, 18);
  this->pkt_[18] = crc & 0xff;
  this->pkt_[19] = crc >> 8;
  this->client_->write(this->handle_, this->pkt_, 20, false, [this, idx](int st) {
    if (this->progress_.state != OtaState::WRITING)
      return;
    if (st != ESP_GATT_OK) {
      // Transport-level failure before the device consumed the packet: the same index may be re-sent
      // (the firmware detects lost packets by index and reports PACKET_LOSS on the next status read).
      if (st == ESP_GATT_CONGESTED || st == ESP_GATT_BUSY) {
        if (++this->write_retries_ <= 20) {
          this->progress_.retries++;
          ESP_LOGW(TAG, "block %u congested (%d), retry %u", (unsigned) idx, st, this->write_retries_);
          this->send_block_();
          return;
        }
      }
      this->fail_(Error::OTA_WRITE_FAILED, "GATT write failed at block " + std::to_string(idx) + " (" + std::to_string(st) + ")");
      return;
    }
    this->write_retries_ = 0;
    this->progress_.blocks_sent = idx + 1;
    size_t sent = (size_t) this->progress_.blocks_sent * 16;
    this->progress_.bytes_sent = sent > this->progress_.bytes_total ? this->progress_.bytes_total : sent;
    uint32_t now = millis();
    if (now - this->last_report_ms_ >= 500 || this->progress_.blocks_sent == this->progress_.blocks_total) {
      this->last_report_ms_ = now;
      this->report_();
    }
    // every 8th block: read the OTA characteristic (status byte + flow control) – sendOTAblock()
    if ((this->progress_.blocks_sent % 8) == 0)
      this->read_status_();
    else
      this->send_block_();
  }, 5000);
}

void TelinkOtaProtocol::read_status_() {
  this->progress_.state = OtaState::CHECKING;
  this->client_->read(this->handle_, [this](int st, const uint8_t *data, size_t len) {
    if (this->progress_.state != OtaState::CHECKING)
      return;
    if (st != ESP_GATT_OK) {
      this->fail_(Error::OTA_WRITE_FAILED, "OTA status read failed (" + std::to_string(st) + ") after block " +
                                             std::to_string(this->progress_.blocks_sent));
      return;
    }
    if (len > 0) {
      this->progress_.device_status = data[0];
      if (data[0] != 0) {
        // Device reported an error – never retry, the pvvx flasher aborts here as well.
        this->fail_(Error::OTA_DEVICE_ERROR, std::string("device OTA error after block ") +
                                                 std::to_string(this->progress_.blocks_sent) + ": " +
                                                 device_status_name(data[0]));
        return;
      }
    }
    this->progress_.state = OtaState::WRITING;
    this->send_block_();
  }, 5000);
}

void TelinkOtaProtocol::send_end_() {
  // sendLastOTA(): 02ff + (n-1) LE16 + (~(n-1)) LE16
  this->progress_.state = OtaState::FINISHING;
  uint16_t last = this->progress_.blocks_total - 1;
  uint16_t nlast = (~last) & 0xffff;
  uint8_t end[6] = {(uint8_t) (CMD_OTA_END & 0xff), (uint8_t) (CMD_OTA_END >> 8), (uint8_t) (last & 0xff),
                    (uint8_t) (last >> 8), (uint8_t) (nlast & 0xff), (uint8_t) (nlast >> 8)};
  this->client_->write(this->handle_, end, 6, false, [this](int st) {
    if (st != ESP_GATT_OK && st != GATT_ERR_DISCONNECTED) {
      this->fail_(Error::OTA_WRITE_FAILED, "write CMD_OTA_END failed (" + std::to_string(st) + ")");
      return;
    }
    ESP_LOGI(TAG, "OTA send %u blocks - ok (%.1f s), reading final status", (unsigned) this->progress_.blocks_total,
             (millis() - this->progress_.started_ms) / 1000.0f);
    // The WRITE_CHAR_EVT of a write-without-response only means "queued in the local stack". A read request is
    // ordered behind the queued writes (ATT is sequential), so its response proves the peripheral consumed the last
    // data blocks and the end packet, and returns the SDK result (OTA_SUCCESS / DATA_UNCOMPLETE / FW_CHECK_ERR ...).
    // The device drops the link by itself when it reboots into the new image – never disconnect from our side.
    this->client_->read(this->handle_, [this](int rst, const uint8_t *data, size_t len) {
      if (this->progress_.state != OtaState::FINISHING)
        return;
      if (rst == ESP_GATT_OK && len > 0) {
        this->progress_.device_status = data[0];
        if (data[0] != 0) {
          this->fail_(Error::OTA_DEVICE_ERROR, std::string("device rejected the image at the end: ") + device_status_name(data[0]));
          return;
        }
        this->progress_.message = "device confirmed the image (status 0), rebooting";
      } else if (rst == GATT_ERR_DISCONNECTED) {
        this->progress_.message = "device dropped the link after the end packet (rebooting)";
      } else {
        this->progress_.message = "final status read failed (" + std::to_string(rst) + "), verifying by reconnect";
      }
      this->progress_.state = OtaState::DONE;
      ESP_LOGI(TAG, "%s", this->progress_.message.c_str());
      this->report_();
      auto cb = std::move(this->done_cb_);
      this->done_cb_ = nullptr;
      if (cb)
        cb(true, Error::NONE, this->progress_.message);
    }, 8000);
  });
}

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
