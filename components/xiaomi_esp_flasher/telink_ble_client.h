#pragma once
// TelinkBleClient: thin asynchronous GATT-client layer on top of ESPHome's esp32_ble_client::BLEClientBase.
// Responsibilities: connect / service discovery / read / write / notify / disconnect with per-operation
// callbacks and timeouts.  It knows nothing about the pvvx or OTA protocols (see telink_ota.*, mi_auth.*).
#include "esphome/core/defines.h"
#ifdef USE_ESP32
#include <functional>
#include <vector>
#include <string>
#include "esphome/components/esp32_ble_client/ble_client_base.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/component.h"

namespace esphome {
namespace xiaomi_esp_flasher {

namespace espbt = esphome::esp32_ble_tracker;

struct GattChar {
  uint16_t handle{0};
  uint16_t cccd_handle{0};
  esp_gatt_char_prop_t props{0};
  espbt::ESPBTUUID uuid;
  espbt::ESPBTUUID service_uuid;
};

// status: ESP_GATT_OK on success, otherwise an esp_gatt_status_t / ESP_GATT_ERROR, or GATT_ERR_TIMEOUT below
constexpr int GATT_ERR_TIMEOUT = 0x100;
constexpr int GATT_ERR_DISCONNECTED = 0x101;
constexpr int GATT_ERR_BUSY = 0x102;

using ReadCallback = std::function<void(int status, const uint8_t *data, size_t len)>;
using StatusCallback = std::function<void(int status)>;
using NotifyCallback = std::function<void(uint16_t handle, const uint8_t *data, size_t len)>;

class TelinkBleClient : public esp32_ble_client::BLEClientBase {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  // Ask the tracker to connect to `address` (MSB-first uint64) – the tracker stops the scan and calls connect().
  bool begin_connect(uint64_t address, esp_ble_addr_type_t addr_type);
  void end_connection();  // graceful disconnect
  bool is_established() { return this->state() == espbt::ClientState::ESTABLISHED; }
  bool is_idle() { return this->state() == espbt::ClientState::IDLE; }
  uint16_t mtu() const { return this->mtu_; }

  // Discovery results (valid while established)
  std::vector<espbt::ESPBTUUID> service_uuids();
  bool has_service(const espbt::ESPBTUUID &uuid);
  bool find_characteristic(const espbt::ESPBTUUID &service, const espbt::ESPBTUUID &chr, GattChar &out);

  // Asynchronous operations (one at a time; callbacks run on the main loop)
  bool read(uint16_t handle, ReadCallback cb, uint32_t timeout_ms = 3000);
  bool write(uint16_t handle, const uint8_t *data, size_t len, bool with_response, StatusCallback cb,
             uint32_t timeout_ms = 3000);
  bool subscribe(const GattChar &chr, StatusCallback cb, uint32_t timeout_ms = 4000);
  bool busy() const { return this->op_ != Op::NONE; }

  void set_notify_callback(NotifyCallback cb) { this->notify_cb_ = std::move(cb); }
  void set_connect_callback(std::function<void(bool established, int reason)> cb) { this->connect_cb_ = std::move(cb); }
  // Request a faster connection interval (units of 1.25 ms) – used during OTA.
  void request_conn_params(uint16_t min_int, uint16_t max_int, uint16_t latency, uint16_t timeout);

  bool gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void on_disconnect_complete(esp_err_t reason) override;
  bool parse_device(const espbt::ESPBTDevice &device) override { return false; }  // we never auto-connect

 protected:
  enum class Op : uint8_t { NONE, READ, WRITE, SUBSCRIBE_REG, SUBSCRIBE_CCCD };
  void finish_op_(int status, const uint8_t *data = nullptr, size_t len = 0);
  void arm_timeout_(uint32_t ms);

  Op op_{Op::NONE};
  uint16_t op_handle_{0};
  ReadCallback read_cb_;
  StatusCallback status_cb_;
  NotifyCallback notify_cb_;
  std::function<void(bool, int)> connect_cb_;
  uint32_t op_started_{0};
  uint32_t op_timeout_{0};
  uint16_t pending_cccd_{0};
  bool connect_reported_{false};
  bool congested_{false};
};

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
