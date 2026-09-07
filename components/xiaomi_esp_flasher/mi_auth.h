#pragma once
// MiAuth: Xiaomi BLE "Do Activation" (registration) and "Login" over service 0xFE95, characteristics
// 0x0010 (control) and 0x0019 (data).  A faithful port of miAuthorization()/startRegister()/sendRegister()/
// sendLogin()/doGenerate()/makeSharedKey()/deriveTheKey()/do_login_generate() from TelinkMiFlasher.html
// (docs/telink_ota_protocol.md §3).  Crypto: mbedtls (ECDH P-256, HKDF-SHA256, AES-128-CCM, HMAC-SHA256).
#include "esphome/core/defines.h"
#ifdef USE_ESP32
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "telink_ble_client.h"
#include "errors.h"

namespace esphome {
namespace xiaomi_esp_flasher {

struct MiKeys {
  bool valid{false};
  uint8_t token[12]{};
  uint8_t bind_key[16]{};
  std::string device_id;  // "blt.3.129v..." (ASCII part of the 20-byte id)
  std::string token_hex() const;
  std::string bind_key_hex() const;
  bool set_from_hex(const std::string &token_hex, const std::string &bind_key_hex);
};

class MiAuth {
 public:
  enum class Mode : uint8_t { REGISTER, LOGIN };
  using DoneCallback = std::function<void(bool ok, Error err, const std::string &msg)>;
  using LogCallback = std::function<void(const std::string &line)>;

  void begin(TelinkBleClient *client, const GattChar &c10, const GattChar &c19, Mode mode, const MiKeys *login_keys,
             DoneCallback done, LogCallback log);
  void on_notify(uint16_t handle, const uint8_t *data, size_t len);
  void loop();
  void abort(const std::string &why);
  bool active() const { return this->active_; }
  const MiKeys &keys() const { return this->keys_; }

  // Known-answer self test of the key derivation (vectors from scripts/mi_auth_reference.py).
  static bool self_test(std::string &report);

  // exposed for the self test
  static bool derive_setup_keys(const uint8_t shared[32], uint8_t out64[64]);
  static bool encrypt_did(const uint8_t key_a[16], const uint8_t *did, size_t did_len, std::vector<uint8_t> &out);
  static bool login_generate(const uint8_t token[12], const uint8_t rand_host[16], const uint8_t rand_dev[16],
                             uint8_t expected[32], uint8_t send[32]);

 protected:
  // BLE helpers
  void write10_(std::initializer_list<uint8_t> bytes);
  void write19_(std::initializer_list<uint8_t> bytes);
  void write19v_(const std::vector<uint8_t> &v);
  void enqueue_(uint16_t handle, std::vector<uint8_t> data);
  void pump_();
  void finish_(bool ok, Error e, const std::string &msg);
  void log_(const std::string &s);
  // flow
  void start_register_();  // startRegister()
  void send_login_();      // sendLogin()
  bool generate_keypair_();
  bool make_shared_key_();
  void handle_10_(const uint8_t *d, size_t n);
  void handle_19_(const uint8_t *d, size_t n);
  void handle_19_register_(const uint8_t *d, size_t n);
  void handle_19_login_(const uint8_t *d, size_t n);

  TelinkBleClient *client_{nullptr};
  GattChar c10_, c19_;
  Mode mode_{Mode::REGISTER};
  bool mode_activation_{true};   // JS: mode_activation (1 register, 0 login)
  int state_{0};                 // JS: state
  bool active_{false};
  bool is_activated_{false};
  bool logged_in_{false};
  bool write_busy_{false};
  uint32_t deadline_{0};
  uint32_t delayed_at_{0};
  uint8_t delayed_action_{0};  // 1 = startRegister, 2 = sendLogin
  std::deque<std::pair<uint16_t, std::vector<uint8_t>>> queue_;
  DoneCallback done_;
  LogCallback log_cb_;
  MiKeys keys_;
  // crypto material
  uint8_t own_priv_[32]{};
  uint8_t own_pub_[65]{};      // 04 || X || Y
  uint8_t dev_pub_[65]{};
  size_t dev_pub_len_{0};
  std::vector<uint8_t> device_new_id_;    // 20 bytes: 0x00 + "blt.3.129v" + rand6 + "g00" (or from device)
  std::vector<uint8_t> device_known_id_;
  std::vector<uint8_t> mi_write_did_;
  uint8_t rand_host_[16]{};
  uint8_t rand_dev_[16]{};
  uint8_t expected_[32]{};
  uint8_t send_info_[32]{};
  std::vector<uint8_t> dev_info_recv_;
};

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
