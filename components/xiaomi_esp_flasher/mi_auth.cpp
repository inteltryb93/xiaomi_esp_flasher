#include "mi_auth.h"
#ifdef USE_ESP32
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <cstring>
#include <esp_random.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/ccm.h>
#include <mbedtls/md.h>

namespace esphome {
namespace xiaomi_esp_flasher {

static const char *const TAG = "xiaomi_flasher.miauth";

static int rng_cb(void *, unsigned char *out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

static std::string hexs(const uint8_t *d, size_t n) {
  static const char *H = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    s += H[d[i] >> 4];
    s += H[d[i] & 15];
  }
  return s;
}
static bool unhex(const std::string &h, uint8_t *out, size_t n) {
  if (h.size() != n * 2)
    return false;
  for (size_t i = 0; i < n; i++) {
    char buf[3] = {h[2 * i], h[2 * i + 1], 0};
    char *end;
    long v = strtol(buf, &end, 16);
    if (*end)
      return false;
    out[i] = (uint8_t) v;
  }
  return true;
}

std::string MiKeys::token_hex() const { return hexs(this->token, 12); }
std::string MiKeys::bind_key_hex() const { return hexs(this->bind_key, 16); }
bool MiKeys::set_from_hex(const std::string &t, const std::string &b) {
  if (!unhex(t, this->token, 12) || !unhex(b, this->bind_key, 16))
    return false;
  this->valid = true;
  return true;
}

// ---------------------------------------------------------------- crypto primitives

// deriveTheKey(): HKDF-SHA256(shared, salt = empty, info = "mible-setup-info", 64 bytes)
bool MiAuth::derive_setup_keys(const uint8_t shared[32], uint8_t out64[64]) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  static const char *INFO = "mible-setup-info";
  // sjcl.misc.hkdf with salt == null uses an empty salt => HMAC key of length 0 (== HashLen zeros, RFC5869)
  return mbedtls_hkdf(md, nullptr, 0, shared, 32, (const uint8_t *) INFO, strlen(INFO), out64, 64) == 0;
}

// mi_write_did = AES-CCM(key A, nonce 101112131415161718191A1B, aad "devID", tag 4 bytes).encrypt(device_new_id)
bool MiAuth::encrypt_did(const uint8_t key_a[16], const uint8_t *did, size_t did_len, std::vector<uint8_t> &out) {
  static const uint8_t NONCE[12] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B};
  static const uint8_t AAD[5] = {'d', 'e', 'v', 'I', 'D'};
  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);
  bool ok = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key_a, 128) == 0;
  out.assign(did_len + 4, 0);
  if (ok)
    ok = mbedtls_ccm_encrypt_and_tag(&ctx, did_len, NONCE, sizeof(NONCE), AAD, sizeof(AAD), did, out.data(),
                                     out.data() + did_len, 4) == 0;
  mbedtls_ccm_free(&ctx);
  return ok;
}

// do_login_generate()
bool MiAuth::login_generate(const uint8_t token[12], const uint8_t rand_host[16], const uint8_t rand_dev[16],
                            uint8_t expected[32], uint8_t send[32]) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  static const char *INFO = "mible-login-info";
  uint8_t salt[32], salt1[32], derived[64];
  memcpy(salt, rand_host, 16);
  memcpy(salt + 16, rand_dev, 16);
  memcpy(salt1, rand_dev, 16);
  memcpy(salt1 + 16, rand_host, 16);
  if (mbedtls_hkdf(md, salt, 32, token, 12, (const uint8_t *) INFO, strlen(INFO), derived, 64) != 0)
    return false;
  if (mbedtls_md_hmac(md, derived, 16, salt1, 32, expected) != 0)
    return false;
  if (mbedtls_md_hmac(md, derived + 16, 16, salt, 32, send) != 0)
    return false;
  return true;
}

bool MiAuth::generate_keypair_() {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point Q;
  mbedtls_mpi d;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&Q);
  mbedtls_mpi_init(&d);
  bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_ecp_gen_keypair(&grp, &d, &Q, rng_cb, nullptr) == 0 &&
            mbedtls_mpi_write_binary(&d, this->own_priv_, 32) == 0;
  size_t olen = 0;
  if (ok)
    ok = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, this->own_pub_, 65) == 0 && olen == 65;
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&Q);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

