#include "xiaomi_esp_flasher.h"
#ifdef USE_ESP32
#include <cstdarg>
#include <cstring>
#include <ctime>
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/time.h"
#include "esphome/components/esp32_ble/ble_uuid.h"
#include <esp_gatt_defs.h>
#include <esp_gattc_api.h>
#include <esp_system.h>

namespace esphome {
namespace xiaomi_esp_flasher {

static const char *const TAG = "xiaomi_flasher";

static const char *const UUID_OTA_SERVICE = "00010203-0405-0607-0809-0a0b0c0d1912";
static const char *const UUID_OTA_CHAR = "00010203-0405-0607-0809-0a0b0c0d2b12";
static const char *const UUID_MI_MAIN = "ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6";
static const char *const UUID_MI_SPEED = "ebe0ccd8-7a0a-4b0c-8a1a-6ff2997da3a6";
static const char *const UUID_MI_TEMP = "ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6";
static const uint16_t DIS_CHARS[6] = {0x2A24, 0x2A25, 0x2A26, 0x2A27, 0x2A28, 0x2A29};

const char *session_state_name(SessionState s) {
  static const char *const N[] = {"IDLE", "SCAN", "CONNECTING", "CONNECTED", "IDENTIFYING", "ACTIVATING", "READY",
                                  "ERASING", "WRITING", "VERIFYING", "REBOOTING", "RECONNECTING", "SUCCESS", "ERROR"};
  return N[(uint8_t) s];
}
const char *job_type_name(JobType t) {
  static const char *const N[] = {"none", "identify", "read_config", "write_config", "set_defaults", "set_time",
                                  "set_name", "set_pin", "reboot", "activate", "flash", "connect"};
  return N[(uint8_t) t];
}

static std::string hexs(const uint8_t *d, size_t n) {
  static const char *H = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += H[d[i] >> 4]; s += H[d[i] & 15]; }
  return s;
}
static std::string clean_str(const uint8_t *d, size_t n) {
  std::string s;
  for (size_t i = 0; i < n && d[i] != 0; i++)
    if (d[i] >= 0x20 && d[i] < 0x7f)
      s += (char) d[i];
  return s;
}

// ------------------------------------------------------------------------------------------------ logging

void XiaomiEspFlasher::log(const std::string &line) {
  ESP_LOGI(TAG, "%s", line.c_str());
  LockGuard g(this->mutex_);
  this->log_.push_back(LogLine{millis(), this->epoch_now_(), ++this->log_seq_, line});
  while (this->log_.size() > 100)
    this->log_.pop_front();
  json::JsonBuilder b;
  JsonObject o = b.root();
  o["type"] = "log";
  o["seq"] = this->log_seq_;
  o["t"] = this->epoch_now_();
  o["msg"] = line;
  auto s = b.serialize();
  this->events_.push_back(std::string(s.c_str(), s.size()));
  while (this->events_.size() > 40)
    this->events_.pop_front();
}

void XiaomiEspFlasher::logf(const char *fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  this->log(buf);
}

uint32_t XiaomiEspFlasher::epoch_now_() const {
  time_t t = ::time(nullptr);
  return t > 1600000000 ? (uint32_t) t : 0;
}

std::string XiaomiEspFlasher::epoch_str_(uint32_t epoch) const {
  if (epoch == 0)
    return "";
  time_t t = epoch;
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
  return buf;
}

void XiaomiEspFlasher::emit_event_(const std::string &json) {
  LockGuard g(this->mutex_);
  this->events_.push_back(json);
  while (this->events_.size() > 60)
    this->events_.pop_front();
}

// ------------------------------------------------------------------------------------------------ setup / loop

void XiaomiEspFlasher::add_known_device(uint64_t mac, const std::string &alias) {
  this->configured_aliases_.emplace_back(mac, alias);
}

struct KnownRec {
  uint64_t mac;
  char alias[24];
  int8_t hw_id;
  uint8_t kind;
  uint8_t fw_ver_byte;
  uint8_t addr_type;
  char hw_str[8];
  char fw_str[16];
  char model[12];
  char ota_result[32];
  uint32_t last_ota_epoch;
  uint32_t identified_epoch;
} __attribute__((packed));
static constexpr size_t MAX_KNOWN = 12;
struct KnownBlob {
  uint8_t count;
  KnownRec rec[MAX_KNOWN];
} __attribute__((packed));

void XiaomiEspFlasher::load_known_devices_() {
  this->pref_ = global_preferences->make_preference<KnownBlob>(fnv1_hash("xiaomi_esp_flasher_known_v1"));
  KnownBlob blob{};
  if (!this->pref_.load(&blob))
    return;
  if (blob.count > MAX_KNOWN)
    blob.count = MAX_KNOWN;
  for (uint8_t i = 0; i < blob.count; i++) {
    KnownRec &r = blob.rec[i];
    if (r.mac == 0)
      continue;
    char macs[18];
    uint8_t m[6];
    for (int j = 0; j < 6; j++) m[j] = (r.mac >> (8 * (5 - j))) & 0xff;
    snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    XiaomiDevice *d = this->get_or_create_device_(r.mac, macs, r.addr_type);
    r.alias[sizeof(r.alias) - 1] = 0; r.hw_str[sizeof(r.hw_str) - 1] = 0; r.fw_str[sizeof(r.fw_str) - 1] = 0;
    r.model[sizeof(r.model) - 1] = 0; r.ota_result[sizeof(r.ota_result) - 1] = 0;
    if (d->alias.empty())
      d->alias = r.alias;
    d->hw.hw_id = r.hw_id;
    d->hw.kind = (FirmwareKind) r.kind;
    d->hw.fw_ver_byte = r.fw_ver_byte;
    d->hw.hw_string = r.hw_str;
    d->hw.fw_version = r.fw_str;
    d->hw.model = r.model;
    d->cfg.ver = r.fw_ver_byte;
    d->cfg.valid = r.fw_ver_byte != 0;
    d->ota_result = r.ota_result;
    d->last_ota_epoch = r.last_ota_epoch;
    d->identified_epoch = r.identified_epoch;
    d->identified = r.hw_id != HW_UNKNOWN || r.kind != 0;
    d->status = DeviceStatus::OFFLINE;
    d->user_added = true;
    this->recompute_eligibility_(*d);
  }
  ESP_LOGI(TAG, "loaded %u known devices", blob.count);
}

void XiaomiEspFlasher::save_known_devices_() {
  KnownBlob blob{};
  for (auto &up : this->devices_) {
    XiaomiDevice &d = *up;
    if (!(d.identified || d.user_added || !d.alias.empty()))
      continue;
    if (blob.count >= MAX_KNOWN)
      break;
    KnownRec &r = blob.rec[blob.count++];
    r.mac = d.address;
    strncpy(r.alias, d.alias.c_str(), sizeof(r.alias) - 1);
    r.hw_id = (int8_t) d.hw.hw_id;
    r.kind = (uint8_t) d.hw.kind;
    r.fw_ver_byte = d.hw.fw_ver_byte;
    r.addr_type = d.addr_type;
    strncpy(r.hw_str, d.hw.hw_string.c_str(), sizeof(r.hw_str) - 1);
    strncpy(r.fw_str, d.hw.fw_version.c_str(), sizeof(r.fw_str) - 1);
    strncpy(r.model, d.hw.model.c_str(), sizeof(r.model) - 1);
    strncpy(r.ota_result, d.ota_result.c_str(), sizeof(r.ota_result) - 1);
    r.last_ota_epoch = d.last_ota_epoch;
    r.identified_epoch = d.identified_epoch;
  }
  this->pref_.save(&blob);
}

void XiaomiEspFlasher::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Xiaomi ESP Flasher");
  this->store_.init();
  this->provider_ = std::make_unique<LocalFirmwareProvider>(this->manifest_json_, this->bundled_, &this->store_);
  this->provider_->setup();
  this->load_manifest_pref_();
  this->load_known_devices_();
  for (auto &p : this->configured_aliases_) {
    char macs[18];
    uint8_t m[6];
    for (int j = 0; j < 6; j++) m[j] = (p.first >> (8 * (5 - j))) & 0xff;
    snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    XiaomiDevice *d = this->get_or_create_device_(p.first, macs, BLE_ADDR_TYPE_PUBLIC);
    d->alias = p.second;
    d->user_added = true;
    if (d->status == DeviceStatus::DISCOVERED)
      d->status = DeviceStatus::OFFLINE;
  }
  if (this->client_ != nullptr) {
    this->client_->set_notify_callback([this](uint16_t h, const uint8_t *d, size_t n) { this->on_notify_(h, d, n); });
    this->client_->set_connect_callback([this](bool ok, int reason) { this->on_connected_(ok, reason); });
  }
  this->base_->init();
  this->base_->add_handler(this);
  std::string rep;
  bool st = MiAuth::self_test(rep);
  ESP_LOGD(TAG, "MiAuth self test %s: %s", st ? "ok" : "FAILED", rep.c_str());
  this->logf("Xiaomi ESP Flasher ready (reset reason %d, free heap %u)", (int) esp_reset_reason(), (unsigned) esp_get_free_heap_size());
  this->publish_global_();
}

void XiaomiEspFlasher::dump_config() {
  ESP_LOGCONFIG(TAG, "Xiaomi ESP Flasher:\n  known devices: %u\n  bundled images: %u\n  fwstore: %s\n  auto identify: %s",
                (unsigned) this->devices_.size(), (unsigned) this->bundled_.size(),
                this->store_.available() ? "available" : "missing", YESNO(this->auto_identify_));
  if (this->test_mac_) {
    ESP_LOGCONFIG(TAG, "  test device: %012llX", (unsigned long long) this->test_mac_);
  }
}

XiaomiDevice *XiaomiEspFlasher::find_device(uint64_t mac) {
  for (auto &d : this->devices_)
    if (d->address == mac)
      return d.get();
  return nullptr;
}
const XiaomiDevice *XiaomiEspFlasher::find_device(uint64_t mac) const {
  for (auto &d : this->devices_)
    if (d->address == mac)
      return d.get();
  return nullptr;
}

XiaomiDevice *XiaomiEspFlasher::get_or_create_device_(uint64_t mac, const char *mac_str, uint8_t addr_type) {
  XiaomiDevice *d = this->find_device(mac);
  if (d != nullptr)
    return d;
  auto nd = std::make_unique<XiaomiDevice>();
  nd->address = mac;
  strncpy(nd->mac, mac_str, sizeof(nd->mac) - 1);
  nd->addr_type = addr_type;
  nd->hw.hw_id = HW_UNKNOWN;
  d = nd.get();
  {
    LockGuard g(this->mutex_);
    this->devices_.push_back(std::move(nd));
  }
  return d;
}

// ------------------------------------------------------------------------------------------------ scanning

