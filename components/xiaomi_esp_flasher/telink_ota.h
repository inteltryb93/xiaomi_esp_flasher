#pragma once
// TelinkOtaProtocol: Telink BLE OTA exactly as TelinkMiFlasher.html / TelinkOTA.html perform it.
// See docs/telink_ota_protocol.md §4.  Only the OTA transfer lives here – connection handling,
// identification and compatibility checks are the caller's job (XiaomiEspFlasher session).
#include "esphome/core/defines.h"
#ifdef USE_ESP32
#include <cstdint>
#include <functional>
#include <string>
#include "telink_ble_client.h"
#include "errors.h"

namespace esphome {
namespace xiaomi_esp_flasher {

enum class OtaState : uint8_t {
  IDLE,
  STARTING,   // 00ff / 01ff sent, waiting the 300 ms settle time
  WRITING,    // data blocks
  CHECKING,   // reading status after every 8th block
  FINISHING,  // 02ff end packet sent
  DONE,
  FAILED,
};
const char *ota_state_name(OtaState s);

struct OtaProgress {
  OtaState state{OtaState::IDLE};
  uint32_t blocks_total{0};
  uint32_t blocks_sent{0};
  uint32_t bytes_total{0};
  uint32_t bytes_sent{0};
  uint32_t started_ms{0};
  uint32_t elapsed_ms{0};
  uint32_t retries{0};
  uint8_t device_status{0};   // last OTA status byte read from the device (SDK enum)
  Error error{Error::NONE};
  std::string message;
  float percent() const { return blocks_total ? 100.0f * blocks_sent / blocks_total : 0.0f; }
  float bytes_per_second() const { return elapsed_ms ? 1000.0f * bytes_sent / elapsed_ms : 0.0f; }
};

// Firmware image accessor: (offset, buf, len) -> ok.  Works for RAM, flash partition or rodata.
using ImageReader = std::function<bool(size_t offset, uint8_t *buf, size_t len)>;

class TelinkOtaProtocol {
 public:
  using ProgressCallback = std::function<void(const OtaProgress &)>;
  using DoneCallback = std::function<void(bool ok, Error err, const std::string &msg)>;

  // image_size may be non-multiple of 16: the last block is padded with 0xFF (pvvx pads the hex string).
  void begin(TelinkBleClient *client, uint16_t ota_handle, ImageReader reader, size_t image_size,
             ProgressCallback progress, DoneCallback done);
  void abort(const std::string &reason);
  void loop();  // drives the settle timers
  const OtaProgress &progress() const { return this->progress_; }
  bool active() const { return this->progress_.state != OtaState::IDLE && this->progress_.state != OtaState::DONE && this->progress_.state != OtaState::FAILED; }
  static const char *device_status_name(uint8_t s);

 protected:
  void send_start_();
  void send_block_();
  void read_status_();
  void send_end_();
  void fail_(Error e, const std::string &msg);
  void report_();

  TelinkBleClient *client_{nullptr};
  uint16_t handle_{0};
  ImageReader reader_;
  OtaProgress progress_;
  ProgressCallback progress_cb_;
  DoneCallback done_cb_;
  uint32_t settle_until_{0};
  uint8_t stage_{0};
  uint8_t write_retries_{0};
  uint8_t pkt_[20];
  uint32_t last_report_ms_{0};
};

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