// makeSharedKey() + deriveTheKey()
bool MiAuth::make_shared_key_() {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point Qp;
  mbedtls_mpi d, z;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&Qp);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&z);
  uint8_t shared[32];
  bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_ecp_point_read_binary(&grp, &Qp, this->dev_pub_, this->dev_pub_len_) == 0 &&
            mbedtls_mpi_read_binary(&d, this->own_priv_, 32) == 0 &&
            mbedtls_ecdh_compute_shared(&grp, &z, &Qp, &d, rng_cb, nullptr) == 0 &&
            mbedtls_mpi_write_binary(&z, shared, 32) == 0;
  mbedtls_mpi_free(&z);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&Qp);
  mbedtls_ecp_group_free(&grp);
  if (!ok) {
    this->log_("ECDH failed");
    return false;
  }
  uint8_t derived[64];
  if (!derive_setup_keys(shared, derived)) {
    this->log_("HKDF failed");
    return false;
  }
  memcpy(this->keys_.token, derived, 12);        // derived_key.substring(0, 24)
  memcpy(this->keys_.bind_key, derived + 12, 16);  // substring(24, 56)
  const uint8_t *mi_bind_a = derived + 28;         // substring(56, 88)
  this->keys_.valid = true;
  this->keys_.device_id = std::string((const char *) this->device_new_id_.data() + 1, this->device_new_id_.size() - 1);
  if (!encrypt_did(mi_bind_a, this->device_new_id_.data(), this->device_new_id_.size(), this->mi_write_did_)) {
    this->log_("AES-CCM failed");
    return false;
  }
  this->log_("Keys derived: token " + this->keys_.token_hex() + " bindkey " + this->keys_.bind_key_hex());
  return true;
}

bool MiAuth::self_test(std::string &report) {
  // Vectors computed with scripts/mi_auth_reference.py (python 'cryptography'); see docs/testing.md
  static const uint8_t SHARED[32] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                                     0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
  uint8_t derived[64];
  bool ok = derive_setup_keys(SHARED, derived);
  report = "hkdf:" + hexs(derived, 64);
  static const uint8_t DID[20] = {0x00, 'b', 'l', 't', '.', '3', '.', '1', '2', '9', 'v', 'a', 'b', 'c', 'd', 'e', 'f', 'g', '0', '0'};
  std::vector<uint8_t> enc;
  ok = ok && encrypt_did(derived + 28, DID, 20, enc);
  report += " ccm:" + hexs(enc.data(), enc.size());
  uint8_t rh[16], rd[16], exp[32], snd[32];
  for (int i = 0; i < 16; i++) { rh[i] = i; rd[i] = 0x20 + i; }
  ok = ok && login_generate(derived, rh, rd, exp, snd);
  report += " expected:" + hexs(exp, 32) + " send:" + hexs(snd, 32);
  return ok;
}

// ---------------------------------------------------------------- BLE plumbing

void MiAuth::log_(const std::string &s) {
  ESP_LOGD(TAG, "%s", s.c_str());
  if (this->log_cb_)
    this->log_cb_(s);
}

void MiAuth::enqueue_(uint16_t handle, std::vector<uint8_t> data) {
  this->queue_.emplace_back(handle, std::move(data));
  this->pump_();
}
void MiAuth::write10_(std::initializer_list<uint8_t> b) { this->enqueue_(this->c10_.handle, std::vector<uint8_t>(b)); }
void MiAuth::write19_(std::initializer_list<uint8_t> b) { this->enqueue_(this->c19_.handle, std::vector<uint8_t>(b)); }
void MiAuth::write19v_(const std::vector<uint8_t> &v) { this->enqueue_(this->c19_.handle, v); }

void MiAuth::pump_() {
  if (!this->active_ || this->write_busy_ || this->queue_.empty() || this->client_->busy())
    return;
  auto item = std::move(this->queue_.front());
  this->queue_.pop_front();
  this->write_busy_ = true;
  bool rsp = (item.first == this->c10_.handle ? this->c10_.props : this->c19_.props) & ESP_GATT_CHAR_PROP_BIT_WRITE;
  ESP_LOGV(TAG, "-> %04X %s", item.first, hexs(item.second.data(), item.second.size()).c_str());
  this->client_->write(item.first, item.second.data(), item.second.size(), rsp, [this](int st) {
    this->write_busy_ = false;
    if (st != ESP_GATT_OK) {
      this->finish_(false, Error::ACTIVATION_FAILED, "GATT write failed (" + std::to_string(st) + ")");
      return;
    }
    this->pump_();
  });
}