bool XiaomiEspFlasher::parse_device(const espbt::ESPBTDevice &device) {
  // Only Telink/Xiaomi-looking devices: name prefixes or the relevant service data UUIDs
  bool interesting = false;
  AdvMeasurement meas;
  std::string model_hint, fw_hint;
  for (auto &sd : device.get_service_datas()) {
    if (sd.uuid.type() != espbt::ESPBTUUID::Type::UUID16)
      continue;
    uint16_t u = sd.uuid.uuid16();
    if (u == 0xFCD2 || u == 0x181A || u == 0x181C || u == 0xFE95) {
      AdvMeasurement m;
      if (parse_advertisement(u, sd.data.data(), sd.data.size(), m, model_hint, fw_hint) || m.format != AdvFormat::NONE) {
        interesting = true;
        if (m.valid || !meas.valid)
          meas = m;
      }
    }
  }
  StringRef nm = device.get_name();
  std::string name(nm.c_str(), nm.size());
  if (!interesting) {
    static const char *const PREFIXES[] = {"ATC_", "LYWSD03MMC", "LYWSD02", "MHO-C401", "MJWSD05MMC", "MJWSD06MMC",
                                           "CGG1", "CGDK2", "Qingping", "C121", "TH0", "ZTH0", "DEV_"};
    for (const char *p : PREFIXES)
      if (name.rfind(p, 0) == 0)
        interesting = true;
  }
  if (!interesting && device.address_uint64() != this->test_mac_ && this->find_device(device.address_uint64()) == nullptr)
    return false;
  char macs[espbt::ESPBTDevice::MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  device.address_str_to(macs);
  XiaomiDevice *d = this->get_or_create_device_(device.address_uint64(), macs, device.get_address_type());
  bool first = d->last_seen_ms == 0;
  {
    LockGuard g(this->mutex_);
    d->addr_type = device.get_address_type();
    if (!name.empty())
      d->name = name;
    d->rssi = device.get_rssi();
    d->last_seen_ms = millis();
    d->last_seen_epoch = this->epoch_now_();
    if (meas.valid || meas.format != AdvFormat::NONE) {
      if (meas.valid)
        d->adv = meas;
      else
        d->adv.format = meas.format;
    }
    if (!model_hint.empty())
      d->model_hint = model_hint;
    if (d->status == DeviceStatus::OFFLINE)
      d->status = d->identified ? DeviceStatus::IDENTIFIED : DeviceStatus::DISCOVERED;
  }
  if (first) {
    this->logf("Found %s %s rssi %d (%s)", macs, name.c_str(), device.get_rssi(), adv_format_name(d->adv.format));
    this->needs_global_publish_ = true;
  }
  this->publish_device_(*d, true);
  return false;  // never claim the advertisement, other listeners may want it
}

// ------------------------------------------------------------------------------------------------ eligibility / entities

void XiaomiEspFlasher::recompute_eligibility_(XiaomiDevice &d) {
  d.update_available = false;
  d.latest_version.clear();
  d.latest_firmware_id.clear();
  d.compat = CompatResult{};
  if (!d.identified || this->provider_ == nullptr)
    return;
  FirmwareInfo fw;
  if (!this->provider_->get_latest_for_device(d.hw, fw)) {
    d.compat.ok = false;
    d.compat.code = d.hw.hw_id == HW_UNKNOWN ? Error::HW_UNKNOWN : Error::INCOMPATIBLE_FIRMWARE;
    d.compat.message = d.hw.hw_id == HW_UNKNOWN ? "Unknown / unsupported hardware revision. Firmware update blocked for safety."
                                                : "No firmware image is listed for this hardware.";
    return;
  }
  d.latest_version = fw.version;
  d.latest_firmware_id = fw.id;
  d.compat = check_compatibility(d.hw, fw);
  bool hw_ok = d.compat.ok;  // hardware/firmware match, independent of whether the bytes are on the ESP32 yet
  if (fw.size == 0) {
    d.compat.ok = false;
    d.compat.code = Error::NOT_ENOUGH_STORAGE;
    d.compat.message = "Firmware " + fw.name + " v" + fw.version + " is listed for this hardware but not stored on the ESP32 yet – download it on the Firmware page (browser fetches it from GitHub).";
  }
  // "update available" reflects versions (what Home Assistant needs to know); flashing is still gated by compat.ok
  if (d.hw.kind == FirmwareKind::CUSTOM_PVVX) {
    d.update_available = hw_ok && compare_versions(d.installed_version(), fw.version) < 0;
  } else if (d.hw.kind == FirmwareKind::STOCK_XIAOMI || d.hw.kind == FirmwareKind::ATC1441) {
    d.update_available = hw_ok;  // conversion to custom firmware
  }
}

void XiaomiEspFlasher::publish_device_(XiaomiDevice &d, bool measurements_only) {
  for (auto &e : this->entities_) {
    if (e.mac != d.address)
      continue;
#ifdef USE_SENSOR
    if (d.adv.valid) {
      if (e.temperature && !std::isnan(d.adv.temperature)) e.temperature->publish_state(d.adv.temperature);
      if (e.humidity && !std::isnan(d.adv.humidity)) e.humidity->publish_state(d.adv.humidity);
      if (e.battery && !std::isnan(d.adv.battery_pct)) e.battery->publish_state(d.adv.battery_pct);
      if (e.battery_voltage && !std::isnan(d.adv.battery_mv)) e.battery_voltage->publish_state(d.adv.battery_mv / 1000.0f);
    } else if (d.live.valid) {
      if (e.temperature) e.temperature->publish_state(d.live.temp / 100.0f);
      if (e.humidity) e.humidity->publish_state(d.live.humi / 100.0f);
      if (e.battery_voltage && d.live.vbat_mv) e.battery_voltage->publish_state(d.live.vbat_mv / 1000.0f);
    }
    if (e.rssi && d.last_seen_ms) e.rssi->publish_state(d.rssi);
#endif
#ifdef USE_TEXT_SENSOR
    if (e.last_seen && d.last_seen_epoch) {
      std::string s = this->epoch_str_(d.last_seen_epoch);
      if (!e.last_seen->has_state() || e.last_seen->get_state() != s) e.last_seen->publish_state(s);
    }
    if (measurements_only)
      continue;
    auto pub = [](text_sensor::TextSensor *ts, const std::string &v) {
      if (ts && (!ts->has_state() || ts->get_state() != v)) ts->publish_state(v);
    };
    pub(e.firmware, d.installed_version().empty() ? "unknown" : (std::string(firmware_kind_name(d.hw.kind)) + " " + d.installed_version()));
    pub(e.hardware, d.hw.hw_string.empty() ? (d.hw.hw_id == HW_UNKNOWN ? "unknown" : hw_id_name(d.hw.hw_id)) : d.hw.hw_string);
    pub(e.model, d.hw.model.empty() ? (d.model_hint.empty() ? d.name : d.model_hint) : d.hw.model);
    pub(e.status, device_status_name(d.status));
    pub(e.device_name, d.device_name.empty() ? d.name : d.device_name);
    std::string us;
    if (!d.identified) us = "not identified";
    else if (d.update_available) us = "Update available: " + d.installed_version() + " -> " + d.latest_version;
    else if (d.compat.ok) us = "Up-to-date";
    else us = std::string("Unsupported firmware target (") + error_to_string(d.compat.code) + ")";
    pub(e.update_status, us);
#else
    if (measurements_only)
      continue;
#endif
#ifdef USE_BINARY_SENSOR
    if (e.update_available) e.update_available->publish_state(d.update_available);
#endif
#ifdef USE_UPDATE
    if (e.update) {
      bool installing = this->dev_ == &d && this->session_active() && this->job_.type == JobType::FLASH;
      std::string title = d.hw.model.empty() ? "pvvx ATC firmware" : (d.hw.model + " pvvx firmware");
      std::string summary = d.identified ? (d.compat.ok ? d.compat.message : d.compat.message) : "Device not identified yet – press Check";
      if (d.hw.kind == FirmwareKind::STOCK_XIAOMI && d.compat.ok)
        summary = "Conversion from original Xiaomi firmware: Mi activation will be performed (device leaves Mi Home).";
      e.update->set_info(d.installed_version().empty() ? "unknown" : d.installed_version(),
                         d.latest_version.empty() ? (d.installed_version().empty() ? "unknown" : d.installed_version()) : d.latest_version,
                         title, summary, d.update_available, installing,
                         installing ? this->ota_.progress().percent() : 0.0f, installing);
    }
#endif
  }
}

void XiaomiEspFlasher::publish_global_() {
  this->needs_global_publish_ = false;
  this->last_global_publish_ms_ = millis();
#ifdef USE_TEXT_SENSOR
  if (this->status_sensor_) {
    std::string s = session_state_name(this->state_);
    if (this->session_active() && this->dev_)
      s += std::string(" ") + job_type_name(this->job_.type) + " " + this->dev_->mac;
    if (!this->status_sensor_->has_state() || this->status_sensor_->get_state() != s)
      this->status_sensor_->publish_state(s);
  }
  if (this->last_error_sensor_) {
    std::string s = this->last_error_ == Error::NONE ? "none" : std::string(error_to_string(this->last_error_)) + ": " + this->last_error_msg_;
    if (!this->last_error_sensor_->has_state() || this->last_error_sensor_->get_state() != s)
      this->last_error_sensor_->publish_state(s);
  }
#endif
#ifdef USE_SENSOR
  if (this->device_count_sensor_) this->device_count_sensor_->publish_state(this->devices_.size());
  if (this->ota_progress_sensor_) this->ota_progress_sensor_->publish_state(this->ota_.active() ? this->ota_.progress().percent() : 0);
  if (this->updates_available_sensor_) {
    int n = 0;
    for (auto &d : this->devices_) if (d->update_available) n++;
    this->updates_available_sensor_->publish_state(n);
  }
#endif
#ifdef USE_BINARY_SENSOR
  if (this->ota_active_sensor_) this->ota_active_sensor_->publish_state(this->session_active() && this->job_.type == JobType::FLASH);
#endif
}

void XiaomiEspFlasher::emit_device_event_(const XiaomiDevice &d) {
  this->emit_event_("{\"type\":\"device\",\"mac\":\"" + std::string(d.mac) + "\"}");
}

// ------------------------------------------------------------------------------------------------ actions

bool XiaomiEspFlasher::parse_mac_(const std::string &s, uint64_t &mac) const {
  uint8_t m[6];
  if (s.size() == 17) {
    if (sscanf(s.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
      return false;
  } else if (s.size() == 12) {
    if (sscanf(s.c_str(), "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
      return false;
  } else {
    return false;
  }
  mac = 0;
  for (int i = 0; i < 6; i++)
    mac = (mac << 8) | m[i];
  return true;
}

bool XiaomiEspFlasher::request_job(const Job &job, std::string &err) {
  const XiaomiDevice *d = this->find_device(job.mac);
  if (d == nullptr) {
    err = "DEVICE_NOT_FOUND";
    return false;
  }
  if (this->session_active() && this->job_.type == JobType::CONNECT) {
    if (job.type == JobType::CONNECT && this->dev_ && this->dev_->address == job.mac)
      return true;  // already holding this device
    this->hold_release_ = true;  // the held link is closed first, then the queued job runs
  }
  {
    LockGuard g(this->mutex_);
    if (this->job_queue_.size() >= 8) {
      err = "BUSY";
      return false;
    }
    this->job_queue_.push_back(job);
  }
  this->enable_loop_soon_any_context();
  return true;
}

bool XiaomiEspFlasher::hold_active(uint64_t mac) const {
  return this->state_ == SessionState::READY && this->job_.type == JobType::CONNECT && this->dev_ != nullptr &&
         this->dev_->address == mac;
}

bool XiaomiEspFlasher::queue_raw_command(uint64_t mac, const std::vector<uint8_t> &payload, std::string &err) {
  if (!this->hold_active(mac)) {
    err = "DEVICE_DISCONNECTED";
    return false;
  }
  if (payload.empty() || payload.size() > 244) {
    err = "INVALID_REQUEST";
    return false;
  }
  LockGuard g(this->mutex_);
  if (this->cmd_queue_.size() >= 16) {
    err = "BUSY";
    return false;
  }
  this->cmd_queue_.push_back(payload);
  this->hold_last_activity_ = millis();
  return true;
}

void XiaomiEspFlasher::load_manifest_pref_() {
  struct Blob { uint16_t len; char json[3070]; } __attribute__((packed));
  this->manifest_pref_ = global_preferences->make_preference<Blob>(fnv1_hash("xiaomi_esp_flasher_manifest_v1"));
  auto *b = new Blob();
  if (this->manifest_pref_.load(b) && b->len > 10 && b->len <= sizeof(b->json)) {
    std::string j(b->json, b->len), err;
    if (this->provider_->set_manifest(j, err))
      this->runtime_manifest_ = j;
    else
      ESP_LOGW(TAG, "stored manifest ignored: %s", err.c_str());
  }
  delete b;
}

bool XiaomiEspFlasher::save_manifest_pref_(const std::string &json) {
  struct Blob { uint16_t len; char json[3070]; } __attribute__((packed));
  if (json.size() > sizeof(Blob::json))
    return false;
  auto *b = new Blob();
  memset(b, 0, sizeof(Blob));
  b->len = json.size();
  memcpy(b->json, json.data(), json.size());
  bool ok = this->manifest_pref_.save(b);
  delete b;
  if (ok) global_preferences->sync();
  return ok;
}

bool XiaomiEspFlasher::request_identify(uint64_t mac, bool from_ha) {
  Job j;
  j.type = JobType::IDENTIFY;
  j.mac = mac;
  j.from_ha = from_ha;
  std::string err;
  return this->request_job(j, err);
}

bool XiaomiEspFlasher::request_flash(uint64_t mac, const std::string &fw_id, bool from_ha) {
  Job j;
  j.type = JobType::FLASH;
  j.mac = mac;
  j.firmware_id = fw_id;
  j.from_ha = from_ha;
  std::string err;
  return this->request_job(j, err);
}

void XiaomiEspFlasher::request_scan() { this->scan_requested_ = true; this->enable_loop_soon_any_context(); }
void XiaomiEspFlasher::request_update_all() { this->update_all_requested_ = true; this->enable_loop_soon_any_context(); }
void XiaomiEspFlasher::request_check_online() { this->check_online_requested_ = true; this->enable_loop_soon_any_context(); }

void XiaomiEspFlasher::forget_device(uint64_t mac) {
  LockGuard g(this->mutex_);
  for (auto it = this->devices_.begin(); it != this->devices_.end(); ++it) {
    if ((*it)->address == mac && this->dev_ != it->get()) {
      this->devices_.erase(it);
      break;
    }
  }
}

void XiaomiEspFlasher::set_alias(uint64_t mac, const std::string &alias) {
  XiaomiDevice *d = this->find_device(mac);
  if (d == nullptr)
    return;
  {
    LockGuard g(this->mutex_);
    d->alias = alias;
    d->user_added = true;
  }
  this->save_known_devices_();
}

// ------------------------------------------------------------------------------------------------ main loop

void XiaomiEspFlasher::loop() {
  uint32_t now = millis();
  this->ota_.loop();
  this->mi_auth_.loop();
  this->sse_pump_();

  // custom command timeout
  if (this->cmd_waiting_ && (int32_t) (now - this->cmd_deadline_) > 0) {
    this->cmd_waiting_ = false;
    auto cb = std::move(this->cmd_cb_);
    this->cmd_cb_ = nullptr;
    if (cb)
      cb(false, nullptr, 0);
  }
  // step timeout
  if (this->session_active() && this->step_deadline_ && (int32_t) (now - this->step_deadline_) > 0) {
    this->step_deadline_ = 0;
    if (this->state_ == SessionState::RECONNECTING || this->state_ == SessionState::REBOOTING) {
      if (++this->verify_attempts_ <= 6) {
        this->logf("Reconnect attempt %u failed, retrying", this->verify_attempts_);
        if (this->client_ && !this->client_->is_idle())
          this->client_->end_connection();
        this->reconnect_at_ = now + 5000;
        this->arm_step_timeout_(30000);
        this->set_state_(SessionState::REBOOTING);
      } else {
        this->fail_session_(Error::OTA_VERIFY_FAILED, "device did not come back after OTA");
      }
    } else {
      this->fail_session_(Error::BLE_TIMEOUT, std::string("timeout in state ") + session_state_name(this->state_));
    }
  }
  // fallback: the device did not drop the link 25 s after the end packet -> disconnect from our side
  if (this->state_ == SessionState::REBOOTING && this->reconnect_at_ == 0 && this->ota_end_ms_ &&
      (now - this->ota_end_ms_) > 25000 && this->client_ && !this->client_->is_idle()) {
    this->log("Device kept the link 25 s after OTA end, disconnecting to verify");
    this->ota_end_ms_ = 0;
    this->client_->end_connection();
  }
  // delayed reconnect for verification
  if (this->state_ == SessionState::REBOOTING && this->reconnect_at_ && (int32_t) (now - this->reconnect_at_) >= 0) {
    this->reconnect_at_ = 0;
    this->step_verify_reconnect_();
  }

  if (this->scan_requested_.exchange(false)) {
    this->last_scan_epoch_ = this->epoch_now_();
    this->log("BLE scan started");
    if (this->parent_ != nullptr && !this->parent_->scan_running() && !this->session_active())
      this->parent_->start_scan();
    this->needs_global_publish_ = true;
  }
  if (this->upload_.finish_requested.exchange(false)) {
    Upload &u = this->upload_;
    std::string err;
    u.ok = this->store_.finish_write(u.name, u.version, u.kind, u.hw_ids, u.source, err);
    u.err = err;
    if (u.ok) {
      StoredImage img;
      FirmwareInfo fi;
      if (this->store_.find(u.name, img)) this->store_.get_info(img, fi);
      u.result_id = fi.id;
      u.result_size = fi.size;
      u.result_crc = fi.crc32;
      this->logf("Firmware %s v%s (%u bytes) stored on the ESP32", u.name.c_str(), u.version.c_str(), (unsigned) u.total);
      this->recompute_requested_ = true;
    } else {
      this->logf("Upload of %s rejected: %s", u.name.c_str(), err.c_str());
    }
    u.done = true;
  }
  if (this->recompute_requested_.exchange(false)) {
    for (auto &d : this->devices_) { this->recompute_eligibility_(*d); this->publish_device_(*d, false); }
    this->needs_global_publish_ = true;
  }
  if (this->update_all_requested_.exchange(false)) {
    LockGuard g(this->mutex_);
    for (auto &d : this->devices_)
      if (d->update_available && d->compat.ok)
        this->update_queue_.push_back(d->address);
    this->logf("Update queue: %u device(s)", (unsigned) this->update_queue_.size());
  }
#ifdef USE_XIAOMI_FLASHER_REMOTE
  if (this->check_online_requested_.exchange(false) && !this->session_active()) {
    this->log("Fetching firmware manifest from GitHub...");
    RemoteGithubFirmwareProvider remote(this->remote_url_);
    std::string json, err;
    if (!remote.fetch_manifest(json, err)) {
      this->log("Manifest fetch failed: " + err);
      this->last_error_ = Error::DOWNLOAD_FAILED;
      this->last_error_msg_ = err;
    } else {
      std::vector<ManifestEntry> entries;
      if (!parse_manifest(json, entries, err)) {
        this->log("Manifest parse failed: " + err);
      } else {
        std::string base = this->remote_url_.substr(0, this->remote_url_.find_last_of('/') + 1);
        this->provider_->set_remote_entries(entries, base);
        this->logf("Online manifest: %u entries", (unsigned) entries.size());
        // download the custom image for every identified device hardware that is not bundled
        for (auto &e : entries) {
          if (e.kind != ImageKind::CUSTOM) continue;
          bool needed = false;
          for (auto &d : this->devices_)
            if (d->identified)
              for (int id : e.hw_ids)
                if (id == d->hw.hw_id) needed = true;
          FirmwareInfo fi;
          bool have = this->provider_->get_firmware_info("bundled:" + e.name, fi) && fi.size != 0;
          if (have && compare_versions(fi.version, e.version) >= 0) needed = false;
          if (needed) {
            std::string url = e.file.rfind("https:", 0) == 0 ? e.file : base + e.file;
            this->log("Downloading " + e.name + " ...");
            size_t last = 0;
            bool ok = remote.download_image(url, this->store_, e, err, [&](size_t r, size_t t) {
              if (r - last > 16384) { last = r; App.feed_wdt(); }
            });
            this->log(ok ? "Downloaded " + e.name + " into the firmware store" : "Download failed: " + err);
            break;  // one image per check
          }
        }
      }
    }
    for (auto &d : this->devices_) { this->recompute_eligibility_(*d); this->publish_device_(*d, false); }
    this->needs_global_publish_ = true;
  }
#else
  if (this->check_online_requested_.exchange(false))
    this->log("Online firmware check is disabled in this build (remote_manifest not configured)");
#endif

  // raw command bridge while a CONNECT job holds the link
  if (this->state_ == SessionState::READY && this->job_.type == JobType::CONNECT && this->dev_ != nullptr) {
    if (this->hold_release_) {
      this->hold_release_ = false;
      this->log("Disconnect requested");
      this->disconnect_and_finish_(true, "disconnected");
    } else if (now - this->hold_last_activity_ > 300000) {
      this->log("Connection idle for 5 min, disconnecting");
      this->disconnect_and_finish_(true, "idle timeout");
    } else if (this->client_ && !this->client_->busy()) {
      std::vector<uint8_t> cmd;
      bool have = false;
      {
        LockGuard g(this->mutex_);
        if (!this->cmd_queue_.empty()) { cmd = std::move(this->cmd_queue_.front()); this->cmd_queue_.pop_front(); have = true; }
      }
      if (have) {
        if (this->ch_custom_.handle == 0) {
          this->log("No 0x1F1F characteristic on this device – command dropped");
        } else {
          ESP_LOGD(TAG, "-> 1f1f %s", hexs(cmd.data(), cmd.size()).c_str());
          if (cmd[0] == CMD_ID_CFG || cmd[0] == CMD_ID_CFG_DEF || cmd[0] == CMD_ID_COMFORT || cmd[0] == CMD_ID_TRG ||
              cmd[0] == CMD_ID_CFS || cmd[0] == CMD_ID_DNAME || cmd[0] == CMD_ID_UTC_TIME)
            this->logf("Send %s", hexs(cmd.data(), cmd.size()).c_str());
          bool rsp = this->ch_custom_.props & ESP_GATT_CHAR_PROP_BIT_WRITE;
          auto data = std::make_shared<std::vector<uint8_t>>(std::move(cmd));
          this->client_->write(this->ch_custom_.handle, data->data(), data->size(), rsp, [this, data](int st) {
            if (st != ESP_GATT_OK) this->logf("Write of %02X failed (%d)", (*data)[0], st);
          });
        }
      }
    }
  }
  // start next job
  if (!this->session_active()) {
    Job next;
    bool have = false;
    {
      LockGuard g(this->mutex_);
      if (!this->job_queue_.empty()) {
        next = this->job_queue_.front();
        this->job_queue_.pop_front();
        have = true;
      } else if (!this->update_queue_.empty() && (now - this->session_started_) > 3000) {
        next.type = JobType::FLASH;
        next.mac = this->update_queue_.front();
        this->update_queue_.pop_front();
        have = true;
      }
    }
    if (!have && this->auto_identify_ && (now - this->last_auto_identify_ms_) > 20000 && now > 30000) {
      // identify one not-yet-identified (or stale) device that is currently in range
      for (auto &d : this->devices_) {
        bool in_range = d->last_seen_ms && (now - d->last_seen_ms) < 60000 && d->rssi > -95;  // weak links: 3x20 s timeouts
        bool stale = d->identified && this->identify_interval_s_ && d->identified_epoch &&
                     this->epoch_now_() > d->identified_epoch + this->identify_interval_s_;
        if (in_range && (!d->identified || stale) && d->status != DeviceStatus::FAILED) {
          next.type = JobType::IDENTIFY;
          next.mac = d->address;
          have = true;
          break;
        }
      }
      this->last_auto_identify_ms_ = now;
    }
    if (have)
      this->start_job_(next);
  }

  // mark devices offline
  for (auto &d : this->devices_) {
    if (d->last_seen_ms && (now - d->last_seen_ms) > 600000 && d->status != DeviceStatus::OFFLINE && this->dev_ != d.get()) {
      d->status = DeviceStatus::OFFLINE;
      this->publish_device_(*d, false);
    }
  }
  if (this->needs_global_publish_ || (now - this->last_global_publish_ms_) > 30000)
    this->publish_global_();
}

// ------------------------------------------------------------------------------------------------ session core

void XiaomiEspFlasher::set_state_(SessionState s) {
  if (this->state_ == s)
    return;
  this->state_ = s;
  ESP_LOGD(TAG, "session state -> %s", session_state_name(s));
  this->emit_event_(std::string("{\"type\":\"state\",\"state\":\"") + session_state_name(s) + "\",\"job\":\"" +
                    job_type_name(this->job_.type) + "\",\"mac\":\"" + (this->dev_ ? this->dev_->mac : "") + "\"}");
  this->needs_global_publish_ = true;
}

void XiaomiEspFlasher::start_job_(const Job &job) {
  XiaomiDevice *d = this->find_device(job.mac);
  if (d == nullptr) {
    this->logf("Job %s: device not found", job_type_name(job.type));
    return;
  }
  if (this->client_ == nullptr || !this->client_->is_idle()) {
    this->log("BLE client busy, job postponed");
    LockGuard g(this->mutex_);
    this->job_queue_.push_front(job);
    return;
  }
  this->job_ = job;
  this->dev_ = d;
  this->session_started_ = millis();
  this->connect_attempts_ = 0;
  this->verify_attempts_ = 0;
  this->finish_msg_.clear();
  this->cmd_waiting_ = false;
  this->cmd_cb_ = nullptr;
  this->stock_temp_seen_ = false;
  d->set_error(Error::NONE, "");
  this->logf("Job %s for %s (%s)", job_type_name(job.type), d->mac, d->display_name().c_str());
  if (job.type == JobType::FLASH) {
    // Safety: validate the image before touching the device
    FirmwareInfo fw;
    std::string id = job.firmware_id.empty() ? d->latest_firmware_id : job.firmware_id;
    if (id.empty() || !this->provider_->get_firmware_info(id, fw)) {
      this->fail_session_(Error::INVALID_REQUEST, "firmware '" + id + "' not found");
      return;
    }
    if (!this->provider_->open_image(id, this->ota_reader_, this->ota_size_)) {
      this->fail_session_(Error::NOT_ENOUGH_STORAGE, "firmware '" + id + "' is not available locally");
      return;
    }
    TelinkImageInfo info;
    std::string r = validate_telink_image(this->ota_size_, this->ota_reader_, info, MAX_EXT_OTA_SIZE);
    if (r != "ok" && r != "sign") {
      this->fail_session_(Error::INVALID_IMAGE, r);
      return;
    }
    if (r == "sign") {
      this->fail_session_(Error::INCOMPATIBLE_FIRMWARE, "signed original images need the signature file – not supported");
      return;
    }
    this->ota_fw_ = fw;
    this->ota_fw_.size = this->ota_size_;
    this->ota_fw_.crc32 = info.crc_stored;
    this->target_version_ = fw.version;
    this->logf("Target firmware %s v%s (%u bytes, crc 0x%08X)", fw.name.c_str(), fw.version.c_str(),
               (unsigned) this->ota_size_, (unsigned) info.crc_stored);
    d->status = DeviceStatus::UPDATING;
  } else {
    d->status = DeviceStatus::CONNECTING;
  }
  this->hold_release_ = false;
  this->publish_device_(*d, false);
  this->step_connect_();
}

void XiaomiEspFlasher::step_connect_() {
  this->set_state_(SessionState::CONNECTING);
  this->connect_attempts_++;
  this->logf("Connecting to %s (attempt %u)", this->dev_->mac, this->connect_attempts_);
  this->disconnect_requested_ = false;
  if (!this->client_->begin_connect(this->dev_->address, (esp_ble_addr_type_t) this->dev_->addr_type)) {
    this->fail_session_(Error::INTERNAL_ERROR, "BLE client not idle");
    return;
  }
  this->arm_step_timeout_(35000);
}

void XiaomiEspFlasher::on_connected_(bool established, int reason) {
  if (!this->session_active() || this->dev_ == nullptr)
    return;
  if (established) {
    if (this->state_ == SessionState::RECONNECTING) {
      this->log("Reconnected");
      this->on_verify_connected_();
      return;
    }
    if (this->state_ != SessionState::CONNECTING)
      return;
    this->logf("Connected (MTU %u)", this->client_->mtu());
    this->set_state_(SessionState::CONNECTED);
    this->dev_->status = DeviceStatus::CONNECTED;
    this->step_discover_();
    return;
  }
  // disconnected / connect failed
  if (this->disconnect_requested_) {
    this->disconnect_requested_ = false;
    this->finish_session_(this->finish_ok_, this->finish_msg_);
    return;
  }
  if (this->state_ == SessionState::CONNECTING) {
    if (this->connect_attempts_ < 3) {
      this->logf("Connect failed (reason %d), retrying", reason);
      this->set_timeout("xf_reconnect", 2000, [this]() {
        if (this->state_ == SessionState::CONNECTING && this->client_->is_idle())
          this->step_connect_();
      });
      return;
    }
    this->fail_session_(Error::BLE_TIMEOUT, "connection failed (reason " + std::to_string(reason) + ")");
    return;
  }
  if (this->state_ == SessionState::WRITING || this->state_ == SessionState::ERASING) {
    this->fail_session_(Error::DEVICE_DISCONNECTED, "device disconnected during OTA");
    return;
  }
  if (this->state_ == SessionState::READY && this->job_.type == JobType::CONNECT) {
    this->finish_session_(true, "link closed by the device (reason " + std::to_string(reason) + ")");
    return;
  }
  if (this->state_ == SessionState::REBOOTING) {
    // expected: device reboots after OTA end – reconnect a few seconds later
    this->log("Device dropped the link (rebooting into the new firmware)");
    this->reconnect_at_ = millis() + 4000;
    return;
  }
  if (this->state_ == SessionState::RECONNECTING) {
    if (++this->verify_attempts_ <= 6) {
      this->reconnect_at_ = millis() + 5000;
      this->arm_step_timeout_(30000);
      this->set_state_(SessionState::REBOOTING);
    } else {
      this->fail_session_(Error::OTA_VERIFY_FAILED, "device did not come back after OTA");
    }
    return;
  }
  this->fail_session_(Error::DEVICE_DISCONNECTED, "device disconnected (reason " + std::to_string(reason) + ")");
}

void XiaomiEspFlasher::fail_session_(Error e, const std::string &msg) {
  this->logf("ERROR %s: %s", error_to_string(e), msg.c_str());
  this->last_error_ = e;
  this->last_error_msg_ = msg;
  if (this->dev_) {
    this->dev_->set_error(e, msg);
    this->dev_->status = DeviceStatus::FAILED;
    if (this->job_.type == JobType::FLASH) {
      this->dev_->ota_result = std::string("failed: ") + error_to_string(e);
      this->dev_->last_ota_epoch = this->epoch_now_();
      this->last_ota_epoch_ = this->dev_->last_ota_epoch;
      this->last_ota_result_ = this->dev_->ota_result + " (" + this->dev_->mac + ")";
      LockGuard g(this->mutex_);
      this->update_queue_.clear();  // never continue an update queue after a failure
    }
  }
  this->ota_.abort(msg);
  this->mi_auth_.abort(msg);
  this->cmd_waiting_ = false;
  this->cmd_cb_ = nullptr;
  this->step_deadline_ = 0;
  if (this->client_ && !this->client_->is_idle())
    this->client_->end_connection();
  this->set_state_(SessionState::ERROR);
  this->emit_event_(std::string("{\"type\":\"result\",\"ok\":false,\"error\":\"") + error_to_string(e) + "\",\"msg\":\"" +
                    json_escape(msg) + "\",\"mac\":\"" + (this->dev_ ? this->dev_->mac : "") + "\"}");
  if (this->dev_) {
    this->publish_device_(*this->dev_, false);
    this->emit_device_event_(*this->dev_);
    this->save_known_devices_();
  }
  this->dev_ = nullptr;
}

void XiaomiEspFlasher::disconnect_and_finish_(bool ok, const std::string &msg) {
  this->finish_ok_ = ok;
  this->finish_msg_ = msg;
  this->step_deadline_ = 0;
  if (this->client_ && !this->client_->is_idle()) {
    this->disconnect_requested_ = true;
    this->arm_step_timeout_(12000);
    this->client_->end_connection();
  } else {
    this->finish_session_(ok, msg);
  }
}

void XiaomiEspFlasher::finish_session_(bool ok, const std::string &msg) {
  this->step_deadline_ = 0;
  this->disconnect_requested_ = false;
  this->set_state_(ok ? SessionState::SUCCESS : SessionState::ERROR);
  if (this->dev_) {
    this->dev_->status = ok ? (this->job_.type == JobType::FLASH ? DeviceStatus::UPDATED : DeviceStatus::IDENTIFIED)
                            : DeviceStatus::FAILED;
    this->publish_device_(*this->dev_, false);
    this->emit_device_event_(*this->dev_);
  }
  this->logf("%s %s", ok ? "Done:" : "Finished with error:", msg.c_str());
  this->emit_event_(std::string("{\"type\":\"result\",\"ok\":") + (ok ? "true" : "false") + ",\"msg\":\"" +
                    json_escape(msg) + "\",\"mac\":\"" + (this->dev_ ? this->dev_->mac : "") + "\"}");
  this->save_known_devices_();
  this->dev_ = nullptr;
}

// ------------------------------------------------------------------------------------------------ discovery + identification

void XiaomiEspFlasher::step_discover_() {
  XiaomiDevice &d = *this->dev_;
  if (!this->verify_pending_) {
    this->set_state_(SessionState::IDENTIFYING);
    this->dev_->status = DeviceStatus::IDENTIFYING;
  }
  this->log("Service discovery");
  auto uuids = this->client_->service_uuids();
  {
    LockGuard g(this->mutex_);
    d.services.clear();
    for (auto &u : uuids) {
      char buf[espbt::UUID_STR_LEN];
      d.services.push_back(u.to_str(buf));
    }
  }
  bool ota = false, custom = false, mi = false, dis = false, env = false, fe95 = false;
  this->ch_ota_ = this->ch_custom_ = this->ch_mi10_ = this->ch_mi19_ = this->ch_mi_speed_ = this->ch_mi_temp_ = GattChar{};
  this->ch_env_temp_ = this->ch_env_humi_ = this->ch_batt_ = GattChar{};
  for (auto &s : this->dis_) s = GattChar{};
  auto U16 = [](uint16_t u) { return espbt::ESPBTUUID::from_uint16(u); };
  auto U128 = [](const char *s) { return espbt::ESPBTUUID::from_raw(s); };
  for (auto &u : uuids) {
    if (u == U128(UUID_OTA_SERVICE)) ota = true;
    else if (u == U16(0x1F10)) custom = true;
    else if (u == U128(UUID_MI_MAIN)) mi = true;
    else if (u == U16(0x180A)) dis = true;
    else if (u == U16(0x181A)) env = true;
    else if (u == U16(0xFE95)) fe95 = true;
  }
  if (ota) this->client_->find_characteristic(U128(UUID_OTA_SERVICE), U128(UUID_OTA_CHAR), this->ch_ota_);
  if (custom) this->client_->find_characteristic(U16(0x1F10), U16(0x1F1F), this->ch_custom_);
  if (fe95) {
    this->client_->find_characteristic(U16(0xFE95), U16(0x0010), this->ch_mi10_);
    this->client_->find_characteristic(U16(0xFE95), U16(0x0019), this->ch_mi19_);
  }
  if (mi) {
    this->client_->find_characteristic(U128(UUID_MI_MAIN), U128(UUID_MI_SPEED), this->ch_mi_speed_);
    this->client_->find_characteristic(U128(UUID_MI_MAIN), U128(UUID_MI_TEMP), this->ch_mi_temp_);
  }
  if (env) {
    this->client_->find_characteristic(U16(0x181A), U16(0x2A6E), this->ch_env_temp_);
    this->client_->find_characteristic(U16(0x181A), U16(0x2A6F), this->ch_env_humi_);
  }
  this->client_->find_characteristic(U16(0x180F), U16(0x2A19), this->ch_batt_);
  if (dis)
    for (int i = 0; i < 6; i++)
      this->client_->find_characteristic(U16(0x180A), U16(DIS_CHARS[i]), this->dis_[i]);
  this->logf("otaEnabled=%d customEnabled=%d miEnabled=%d devInfo=%d", ota, custom, mi, dis);
  d.hw.kind = custom ? FirmwareKind::CUSTOM_PVVX : (mi || fe95) ? FirmwareKind::STOCK_XIAOMI : (ota ? FirmwareKind::OTHER_TELINK : FirmwareKind::UNKNOWN);
  if (custom && !env)
    d.hw.kind = FirmwareKind::ATC1441;  // atc1441 firmware: 0x1F10 without 0x181A (customAction devIdEnabled)
  this->arm_step_timeout_(30000);
  this->step_read_dis_(0);
}

void XiaomiEspFlasher::step_read_dis_(size_t idx) {
  while (idx < 6 && this->dis_[idx].handle == 0)
    idx++;
  if (idx >= 6) {
    XiaomiDevice &d = *this->dev_;
    if (!d.dis_hardware.empty()) this->log("Hardware Revision String: " + d.dis_hardware);
    if (!d.dis_software.empty()) this->log("Software Revision String: " + d.dis_software);
    if (!d.dis_firmware.empty()) this->log("Firmware Revision String: " + d.dis_firmware);
    if (d.hw.kind == FirmwareKind::CUSTOM_PVVX || d.hw.kind == FirmwareKind::ATC1441)
      this->step_subscribe_custom_();
    else
      this->step_identify_stock_();
    return;
  }
  this->client_->read(this->dis_[idx].handle, [this, idx](int st, const uint8_t *data, size_t len) {
    if (!this->session_active()) return;
    std::string v = st == ESP_GATT_OK ? clean_str(data, len) : "";
    XiaomiDevice &d = *this->dev_;
    LockGuard g(this->mutex_);
    switch (DIS_CHARS[idx]) {
      case 0x2A24: d.dis_model = v; break;
      case 0x2A25: d.dis_serial = v; break;
      case 0x2A26: d.dis_firmware = v; break;
      case 0x2A27: d.dis_hardware = v; break;
      case 0x2A28: d.dis_software = v; break;
      case 0x2A29: d.dis_manufacturer = v; break;
    }
    this->defer([this, idx]() { if (this->session_active()) this->step_read_dis_(idx + 1); });
  });
}

void XiaomiEspFlasher::custom_cmd_(std::vector<uint8_t> payload, uint8_t expect, uint32_t timeout_ms,
                                   std::function<void(bool, const uint8_t *, size_t)> cb) {
  if (this->ch_custom_.handle == 0) { cb(false, nullptr, 0); return; }
  ESP_LOGD(TAG, "-> 1f1f %s", hexs(payload.data(), payload.size()).c_str());
  this->expect_cmd_ = expect;
  this->cmd_cb_ = std::move(cb);
  this->cmd_waiting_ = true;
  this->cmd_deadline_ = millis() + timeout_ms;
  bool rsp = this->ch_custom_.props & ESP_GATT_CHAR_PROP_BIT_WRITE;
  auto data = std::make_shared<std::vector<uint8_t>>(std::move(payload));
  this->client_->write(this->ch_custom_.handle, data->data(), data->size(), rsp, [this, data](int st) {
    if (st != ESP_GATT_OK && this->cmd_waiting_) {
      this->cmd_waiting_ = false;
      auto cb = std::move(this->cmd_cb_);
      this->cmd_cb_ = nullptr;
      if (cb) cb(false, nullptr, 0);
    }
  });
}

void XiaomiEspFlasher::on_notify_(uint16_t handle, const uint8_t *data, size_t len) {
  if (this->mi_auth_.active() && (handle == this->ch_mi10_.handle || handle == this->ch_mi19_.handle)) {
    this->mi_auth_.on_notify(handle, data, len);
    return;
  }
  if (handle == this->ch_custom_.handle && len > 0) {
    ESP_LOGD(TAG, "<- 1f1f %s", hexs(data, len).c_str());
    if (this->dev_ && this->job_.type == JobType::CONNECT && this->state_ == SessionState::READY) {
      std::string hx = hexs(data, len);
      uint32_t seq;
      {
        LockGuard g(this->mutex_);
        seq = ++this->notify_seq_;
        this->notify_.push_back(NotifyLine{seq, this->dev_->address, hx});
        while (this->notify_.size() > 64) this->notify_.pop_front();
      }
      this->emit_event_(std::string("{\"type\":\"notify\",\"mac\":\"") + this->dev_->mac + "\",\"seq\":" + std::to_string(seq) + ",\"hex\":\"" + hx + "\"}");
      this->hold_last_activity_ = millis();
    }
    if (this->dev_ && data[0] == CMD_ID_MEASURE) {
      Measurement m;
      if (m.decode(data, len)) {
        LockGuard g(this->mutex_);
        this->dev_->live = m;
      }
    }
    if (this->cmd_waiting_ && data[0] == this->expect_cmd_) {
      // measurement echo "33 ff" is not the measurement itself
      if (data[0] == CMD_ID_MEASURE && len < 9)
        return;
      this->cmd_waiting_ = false;
      auto cb = std::move(this->cmd_cb_);
      this->cmd_cb_ = nullptr;
      if (cb) cb(true, data, len);
    }
    return;
  }
  if (handle == this->ch_mi_temp_.handle && len >= 5 && this->dev_) {
    // stock Xiaomi ebe0ccc1: temp i16 x0.01, humi u8, vbat u16
    LockGuard g(this->mutex_);
    this->dev_->live.valid = true;
    this->dev_->live.temp = (int16_t) (data[0] | (data[1] << 8));
    this->dev_->live.humi = data[2] * 100;
    this->dev_->live.vbat_mv = data[3] | (data[4] << 8);
    this->stock_temp_seen_ = true;
  }
}

void XiaomiEspFlasher::step_subscribe_custom_() {
  if (this->ch_custom_.handle == 0) {
    this->finish_identification_();
    return;
  }
  this->client_->subscribe(this->ch_custom_, [this](int st) {
    if (!this->session_active()) return;
    if (st != ESP_GATT_OK) {
      this->fail_session_(Error::INTERNAL_ERROR, "subscribe 0x1F1F failed (" + std::to_string(st) + ")");
      return;
    }
    this->step_identify_custom_(0);
  });
}

// identification command list for custom firmware (order of TelinkMiFlasher: 00, 55, then GUI reads)
void XiaomiEspFlasher::step_identify_custom_(size_t idx) {
  static const uint8_t CMDS[] = {CMD_ID_DEV_ID, CMD_ID_CFG, CMD_ID_COMFORT, CMD_ID_CFS, CMD_ID_TRG, CMD_ID_DNAME,
                                 CMD_ID_UTC_TIME, CMD_ID_GDEVS, CMD_ID_DEV_MAC};
  if (idx >= sizeof(CMDS)) {
    this->step_read_env_(0);
    return;
  }
  uint8_t cmd = CMDS[idx];
  XiaomiDevice &d = *this->dev_;
  if (cmd == CMD_ID_CFS && d.cfg.valid && d.cfg.ver < 0x47) {  // sensor cfg only from 4.7
    this->step_identify_custom_(idx + 1);
    return;
  }
  if (this->job_.type == JobType::REBOOT && idx > 1) {  // reboot job needs only dev id + cfg
    this->step_read_env_(0);
    return;
  }
  this->custom_cmd_({cmd}, cmd, 2500, [this, idx, cmd](bool ok, const uint8_t *data, size_t len) {
    if (!this->session_active()) return;
    XiaomiDevice &d = *this->dev_;
    if (ok) {
      LockGuard g(this->mutex_);
      switch (cmd) {
        case CMD_ID_DEV_ID: d.dev_id.decode(data, len); break;
        case CMD_ID_CFG: d.cfg.decode(data, len); break;
        case CMD_ID_COMFORT: d.comfort.decode(data, len); break;
        case CMD_ID_CFS: d.sensor_cfg.decode(data, len); break;
        case CMD_ID_TRG: d.trigger.decode(data, len, d.cfg.ver); break;
        case CMD_ID_DNAME: d.device_name = clean_str(data + 1, len - 1); break;
        case CMD_ID_UTC_TIME:
          if (len >= 5) d.device_time = data[1] | (data[2] << 8) | (data[3] << 16) | ((uint32_t) data[4] << 24);
          if (len >= 9) d.device_time_set = data[5] | (data[6] << 8) | (data[7] << 16) | ((uint32_t) data[8] << 24);
          break;
        case CMD_ID_GDEVS:
          if (len >= 3) { d.hw.i2c_sensor = data[1]; d.hw.i2c_lcd = data[2]; }
          break;
        case CMD_ID_DEV_MAC:
          if (len >= 8) {
            char b[24];
            snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", data[7], data[6], data[5], data[4], data[3], data[2]);
            d.mac_from_device = b;
          }
          break;
      }
    } else {
      ESP_LOGW(TAG, "no response to cmd 0x%02X", cmd);
      if (cmd == CMD_ID_CFG && this->dev_->hw.kind == FirmwareKind::CUSTOM_PVVX && !this->dev_->cfg.valid) {
        // custom firmware without config channel answer: treat as atc1441-like / PIN protected
        this->dev_->pin_required = true;
      }
    }
    this->defer([this, idx]() { if (this->session_active()) this->step_identify_custom_(idx + 1); });
  });
}

void XiaomiEspFlasher::step_read_env_(size_t idx) {
  // Environmental sensing / battery characteristics (custom fw): temperature 0x2A6E, humidity 0x2A6F, battery 0x2A19
  const GattChar *chs[3] = {&this->ch_env_temp_, &this->ch_env_humi_, &this->ch_batt_};
  while (idx < 3 && chs[idx]->handle == 0)
    idx++;
  if (idx >= 3) {
    this->finish_identification_();
    return;
  }
  this->client_->read(chs[idx]->handle, [this, idx](int st, const uint8_t *data, size_t len) {
    if (!this->session_active()) return;
    if (st == ESP_GATT_OK) {
      LockGuard g(this->mutex_);
      XiaomiDevice &d = *this->dev_;
      if (idx == 0 && len >= 2) { d.live.temp = (int16_t) (data[0] | (data[1] << 8)); d.live.valid = true; }
      if (idx == 1 && len >= 2) { d.live.humi = data[0] | (data[1] << 8); d.live.valid = true; }
      if (idx == 2 && len >= 1) { d.adv.battery_pct = data[0]; }
    }
    this->defer([this, idx]() { if (this->session_active()) this->step_read_env_(idx + 1); });
  });
}

void XiaomiEspFlasher::step_identify_stock_() {
  // Stock Xiaomi: subscribe to the temperature characteristic to get one live sample (ebe0ccc1), max 8 s
  if (this->ch_mi_temp_.handle == 0) {
    this->finish_identification_();
    return;
  }
  this->client_->subscribe(this->ch_mi_temp_, [this](int st) {
    if (!this->session_active()) return;
    uint32_t until = millis() + 8000;
    this->set_interval("xf_stock_wait", 250, [this, until]() {
      if (!this->session_active() || this->stock_temp_seen_ || (int32_t) (millis() - until) > 0) {
        this->cancel_interval("xf_stock_wait");
        if (this->session_active()) this->finish_identification_();
      }
    });
  });
}

void XiaomiEspFlasher::finish_identification_() {
  XiaomiDevice &d = *this->dev_;
  {
    LockGuard g(this->mutex_);
    HardwareInfo &hw = d.hw;
    hw.hw_string = d.dis_hardware.substr(0, 4);
    hw.model = !d.dis_model.empty() ? d.dis_model : (!d.model_hint.empty() ? d.model_hint : d.name);
    if (hw.model.rfind("ATC_", 0) == 0 || hw.model.rfind("DEV_", 0) == 0) hw.model = "LYWSD03MMC?";
    hw.requires_cloud_token = false;
    if (hw.kind == FirmwareKind::CUSTOM_PVVX && d.cfg.valid) {
      hw.fw_ver_byte = d.cfg.ver;
      hw.fw_version = d.cfg.version_string();
      hw.big_ota = d.cfg.big_ota();
      // hwver_id from cfg (CustomBlkParse): ver < 0x48 -> hver & 0x0f (0x0f => 16 + lcd_tint&0x7f), else hver
      int id;
      if (d.cfg.ver < 0x48) {
        id = d.cfg.hver & 0x0f;
        if (id == 0x0f && d.cfg.raw_len >= 11) {
          id = 16 + (d.cfg.lcd_tint & 0x7f);
          if (d.cfg.lcd_tint & 0x80) hw.big_ota = true;
        }
      } else {
        id = d.cfg.hver;
      }
      if (d.dev_id.valid && d.dev_id.hw_version != 0 && d.dev_id.hw_version != id && d.cfg.ver >= 0x48)
        id = d.dev_id.hw_version;
      hw.hw_id = id;
      if (hw.model.rfind("LYWSD03MMC", 0) == 0 && !is_lywsd03mmc_id(id))
        hw.hw_id = HW_UNKNOWN;  // inconsistent
    } else if (hw.kind == FirmwareKind::STOCK_XIAOMI || hw.kind == FirmwareKind::OTHER_TELINK) {
      hw.fw_version = d.dis_firmware;
      hw.fw_ver_byte = 0;
      hw.big_ota = false;
      // miAuthorization() name-based mapping
      if (d.name == "LYWSD03MMC" || hw.model == "LYWSD03MMC") {
        hw.model = "LYWSD03MMC";
        hw.hw_id = lywsd03mmc_hw_id_from_string(hw.hw_string);
        if (d.dis_firmware.rfind("2.1.1_0159", 0) == 0) hw.requires_cloud_token = true;
      } else if (d.name.rfind("MHO-C401", 0) == 0) {
        hw.hw_id = HW_UNKNOWN;  // pvvx asks the user (old vs 2022) – cannot be decided safely here
      } else if (d.name.rfind("C121", 0) == 0) hw.hw_id = 11;
      else if (d.name.rfind("Qingping Temp & RH M", 0) == 0) hw.hw_id = 2;
      else if (d.name.rfind("Qingping Temp RH M", 0) == 0) hw.hw_id = 7;
      else if (d.name.rfind("Qingping Temp RH Lite", 0) == 0) hw.hw_id = 6;
      else if (d.name == "MJWSD05MMC") { hw.hw_id = d.dis_firmware.rfind("0005", 0) == 0 ? 12 : 9; hw.big_ota = true; }
      else if (d.name.rfind("MJWSD06MMC", 0) == 0) { hw.hw_id = 13; hw.big_ota = true; if (d.dis_firmware.rfind("0009", 0) == 0) hw.requires_cloud_token = true; }
      else if (d.name.rfind("LYWSD02MMC", 0) == 0) hw.hw_id = 49;
      else hw.hw_id = HW_UNKNOWN;
    } else {
      hw.hw_id = HW_UNKNOWN;
    }
    d.identified = true;
    d.identified_epoch = this->epoch_now_();
    d.status = DeviceStatus::IDENTIFIED;
  }
  this->recompute_eligibility_(d);
  this->logf("Device identified: %s, %s firmware %s, HW %s (id %d: %s)", d.hw.model.c_str(), firmware_kind_name(d.hw.kind),
             d.installed_version().c_str(), d.hw.hw_string.c_str(), d.hw.hw_id, hw_id_name(d.hw.hw_id));
  if (d.hw.kind == FirmwareKind::CUSTOM_PVVX && d.cfg.valid)
    this->logf("Hardware Version: %s %s, Software Version: %s", hw_id_name(d.hw.hw_id), d.dis_hardware.c_str(), d.cfg.version_string().c_str());
  if (d.live.valid)
    this->logf("Live: %.2f °C, %.2f %%, %u mV", d.live.temp / 100.0f, d.live.humi / 100.0f, d.live.vbat_mv);
  if (d.update_available)
    this->logf("Update available: %s -> %s", d.installed_version().c_str(), d.latest_version.c_str());
  else if (!d.compat.ok)
    this->logf("Update blocked: %s", d.compat.message.c_str());
  else
    this->log("Firmware is up to date");
  for (auto &w : d.compat.warnings) this->log("Warning: " + w);
  this->publish_device_(d, false);
  this->emit_device_event_(d);
  this->save_known_devices_();
  this->continue_after_identify_();
}

void XiaomiEspFlasher::continue_after_identify_() {
  if (this->verify_pending_) {
    this->evaluate_verify_();
    return;
  }
  switch (this->job_.type) {
    case JobType::IDENTIFY:
    case JobType::READ_CONFIG:
      this->disconnect_and_finish_(true, "identification complete");
      break;
    case JobType::WRITE_CONFIG: this->step_write_config_(); break;
    case JobType::SET_DEFAULTS: this->step_set_defaults_(); break;
    case JobType::SET_TIME: this->step_set_time_(); break;
    case JobType::SET_NAME: this->step_set_name_(); break;
    case JobType::SET_PIN: this->step_set_pin_(); break;
    case JobType::REBOOT: this->step_reboot_(); break;
    case JobType::ACTIVATE: this->step_activate_(); break;
    case JobType::FLASH: this->step_prepare_ota_(); break;
    case JobType::CONNECT: {
      // hold the link for the pvvx-style configuration GUI; commands arrive through queue_raw_command()
      this->set_state_(SessionState::READY);
      this->step_deadline_ = 0;
      this->hold_last_activity_ = millis();
      this->hold_release_ = false;
      { LockGuard g(this->mutex_); this->cmd_queue_.clear(); }
      this->dev_->status = DeviceStatus::CONNECTED;
      this->log("Connected – configuration channel open (idle timeout 5 min)");
      if (this->ch_custom_.handle != 0) {
        // customAction(): "Send cmd (33C8): Query 200 measurements"
        std::vector<uint8_t> q = {CMD_ID_MEASURE, 0xC8};
        std::string e;
        this->queue_raw_command(this->dev_->address, q, e);
      }
      this->publish_device_(*this->dev_, false);
      this->emit_device_event_(*this->dev_);
      break;
    }
    default: this->disconnect_and_finish_(true, "done"); break;
  }
}

// ------------------------------------------------------------------------------------------------ configuration jobs

void XiaomiEspFlasher::step_write_config_() {
  XiaomiDevice &d = *this->dev_;
  if (d.hw.kind != FirmwareKind::CUSTOM_PVVX || !d.cfg.valid) {
    this->fail_session_(Error::DEVICE_UNSUPPORTED, "configuration requires pvvx custom firmware");
    return;
  }
  this->set_state_(SessionState::WRITING);
  this->arm_step_timeout_(30000);
  if (!this->job_.has_cfg) {
    this->step_write_comfort_();
    return;
  }
  DeviceConfig nc = this->job_.cfg;
  nc.ver = d.cfg.ver;   // read-only fields come from the device
  nc.hver = d.cfg.hver;
  if (!d.cfg.offsets_in_cfg()) {  // fw >= 4.7: bytes 4/5 are flg3/event_adv_cnt, keep the device values unless the GUI sent them
    if (this->job_.cfg.temp_offset == 0 && this->job_.cfg.humi_offset == 0) { nc.temp_offset = d.cfg.temp_offset; nc.humi_offset = d.cfg.humi_offset; }
  }
  nc.clamp();
  auto payload = nc.encode();
  this->log("Send Config: 55" + hexs(payload.data() + 1, payload.size() - 1));
  this->custom_cmd_(payload, CMD_ID_CFG, 4000, [this, nc](bool ok, const uint8_t *data, size_t len) {
    if (!this->session_active()) return;
    if (!ok) { this->fail_session_(Error::BLE_TIMEOUT, "no answer to config write"); return; }
    DeviceConfig back;
    back.decode(data, len);
    {
      LockGuard g(this->mutex_);
      this->dev_->cfg = back;
    }
    auto sent = nc.encode();
    auto got = back.encode();
    bool same = sent.size() == got.size() && memcmp(sent.data() + 1, got.data() + 1, sent.size() - 1) == 0;
    // hver and connect_latency may be normalised by the firmware (test_config) – compare the user fields only
    if (!same) {
      bool core_same = sent.size() >= 10 && got.size() >= 10 && memcmp(sent.data() + 1, got.data() + 1, 9) == 0;
      if (!core_same) {
        this->log("Read back config differs: 55" + hexs(got.data() + 1, got.size() - 1));
        this->fail_session_(Error::CONFIG_MISMATCH, "device stored a different configuration than requested");
        return;
      }
    }
    this->log("Configuration successfully written (verified)");
    this->step_write_comfort_();
  });
}

void XiaomiEspFlasher::step_write_comfort_() {
  if (!this->job_.has_comfort) { this->step_write_sensor_(); return; }
  auto payload = this->job_.comfort.encode();
  this->custom_cmd_(payload, CMD_ID_COMFORT, 4000, [this](bool ok, const uint8_t *data, size_t len) {
    if (!this->session_active()) return;
    if (!ok) { this->fail_session_(Error::BLE_TIMEOUT, "no answer to comfort write"); return; }
    ComfortConfig back;
    back.decode(data, len);
    { LockGuard g(this->mutex_); this->dev_->comfort = back; }
    auto s = this->job_.comfort.encode(), r = back.encode();
    if (s != r) { this->fail_session_(Error::CONFIG_MISMATCH, "comfort parameters read back differ"); return; }
    this->log("Comfort parameters written (verified)");
    this->step_write_sensor_();
  });
}

void XiaomiEspFlasher::step_write_sensor_() {
  if (!this->job_.has_sensor || this->dev_->cfg.ver < 0x47) { this->step_write_trigger_(); return; }
  auto payload = this->job_.sensor.encode();
  this->custom_cmd_(payload, CMD_ID_CFS, 4000, [this](bool ok, const uint8_t *data, size_t len) {
    if (!this->session_active()) return;
    if (!ok) { this->fail_session_(Error::BLE_TIMEOUT, "no answer to sensor config write"); return; }
    SensorConfig back;
    back.decode(data, len);
    { LockGuard g(this->mutex_); this->dev_->sensor_cfg = back; }
    if (back.temp_z != this->job_.sensor.temp_z || back.humi_z != this->job_.sensor.humi_z ||
        back.temp_k != this->job_.sensor.temp_k || back.humi_k != this->job_.sensor.humi_k) {
      this->fail_session_(Error::CONFIG_MISMATCH, "sensor calibration read back differs");
      return;
    }
    this->log("Sensor calibration / offsets written (verified)");
    this->step_write_trigger_();
  });
}

void XiaomiEspFlasher::step_write_trigger_() {
  if (!this->job_.has_trigger) { this->step_readback_(); return; }
  auto payload = this->job_.trigger.encode(this->dev_->cfg.ver);
  this->custom_cmd_(payload, CMD_ID_TRG, 4000, [this](bool ok, const uint8_t *data, size_t len) {
    if (!this->session_active()) return;
    if (!ok) { this->fail_session_(Error::BLE_TIMEOUT, "no answer to trigger write"); return; }
    TriggerConfig back;
    back.decode(data, len, this->dev_->cfg.ver);
    { LockGuard g(this->mutex_); this->dev_->trigger = back; }
    this->log("Trigger parameters written");
    this->step_readback_();
  });
}

void XiaomiEspFlasher::step_readback_() {
  // final confirmation: re-read 55 and 20
  this->custom_cmd_({CMD_ID_CFG}, CMD_ID_CFG, 3000, [this](bool ok, const uint8_t *data, size_t len) {
    if (!this->session_active()) return;
    if (ok) { LockGuard g(this->mutex_); this->dev_->cfg.decode(data, len); }
    this->custom_cmd_({CMD_ID_COMFORT}, CMD_ID_COMFORT, 3000, [this](bool ok2, const uint8_t *d2, size_t l2) {
      if (!this->session_active()) return;
      if (ok2) { LockGuard g(this->mutex_); this->dev_->comfort.decode(d2, l2); }
      this->publish_device_(*this->dev_, false);
      this->disconnect_and_finish_(true, "configuration written and read back");
    });
  });
}

void XiaomiEspFlasher::step_set_defaults_() {
  this->set_state_(SessionState::WRITING);
  this->arm_step_timeout_(20000);
  this->custom_cmd_({CMD_ID_CFG_DEF}, CMD_ID_CFG, 4000, [this](bool ok, const uint8_t *data, size_t len) {
    if (!this->session_active()) return;
    if (!ok) { this->fail_session_(Error::BLE_TIMEOUT, "no answer to set defaults"); return; }
    { LockGuard g(this->mutex_); this->dev_->cfg.decode(data, len); }
    this->log("Device configuration reset to defaults");
    this->step_readback_();
  });
}

void XiaomiEspFlasher::step_set_time_() {
  if (this->dev_->hw.kind != FirmwareKind::CUSTOM_PVVX) { this->fail_session_(Error::DEVICE_UNSUPPORTED, "set time requires custom firmware"); return; }
  uint32_t now = this->epoch_now_();
  if (now == 0) { this->fail_session_(Error::INTERNAL_ERROR, "ESP32 clock is not synchronised (no SNTP time yet)"); return; }
  // setDevTime(): local time = utc - timezone offset
  uint32_t local = now + ESPTime::timezone_offset();  // setDevTime(): local time
  this->set_state_(SessionState::WRITING);
  this->arm_step_timeout_(15000);
  this->custom_cmd_(encode_set_time(local), CMD_ID_UTC_TIME, 4000, [this, local](bool ok, const uint8_t *data, size_t len) {
    if (!this->session_active()) return;
    if (!ok) { this->fail_session_(Error::BLE_TIMEOUT, "no answer to set time"); return; }
    uint32_t dev = len >= 5 ? (data[1] | (data[2] << 8) | (data[3] << 16) | ((uint32_t) data[4] << 24)) : 0;
    { LockGuard g(this->mutex_); this->dev_->device_time = dev; }
    this->logf("Device time set to %u (device reports %u)", (unsigned) local, (unsigned) dev);
    this->disconnect_and_finish_(true, "time set");
  });
}

void XiaomiEspFlasher::step_set_name_() {
  this->set_state_(SessionState::WRITING);
  this->arm_step_timeout_(15000);
  this->custom_cmd_(encode_device_name(this->job_.name), CMD_ID_DNAME, 4000, [this](bool ok, const uint8_t *data, size_t len) {
    if (!this->session_active()) return;
    if (!ok) { this->fail_session_(Error::BLE_TIMEOUT, "no answer to set name"); return; }
    { LockGuard g(this->mutex_); this->dev_->device_name = clean_str(data + 1, len - 1); }
    this->log("Device name is now '" + this->dev_->device_name + "' (effective after reboot/reconnect)");
    this->disconnect_and_finish_(true, "name set");
  });
}

void XiaomiEspFlasher::step_set_pin_() {
  this->set_state_(SessionState::WRITING);
  this->arm_step_timeout_(15000);
  this->custom_cmd_(encode_pin_code(this->job_.pin), CMD_ID_PINCODE, 4000, [this](bool ok, const uint8_t *, size_t) {
    if (!this->session_active()) return;
    this->log(ok ? "PIN code written" : "PIN write sent (no acknowledgement)");
    this->disconnect_and_finish_(true, "pin set");
  });
}

void XiaomiEspFlasher::step_reboot_() {
  if (this->ch_custom_.handle == 0) { this->fail_session_(Error::DEVICE_UNSUPPORTED, "reboot requires custom firmware"); return; }
  this->set_state_(SessionState::REBOOTING);
  this->arm_step_timeout_(15000);
  this->custom_cmd_({CMD_ID_REBOOT}, CMD_ID_REBOOT, 3000, [this](bool, const uint8_t *, size_t) {
    if (!this->session_active()) return;
    this->log("Reboot on disconnect requested");
    this->disconnect_and_finish_(true, "device reboots");
  });
}

// ------------------------------------------------------------------------------------------------ activation

void XiaomiEspFlasher::step_activate_() {
  XiaomiDevice &d = *this->dev_;
  if (this->ch_mi10_.handle == 0 || this->ch_mi19_.handle == 0) {
    if (d.hw.kind == FirmwareKind::CUSTOM_PVVX) {
      this->log("Custom firmware: no activation needed");
      if (this->job_.type == JobType::ACTIVATE) this->disconnect_and_finish_(true, "no activation needed");
      else this->step_prepare_ota_();
      return;
    }
    this->fail_session_(Error::DEVICE_UNSUPPORTED, "Mi authentication service (0xFE95) not found");
    return;
  }
  if (d.hw.requires_cloud_token && !this->job_.use_login) {
    this->fail_session_(Error::ACTIVATION_REQUIRED, "firmware " + d.dis_firmware + " requires Mi-Home token (login) – activation blocked");
    return;
  }
  this->set_state_(SessionState::ACTIVATING);
  this->arm_step_timeout_(100000);
  this->log(this->job_.use_login ? "Login with stored token" : "Activation started");
  // subscribe 0x0010 then 0x0019, then run MiAuth
  this->client_->subscribe(this->ch_mi10_, [this](int st) {
    if (!this->session_active()) return;
    if (st != ESP_GATT_OK) { this->fail_session_(Error::ACTIVATION_FAILED, "subscribe 0x0010 failed"); return; }
    this->client_->subscribe(this->ch_mi19_, [this](int st2) {
      if (!this->session_active()) return;
      if (st2 != ESP_GATT_OK) { this->fail_session_(Error::ACTIVATION_FAILED, "subscribe 0x0019 failed"); return; }
      this->mi_auth_.begin(this->client_, this->ch_mi10_, this->ch_mi19_,
                           this->job_.use_login ? MiAuth::Mode::LOGIN : MiAuth::Mode::REGISTER,
                           this->job_.use_login ? &this->job_.keys : nullptr,
                           [this](bool ok, Error e, const std::string &msg) {
                             if (!this->session_active()) return;
                             if (!ok) { this->fail_session_(e, msg); return; }
                             this->log("Activation successful: " + msg);
                             if (this->mi_auth_.keys().valid)
                               this->logf("Mi token %s bindkey %s", this->mi_auth_.keys().token_hex().c_str(), this->mi_auth_.keys().bind_key_hex().c_str());
                             if (this->job_.type == JobType::ACTIVATE) this->disconnect_and_finish_(true, msg);
                             else this->step_prepare_ota_();
                           },
                           [this](const std::string &l) { this->log(l); });
    });
  });
}

// ------------------------------------------------------------------------------------------------ OTA

void XiaomiEspFlasher::step_prepare_ota_() {
  XiaomiDevice &d = *this->dev_;
  if (this->ch_ota_.handle == 0) {
    this->fail_session_(Error::DEVICE_UNSUPPORTED, "Telink OTA service not found on this device");
    return;
  }
  if (this->state_ != SessionState::ACTIVATING && this->state_ != SessionState::READY) {
    // compatibility gate – evaluated with the freshly identified hardware
    CompatResult c = check_compatibility(d.hw, this->ota_fw_);
    if (!c.ok) {
      this->fail_session_(c.code, c.message);
      return;
    }
    this->installed_before_ = d.installed_version();
    this->logf("Compatibility check: PASSED (hw %s id %d, %s -> %s)", d.hw.hw_string.c_str(), d.hw.hw_id,
               this->installed_before_.c_str(), this->target_version_.c_str());
    if (d.hw.kind == FirmwareKind::STOCK_XIAOMI || (d.hw.kind == FirmwareKind::OTHER_TELINK && this->ch_mi10_.handle)) {
      this->step_activate_();
      return;
    }
  }
  this->set_state_(SessionState::READY);
  // Mi firmware: "1e0000" to the speed characteristic makes the upload faster (updateBegin)
  if (this->ch_mi_speed_.handle != 0) {
    static const uint8_t SPEED[3] = {0x1e, 0x00, 0x00};
    this->client_->write(this->ch_mi_speed_.handle, SPEED, 3, true, [this](int) { if (this->session_active()) this->step_big_ota_prepare_(); });
  } else {
    this->step_big_ota_prepare_();
  }
}

void XiaomiEspFlasher::step_big_ota_prepare_() {
  XiaomiDevice &d = *this->dev_;
  size_t limit = MAX_BLE_OTA_SIZE;
  if (this->ota_size_ > limit) {
    if (!d.hw.big_ota || this->ch_custom_.handle == 0) {
      this->fail_session_(Error::INCOMPATIBLE_FIRMWARE, "image over 128 KiB needs custom firmware >= 4.6 (ext OTA)");
      return;
    }
    // startDFU(): 73 00 00 04 00 <szk LE32>; wait for 73 03 (ready) then updateBegin()
    uint32_t szk = (this->ota_size_ + 1023) >> 10;
    if (d.cfg.ver <= 0x53 && szk <= 128) szk = 129;
    this->set_state_(SessionState::ERASING);
    this->arm_step_timeout_(60000);
    this->logf("Clear Ext.OTA region (%u kib)...", (unsigned) szk);
    std::vector<uint8_t> p = {CMD_ID_SET_OTA, 0, 0, 4, 0, (uint8_t) szk, (uint8_t) (szk >> 8), (uint8_t) (szk >> 16), (uint8_t) (szk >> 24)};
    this->custom_cmd_(p, CMD_ID_SET_OTA, 45000, [this](bool ok, const uint8_t *data, size_t len) {
      if (!this->session_active()) return;
      if (!ok || len < 2) { this->fail_session_(Error::OTA_WRITE_FAILED, "no answer to ext OTA prepare"); return; }
      uint8_t e = data[1];
      if (e == 3) { this->log("Ext.OTA region ready"); this->step_start_ota_(); }
      else if (e == 2 || e == 4) { this->log("Ext.OTA busy, waiting"); /* wait for another 73 notification */
        this->custom_cmd_({}, CMD_ID_SET_OTA, 45000, [this](bool ok2, const uint8_t *d2, size_t l2) {
          if (!this->session_active()) return;
          if (ok2 && l2 >= 2 && d2[1] == 3) this->step_start_ota_();
          else this->fail_session_(Error::OTA_WRITE_FAILED, "ext OTA region not ready");
        });
      } else { this->fail_session_(Error::OTA_WRITE_FAILED, "ext OTA error " + std::to_string(e)); }
    });
    return;
  }
  this->step_start_ota_();
}

void XiaomiEspFlasher::step_start_ota_() {
  this->set_state_(SessionState::WRITING);
  this->arm_step_timeout_(0);
  this->step_deadline_ = 0;
  this->log("OTA started");
  this->last_progress_event_ms_ = 0;
  this->last_pct10_ = 0;
  // ask for a fast connection interval – the peripheral itself requests 20 ms + latency 0 in OTA mode
  this->client_->request_conn_params(12, 24, 0, 400);
  this->ota_.begin(this->client_, this->ch_ota_.handle, this->ota_reader_, this->ota_size_,
                   [this](const OtaProgress &p) {
                     uint32_t now = millis();
                     if (now - this->last_progress_event_ms_ >= 500 || p.state != OtaState::WRITING) {
                       this->last_progress_event_ms_ = now;
                       this->emit_progress_event_();
                       unsigned pct10 = (unsigned) p.percent() / 10;
                       if (p.state == OtaState::WRITING && pct10 != this->last_pct10_) {
                         this->last_pct10_ = pct10;
                         this->logf("Writing %u%% (%u/%u blocks, %.0f B/s)", (unsigned) p.percent(), (unsigned) p.blocks_sent,
                                    (unsigned) p.blocks_total, p.bytes_per_second());
                       }
#ifdef USE_SENSOR
                       if (this->ota_progress_sensor_) this->ota_progress_sensor_->publish_state(p.percent());
#endif
#ifdef USE_UPDATE
                       if (this->dev_) this->publish_device_(*this->dev_, false);
#endif
                     }
                   },
                   [this](bool ok, Error e, const std::string &msg) { this->on_ota_done_(ok, e, msg); });
}

void XiaomiEspFlasher::emit_progress_event_() {
  const OtaProgress &p = this->ota_.progress();
  json::JsonBuilder b;
  JsonObject o = b.root();
  o["type"] = "ota_progress";
  o["device"] = this->dev_ ? this->dev_->mac : "";
  o["state"] = ota_state_name(p.state);
  o["session"] = session_state_name(this->state_);
  o["progress"] = p.percent();
  o["bytes_sent"] = p.bytes_sent;
  o["total_bytes"] = p.bytes_total;
  o["blocks_sent"] = p.blocks_sent;
  o["blocks_total"] = p.blocks_total;
  o["speed"] = p.bytes_per_second();
  o["elapsed_ms"] = p.elapsed_ms;
  o["retries"] = p.retries;
  o["last_error"] = p.error == Error::NONE ? "" : error_to_string(p.error);
  auto s = b.serialize();
  this->emit_event_(std::string(s.c_str(), s.size()));
}

void XiaomiEspFlasher::on_ota_done_(bool ok, Error err, const std::string &msg) {
  if (!this->session_active()) return;
  this->emit_progress_event_();
  if (!ok) {
    this->fail_session_(err, msg);
    return;
  }
  this->log("Verifying... (device checks the image CRC and reboots): " + msg);
  this->set_state_(SessionState::REBOOTING);
  this->dev_->status = DeviceStatus::UPDATING;
  this->verify_attempts_ = 0;
  this->ota_end_ms_ = millis();
  // Do NOT disconnect here: the last write-without-response packets may still sit in the controller queue and
  // would be discarded (observed: image rejected, device stayed on the old firmware). The peripheral drops the
  // link itself when it reboots; on_connected_(false) schedules the reconnect. Fallback after 25 s below.
  if (this->client_->is_idle()) {
    this->reconnect_at_ = millis() + 4000;
  } else {
    this->reconnect_at_ = 0;
  }
  this->arm_step_timeout_(60000);
}

void XiaomiEspFlasher::step_verify_reconnect_() {
  if (!this->client_->is_idle()) {
    this->reconnect_at_ = millis() + 2000;
    return;
  }
  this->set_state_(SessionState::RECONNECTING);
  this->log("Reconnecting to verify the new firmware...");
  this->connect_attempts_ = 0;
  this->disconnect_requested_ = false;
  this->cmd_waiting_ = false;
  this->cmd_cb_ = nullptr;
  this->client_->begin_connect(this->dev_->address, (esp_ble_addr_type_t) this->dev_->addr_type);
  this->arm_step_timeout_(35000);
}

void XiaomiEspFlasher::on_verify_connected_() {
  // run the identification chain again; continue_after_identify_() evaluates the result (verify_pending_)
  this->set_state_(SessionState::VERIFYING);
  XiaomiDevice &d = *this->dev_;
  {
    LockGuard g(this->mutex_);
    d.cfg = DeviceConfig{};
    d.dev_id = DevId{};
    d.dis_firmware.clear();
    d.dis_software.clear();
  }
  this->verify_pending_ = true;
  this->step_discover_();
}

void XiaomiEspFlasher::evaluate_verify_() {
  this->verify_pending_ = false;
  XiaomiDevice &d = *this->dev_;
  std::string now = d.installed_version();
  bool match = compare_versions(now, this->target_version_) == 0 && !now.empty();
  d.last_ota_epoch = this->epoch_now_();
  this->last_ota_epoch_ = d.last_ota_epoch;
  if (match) {
    d.ota_result = "success " + this->installed_before_ + " -> " + now;
    this->last_ota_result_ = d.ota_result + " (" + d.mac + ")";
    this->log("Read new firmware version: " + now);
    this->disconnect_and_finish_(true, "OTA verified: firmware " + this->installed_before_ + " -> " + now);
  } else {
    d.ota_result = "verify failed: " + now;
    this->last_ota_result_ = d.ota_result + " (" + d.mac + ")";
    this->fail_session_(Error::OTA_VERIFY_FAILED, "device reports firmware '" + now + "', expected " + this->target_version_);
  }
}

// ------------------------------------------------------------------------------------------------ entities

#ifdef USE_UPDATE
void XiaomiFirmwareUpdate::perform(bool force) {
  if (this->parent_ == nullptr) return;
  XiaomiDevice *d = this->parent_->find_device(this->mac_);
  if (d == nullptr) return;
  if (!d->update_available && !force) {
    ESP_LOGW(TAG, "update requested for %s but no update is available", d->mac);
    return;
  }
  if (!d->compat.ok) {
    ESP_LOGW(TAG, "update blocked for %s: %s", d->mac, d->compat.message.c_str());
    return;
  }
  this->parent_->request_flash(this->mac_, d->latest_firmware_id, true);
}
void XiaomiFirmwareUpdate::check() {
  if (this->parent_ != nullptr) this->parent_->request_identify(this->mac_, true);
}
void XiaomiFirmwareUpdate::set_info(const std::string &current, const std::string &latest, const std::string &title,
                                    const std::string &summary, bool available, bool installing, float progress, bool has_progress) {
  bool changed = this->update_info_.current_version != current || this->update_info_.latest_version != latest ||
                 this->update_info_.summary != summary || this->update_info_.has_progress != has_progress ||
                 (has_progress && this->update_info_.progress != progress);
  update::UpdateState st = installing ? update::UPDATE_STATE_INSTALLING : (available ? update::UPDATE_STATE_AVAILABLE : update::UPDATE_STATE_NO_UPDATE);
  if (current == "unknown") st = update::UPDATE_STATE_UNKNOWN;
  changed = changed || st != this->state_;
  this->update_info_.current_version = current;
  this->update_info_.latest_version = latest;
  this->update_info_.title = title;
  this->update_info_.summary = summary;
  this->update_info_.has_progress = has_progress;
  this->update_info_.progress = progress;
  this->state_ = st;
  if (changed) this->publish_state();
}
#endif

#ifdef USE_BUTTON
void XiaomiFlasherButton::press_action() {
  if (this->parent_ == nullptr) return;
  switch (this->action_) {
    case ButtonAction::SCAN: this->parent_->request_scan(); break;
    case ButtonAction::IDENTIFY: this->parent_->request_identify(this->mac_, true); break;
    case ButtonAction::FLASH: {
      XiaomiDevice *d = this->parent_->find_device(this->mac_);
      if (d && d->update_available && d->compat.ok) this->parent_->request_flash(this->mac_, d->latest_firmware_id, true);
      else ESP_LOGW(TAG, "flash button: no compatible update for %012llX", (unsigned long long) this->mac_);
      break;
    }
    case ButtonAction::SET_TIME: { Job j; j.type = JobType::SET_TIME; j.mac = this->mac_; std::string e; this->parent_->request_job(j, e); break; }
    case ButtonAction::REBOOT: { Job j; j.type = JobType::REBOOT; j.mac = this->mac_; std::string e; this->parent_->request_job(j, e); break; }
    case ButtonAction::CHECK_ONLINE: this->parent_->request_check_online(); break;
    case ButtonAction::UPDATE_ALL: this->parent_->request_update_all(); break;
  }
}
#endif

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