void MiAuth::finish_(bool ok, Error e, const std::string &msg) {
  if (!this->active_)
    return;
  this->active_ = false;
  this->queue_.clear();
  auto cb = std::move(this->done_);
  this->done_ = nullptr;
  if (cb)
    cb(ok, e, msg);
}

void MiAuth::abort(const std::string &why) { this->finish_(false, Error::ACTIVATION_FAILED, why); }

void MiAuth::loop() {
  if (!this->active_)
    return;
  if ((int32_t) (millis() - this->deadline_) > 0) {
    this->finish_(false, Error::ACTIVATION_FAILED, "activation/login timeout");
    return;
  }
  if (this->delayed_action_ && (int32_t) (millis() - this->delayed_at_) >= 0) {
    uint8_t a = this->delayed_action_;
    this->delayed_action_ = 0;
    if (a == 1)
      this->start_register_();
    else if (a == 2)
      this->send_login_();
  }
  this->pump_();
}

void MiAuth::begin(TelinkBleClient *client, const GattChar &c10, const GattChar &c19, Mode mode,
                   const MiKeys *login_keys, DoneCallback done, LogCallback log) {
  this->client_ = client;
  this->c10_ = c10;
  this->c19_ = c19;
  this->mode_ = mode;
  this->done_ = std::move(done);
  this->log_cb_ = std::move(log);
  this->active_ = true;
  this->logged_in_ = false;
  this->is_activated_ = false;
  this->write_busy_ = false;
  this->queue_.clear();
  this->delayed_action_ = 0;
  this->deadline_ = millis() + 90000;
  this->state_ = 0;
  // device_new_id = 00 + "blt.3.129v" + 6 random alnum + "g00"   (TelinkMiFlasher.html global)
  static const char *ALNUM = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  this->device_new_id_.clear();
  this->device_new_id_.push_back(0x00);
  for (const char *p = "blt.3.129v"; *p; p++) this->device_new_id_.push_back(*p);
  for (int i = 0; i < 6; i++) this->device_new_id_.push_back(ALNUM[esp_random() % 62]);
  for (const char *p = "g00"; *p; p++) this->device_new_id_.push_back(*p);
  if (mode == Mode::LOGIN) {
    if (login_keys == nullptr || !login_keys->valid) {
      this->finish_(false, Error::INVALID_REQUEST, "login requires token and bind key");
      return;
    }
    this->keys_ = *login_keys;
    this->send_login_();
  } else {
    // sendRegister(): state = 0, mode_activation = 1, doGenerate(), write a2000000 to enc_10
    this->log_("Activating now, please wait...");
    this->mode_activation_ = true;
    if (!this->generate_keypair_()) {
      this->finish_(false, Error::INTERNAL_ERROR, "ECDH keypair generation failed");
      return;
    }
    this->write10_({0xa2, 0x00, 0x00, 0x00});
  }
}

void MiAuth::start_register_() {
  // startRegister(): 15000000 -> enc_10; mode=1; state=1; doGenerate(); 000000030400 -> enc_19
  this->write10_({0x15, 0x00, 0x00, 0x00});
  this->mode_activation_ = true;
  this->state_ = 1;
  this->generate_keypair_();
  this->write19_({0x00, 0x00, 0x00, 0x03, 0x04, 0x00});
}

void MiAuth::send_login_() {
  // sendLogin(): random 16 bytes; state=0; mode=0; 24000000 -> enc_10; 0000000b0100 -> enc_19
  this->log_("Send Login, please wait...");
  esp_fill_random(this->rand_host_, 16);
  this->state_ = 0;
  this->mode_activation_ = false;
  this->write10_({0x24, 0x00, 0x00, 0x00});
  this->write19_({0x00, 0x00, 0x00, 0x0b, 0x01, 0x00});
}

void MiAuth::on_notify(uint16_t handle, const uint8_t *d, size_t n) {
  if (!this->active_)
    return;
  ESP_LOGV(TAG, "<- %04X %s", handle, hexs(d, n).c_str());
  if (handle == this->c10_.handle)
    this->handle_10_(d, n);
  else if (handle == this->c19_.handle)
    this->handle_19_(d, n);
}

static bool eq(const uint8_t *d, size_t n, std::initializer_list<uint8_t> v) {
  if (n != v.size())
    return false;
  size_t i = 0;
  for (uint8_t b : v)
    if (d[i++] != b)
      return false;
  return true;
}
static bool starts(const uint8_t *d, size_t n, std::initializer_list<uint8_t> v) {
  if (n < v.size())
    return false;
  size_t i = 0;
  for (uint8_t b : v)
    if (d[i++] != b)
      return false;
  return true;
}

void MiAuth::handle_10_(const uint8_t *d, size_t n) {
  if (n < 4)
    return;
  uint32_t v = (uint32_t) d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3];
  switch (v) {
    case 0x11000000:  // REG_SUCCESS
      this->log_("Registration successful");
      this->send_login_();
      break;
    case 0x12000000: this->finish_(false, Error::ACTIVATION_FAILED, "Registration failed!"); break;
    case 0x13000000: this->log_("Registration verify successful"); break;
    case 0x14000000: this->finish_(false, Error::ACTIVATION_FAILED, "Registration verify failed!"); break;
    case 0x21000000:  // LOG_SUCCESS
      this->logged_in_ = true;
      this->log_("Login successful");
      this->finish_(true, Error::NONE, "Login successful");
      break;
    case 0x22000000: this->finish_(false, Error::ACTIVATION_FAILED, "Login invalid LTMK!"); break;
    case 0x23000000: this->finish_(false, Error::ACTIVATION_FAILED, "Login Failed!"); break;
    case 0xe0000000: this->finish_(false, Error::ACTIVATION_FAILED, "Not registered!"); break;
    case 0xe1000000: this->finish_(false, Error::ACTIVATION_FAILED, "Error Registered!"); break;
    case 0xe2000000:  // ERR_REPEAT_LOGIN
      this->logged_in_ = true;
      this->log_("Repeat login!");
      this->finish_(true, Error::NONE, "Repeat login (already logged in)");
      break;
    case 0xe3000000: this->finish_(false, Error::ACTIVATION_FAILED, "Error: Invalid OOB!"); break;
    default: break;
  }
}

void MiAuth::handle_19_(const uint8_t *d, size_t n) {
  // common: 000004xx frames (device asks for ack)
  if (n >= 4 && d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x04) {
    std::vector<uint8_t> v = {0x00, 0x00, 0x05, d[3]};
    v.insert(v.end(), d + 4, d + n);
    this->write19v_(v);
    if (d[3] == 0x01) {
      this->delayed_action_ = 2;  // setTimeout(sendLogin, 250)
      this->delayed_at_ = millis() + 250;
    }
    return;
  }
  if (this->mode_activation_)
    this->handle_19_register_(d, n);
  else
    this->handle_19_login_(d, n);
}

void MiAuth::handle_19_register_(const uint8_t *d, size_t n) {
  if (starts(d, n, {0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00})) {
    // device_new_id = value.substring(16)
    this->device_new_id_.assign(d + 8, d + n);
    this->log_("Device id from device: " + std::string((const char *) d + 9, n - 9));
    this->write19_({0x00, 0x00, 0x01, 0x00});
    this->delayed_action_ = 1;
    this->delayed_at_ = millis() + 250;
  } else if (eq(d, n, {0x00, 0x00, 0x00, 0x00, 0x01, 0x00})) {
    this->is_activated_ = false;
    this->write19_({0x00, 0x00, 0x01, 0x01});
  } else if (eq(d, n, {0x00, 0x00, 0x00, 0x00, 0x02, 0x00})) {
    this->is_activated_ = true;
    this->write19_({0x00, 0x00, 0x01, 0x01});
  } else if (this->is_activated_ && this->state_ == 0 && starts(d, n, {0x01, 0x00})) {
    this->device_known_id_.assign(d + 2, d + n);
  } else if (this->is_activated_ && this->state_ == 0 && starts(d, n, {0x02, 0x00})) {
    this->device_known_id_.insert(this->device_known_id_.end(), d + 2, d + n);
    // device_new_id = device_known_id.substring(8) (hex chars) -> skip 4 bytes
    if (this->device_known_id_.size() > 4)
      this->device_new_id_.assign(this->device_known_id_.begin() + 4, this->device_known_id_.end());
    this->log_("Known device id: " + std::string((const char *) this->device_new_id_.data() + 1, this->device_new_id_.size() - 1));
    this->write19_({0x00, 0x00, 0x01, 0x00});
    this->delayed_action_ = 1;
    this->delayed_at_ = millis() + 250;
  } else if (eq(d, n, {0x01, 0x00, 0x01, 0x00, 0x00, 0x00})) {
    this->write19_({0x00, 0x00, 0x01, 0x00});
    this->delayed_action_ = 1;
    this->delayed_at_ = millis() + 250;
  } else if (this->state_ == 1 && eq(d, n, {0x00, 0x00, 0x01, 0x01})) {
    // send own public key (without the leading 04) in 4 frames of 18 bytes: 0100, 0200, 0300, 0400
    this->state_ = 2;
    const uint8_t *k = this->own_pub_ + 1;
    for (int f = 0; f < 4; f++) {
      std::vector<uint8_t> v = {(uint8_t) (f + 1), 0x00};
      size_t off = f * 18;
      size_t len = (off + 18 <= 64) ? 18 : 64 - off;
      v.insert(v.end(), k + off, k + off + len);
      this->write19v_(v);
    }
  } else if (eq(d, n, {0x00, 0x00, 0x00, 0x03, 0x04, 0x00})) {
    this->write19_({0x00, 0x00, 0x01, 0x01});
  } else if (this->state_ == 2 && n >= 2 && d[1] == 0x00 && d[0] >= 0x01 && d[0] <= 0x04) {
    if (d[0] == 0x01) {
      this->dev_pub_[0] = 0x04;
      this->dev_pub_len_ = 1;
    }
    size_t room = sizeof(this->dev_pub_) - this->dev_pub_len_;
    size_t len = n - 2 < room ? n - 2 : room;
    memcpy(this->dev_pub_ + this->dev_pub_len_, d + 2, len);
    this->dev_pub_len_ += len;
    if (d[0] == 0x04) {
      this->write19_({0x00, 0x00, 0x01, 0x00});
      if (!this->make_shared_key_()) {
        this->finish_(false, Error::INTERNAL_ERROR, "key derivation failed");
        return;
      }
      this->write19_({0x00, 0x00, 0x00, 0x00, 0x02, 0x00});  // deriveTheKey() tail
    }
  } else if (this->state_ == 2 && eq(d, n, {0x00, 0x00, 0x01, 0x01})) {
    // send mi_write_did in 2 frames: 0100 + [0..18), 0200 + [18..)
    this->state_ = 3;
    std::vector<uint8_t> f1 = {0x01, 0x00};
    size_t l1 = this->mi_write_did_.size() < 18 ? this->mi_write_did_.size() : 18;
    f1.insert(f1.end(), this->mi_write_did_.begin(), this->mi_write_did_.begin() + l1);
    this->write19v_(f1);
    if (this->mi_write_did_.size() > 18) {
      std::vector<uint8_t> f2 = {0x02, 0x00};
      f2.insert(f2.end(), this->mi_write_did_.begin() + 18, this->mi_write_did_.end());
      this->write19v_(f2);
    }
  } else if (this->state_ == 3 && eq(d, n, {0x00, 0x00, 0x01, 0x00})) {
    this->state_ = 0;
    this->write10_({0x13, 0x00, 0x00, 0x00});  // REG_VERIFY_SUCC ?
  } else if (eq(d, n, {0x00, 0x00, 0x01, 0x05, 0x01, 0x00})) {
    this->log_("Received Timeout from device");
  } else if (n >= 4 && d[0] == 0x12 && d[1] == 0 && d[2] == 0 && d[3] == 0) {
    this->finish_(false, Error::ACTIVATION_FAILED, "Register Failed!");
  } else if (n >= 4 && d[0] == 0x11 && d[1] == 0 && d[2] == 0 && d[3] == 0) {
    this->log_("Register successful");
  }
}

void MiAuth::handle_19_login_(const uint8_t *d, size_t n) {
  if (this->state_ == 0 && eq(d, n, {0x00, 0x00, 0x01, 0x01})) {
    this->state_ = 1;
    std::vector<uint8_t> v = {0x01, 0x00};
    v.insert(v.end(), this->rand_host_, this->rand_host_ + 16);
    this->write19v_(v);
  } else if (this->state_ == 1 && eq(d, n, {0x00, 0x00, 0x00, 0x0d, 0x01, 0x00})) {
    this->state_ = 2;
    this->write19_({0x00, 0x00, 0x01, 0x01});
  } else if (this->state_ == 1 && starts(d, n, {0x00, 0x00, 0x02, 0x0d}) && n >= 20) {
    // short variant: device random follows directly
    this->state_ = 12;
    memcpy(this->rand_dev_, d + 4, 16);
    login_generate(this->keys_.token, this->rand_host_, this->rand_dev_, this->expected_, this->send_info_);
    this->write19_({0x00, 0x00, 0x03, 0x00});
  } else if (this->state_ == 2 && starts(d, n, {0x01, 0x00}) && n >= 18) {
    this->state_ = 3;
    memcpy(this->rand_dev_, d + 2, 16);
    login_generate(this->keys_.token, this->rand_host_, this->rand_dev_, this->expected_, this->send_info_);
    this->write19_({0x00, 0x00, 0x01, 0x00});
  } else if (this->state_ == 3 && eq(d, n, {0x00, 0x00, 0x00, 0x0c, 0x02, 0x00})) {
    this->state_ = 4;
    this->write19_({0x00, 0x00, 0x01, 0x01});
  } else if (this->state_ == 12 && starts(d, n, {0x00, 0x00, 0x02, 0x0c})) {
    this->state_ = 13;
    this->dev_info_recv_.assign(d + 4, d + n);
    this->write19_({0x00, 0x00, 0x03, 0x00});
    if (this->dev_info_recv_.size() == 32 && memcmp(this->dev_info_recv_.data(), this->expected_, 32) == 0) {
      this->log_("Received device infos are correct");
      this->write19_({0x00, 0x00, 0x00, 0x0a, 0x01, 0x00});
    } else {
      this->log_("Received device infos are not correct");
      this->state_ = 0;
      this->write19_({0x00, 0x00, 0x01, 0x00});
    }
  } else if (this->state_ == 4 && starts(d, n, {0x01, 0x00})) {
    this->state_ = 5;
    this->dev_info_recv_.assign(d + 2, d + n);
  } else if (this->state_ == 5 && starts(d, n, {0x02, 0x00})) {
    this->state_ = 6;
    this->dev_info_recv_.insert(this->dev_info_recv_.end(), d + 2, d + n);
    if (this->dev_info_recv_.size() == 32 && memcmp(this->dev_info_recv_.data(), this->expected_, 32) == 0)
      this->log_("Received device infos are correct");
    else
      this->log_("Received device infos are not correct");
    this->write19_({0x00, 0x00, 0x01, 0x00});
    this->write19_({0x00, 0x00, 0x00, 0x0a, 0x02, 0x00});
  } else if (this->state_ == 6 && eq(d, n, {0x00, 0x00, 0x01, 0x01})) {
    this->state_ = 7;
    std::vector<uint8_t> f1 = {0x01, 0x00};
    f1.insert(f1.end(), this->send_info_, this->send_info_ + 18);
    std::vector<uint8_t> f2 = {0x02, 0x00};
    f2.insert(f2.end(), this->send_info_ + 18, this->send_info_ + 32);
    this->write19v_(f1);
    this->write19v_(f2);
  } else if (this->state_ == 13 && eq(d, n, {0x00, 0x00, 0x01, 0x01})) {
    this->state_ = 14;
    std::vector<uint8_t> f = {0x01, 0x00};
    f.insert(f.end(), this->send_info_, this->send_info_ + 32);
    this->write19v_(f);
  } else if (this->state_ == 14 && eq(d, n, {0x00, 0x00, 0x01, 0x00})) {
    this->state_ = 0;
    this->log_("Waiting for terminating code ...");
  }
}

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
