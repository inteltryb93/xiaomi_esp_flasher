#pragma once
// XiaomiEspFlasher – the hub component.
//  * listens to the BLE scanner (esp32_ble_tracker) and keeps a registry of thermometers (XiaomiDevice)
//  * runs one BLE "session" (job) at a time on the TelinkBleClient: identify / read+write config / set time /
//    activation (MiAuth) / OTA (TelinkOtaProtocol) / verify, as an explicit state machine with timeouts
//  * serves the web GUI + JSON API + SSE on the ESPHome web server base
//  * feeds Home Assistant entities (sensor/text_sensor/binary_sensor/button/update)
#include "esphome/core/defines.h"
#ifdef USE_ESP32
#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/json/json_util.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_UPDATE
#include "esphome/components/update/update_entity.h"
#endif
#include "telink_ble_client.h"
#include "telink_ota.h"
#include "mi_auth.h"
#include "xiaomi_device.h"
#include "device_config.h"
#include "compatibility.h"
#include "firmware_store.h"
#include "firmware_provider.h"
#include "errors.h"

namespace esphome {
namespace xiaomi_esp_flasher {

class XiaomiEspFlasher;

enum class SessionState : uint8_t {
  IDLE = 0, SCAN, CONNECTING, CONNECTED, IDENTIFYING, ACTIVATING, READY, ERASING, WRITING, VERIFYING,
  REBOOTING, RECONNECTING, SUCCESS, ERROR,
};
const char *session_state_name(SessionState s);

enum class JobType : uint8_t {
  NONE = 0, IDENTIFY, READ_CONFIG, WRITE_CONFIG, SET_DEFAULTS, SET_TIME, SET_NAME, SET_PIN, REBOOT, ACTIVATE, FLASH, CONNECT,
};
const char *job_type_name(JobType t);

struct Job {
  JobType type{JobType::NONE};
  uint64_t mac{0};
  std::string firmware_id;
  bool from_ha{false};
  bool has_cfg{false};
  DeviceConfig cfg;
  bool has_comfort{false};
  ComfortConfig comfort;
  bool has_sensor{false};
  SensorConfig sensor;
  bool has_trigger{false};
  TriggerConfig trigger;
  std::string name;
  uint32_t pin{0};
  MiKeys keys;
  bool use_login{false};
};

struct NotifyLine {
  uint32_t seq;
  uint64_t mac;
  std::string hex;
};

struct LogLine {
  uint32_t ms;
  uint32_t epoch;
  uint32_t seq;
  std::string text;
};

#ifdef USE_UPDATE
class XiaomiFirmwareUpdate : public update::UpdateEntity, public Component {
 public:
  void set_parent(XiaomiEspFlasher *p) { this->parent_ = p; }
  void set_mac(uint64_t mac) { this->mac_ = mac; }
  uint64_t mac() const { return this->mac_; }
  void perform(bool force) override;
  void check() override;
  void set_info(const std::string &current, const std::string &latest, const std::string &title, const std::string &summary,
                bool available, bool installing, float progress, bool has_progress);
 protected:
  XiaomiEspFlasher *parent_{nullptr};
  uint64_t mac_{0};
};
#endif

#ifdef USE_BUTTON
enum class ButtonAction : uint8_t { SCAN, IDENTIFY, FLASH, SET_TIME, REBOOT, CHECK_ONLINE, UPDATE_ALL };
class XiaomiFlasherButton : public button::Button, public Component {
 public:
  void set_parent(XiaomiEspFlasher *p) { this->parent_ = p; }
  void set_action(ButtonAction a) { this->action_ = a; }
  void set_mac(uint64_t mac) { this->mac_ = mac; }
 protected:
  void press_action() override;
  XiaomiEspFlasher *parent_{nullptr};
  ButtonAction action_{ButtonAction::SCAN};
  uint64_t mac_{0};
};
#endif

struct DeviceEntities {
  uint64_t mac{0};
#ifdef USE_SENSOR
  sensor::Sensor *temperature{nullptr}, *humidity{nullptr}, *battery{nullptr}, *battery_voltage{nullptr}, *rssi{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *firmware{nullptr}, *hardware{nullptr}, *model{nullptr}, *status{nullptr}, *last_seen{nullptr},
      *update_status{nullptr}, *device_name{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *update_available{nullptr};
#endif
#ifdef USE_UPDATE
  XiaomiFirmwareUpdate *update{nullptr};
#endif
};

// SSE session (one per browser tab)
struct SseClient {
  void *hd{nullptr};
  std::atomic<int> fd{0};
  uint32_t last_seq{0};
};

class XiaomiEspFlasher : public Component, public espbt::ESPBTDeviceListener, public AsyncWebHandler {
 public:
  explicit XiaomiEspFlasher(web_server_base::WebServerBase *base) : base_(base) {}

  // ---- configuration (codegen) ----
  void set_client(TelinkBleClient *c) { this->client_ = c; }
  void set_test_device_mac(uint64_t mac) { this->test_mac_ = mac; }
  void add_known_device(uint64_t mac, const std::string &alias);
  void set_auto_identify(bool v) { this->auto_identify_ = v; }
  void set_identify_interval(uint32_t s) { this->identify_interval_s_ = s; }
  void set_manifest_json(const char *json) { this->manifest_json_ = json; }
  void add_bundled_image(const char *name, const uint8_t *data, size_t size) { this->bundled_.push_back({name, data, size}); }
  void set_remote_manifest_url(const std::string &url) { this->remote_url_ = url; }
  void set_assets(const uint8_t *html, size_t html_len, const uint8_t *js, size_t js_len, const uint8_t *css, size_t css_len,
                  const uint8_t *pvvx, size_t pvvx_len) {
    this->html_ = html; this->html_len_ = html_len; this->js_ = js; this->js_len_ = js_len; this->css_ = css; this->css_len_ = css_len;
    this->pvvx_js_ = pvvx; this->pvvx_js_len_ = pvvx_len;
  }
  // GUI files served from a CDN (GitHub via jsDelivr) instead of flash: "/" becomes a tiny bootstrap page
  void set_assets_url(const std::string &url) { this->assets_url_ = url; }
  void add_device_entities(const DeviceEntities &e) { this->entities_.push_back(e); }
#ifdef USE_TEXT_SENSOR
  void set_status_text_sensor(text_sensor::TextSensor *s) { this->status_sensor_ = s; }
  void set_last_error_text_sensor(text_sensor::TextSensor *s) { this->last_error_sensor_ = s; }
#endif
#ifdef USE_SENSOR
  void set_device_count_sensor(sensor::Sensor *s) { this->device_count_sensor_ = s; }
  void set_ota_progress_sensor(sensor::Sensor *s) { this->ota_progress_sensor_ = s; }
  void set_updates_available_sensor(sensor::Sensor *s) { this->updates_available_sensor_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_ota_active_binary_sensor(binary_sensor::BinarySensor *s) { this->ota_active_sensor_ = s; }
#endif

  // ---- ESPHome component ----
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH - 1; }

  // ---- BLE scanner listener ----
  bool parse_device(const espbt::ESPBTDevice &device) override;
  void on_scan_end() override {}

  // ---- web server handler (httpd task!) ----
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

  // ---- actions (main loop) ----
  bool request_job(const Job &job, std::string &err);
  bool request_identify(uint64_t mac, bool from_ha = false);
  bool request_flash(uint64_t mac, const std::string &fw_id, bool from_ha);
  void request_scan();
  void request_update_all();
  void request_check_online();
  void forget_device(uint64_t mac);
  void set_alias(uint64_t mac, const std::string &alias);
  // raw command bridge (pvvx-style GUI): valid while a CONNECT job holds the link
  bool hold_active(uint64_t mac) const;
  bool queue_raw_command(uint64_t mac, const std::vector<uint8_t> &payload, std::string &err);
  void release_hold() { this->hold_release_ = true; this->enable_loop_soon_any_context(); }
  XiaomiDevice *find_device(uint64_t mac);
  const XiaomiDevice *find_device(uint64_t mac) const;
  bool session_active() const { return this->state_ != SessionState::IDLE && this->state_ != SessionState::SUCCESS && this->state_ != SessionState::ERROR; }

 protected:
  // ---- helpers ----
  void log(const std::string &line);
  void logf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  uint32_t epoch_now_() const;
  std::string epoch_str_(uint32_t epoch) const;
  XiaomiDevice *get_or_create_device_(uint64_t mac, const char *mac_str, uint8_t addr_type);
  void recompute_eligibility_(XiaomiDevice &d);
  void publish_device_(XiaomiDevice &d, bool measurements_only);
  void publish_global_();
  void save_known_devices_();
  void load_known_devices_();
  void set_state_(SessionState s);
  void fail_session_(Error e, const std::string &msg);
  void finish_session_(bool ok, const std::string &msg);
  void emit_event_(const std::string &json);
  void emit_progress_event_();
  void emit_device_event_(const XiaomiDevice &d);

  // ---- session steps ----
  void start_job_(const Job &job);
  void step_connect_();
  void on_connected_(bool established, int reason);
  void step_discover_();
  void step_read_dis_(size_t idx);
  void step_subscribe_custom_();
  void step_identify_custom_(size_t idx);
  void step_read_env_(size_t idx);
  void step_identify_stock_();
  void finish_identification_();
  void continue_after_identify_();
  void step_write_config_();
  void step_write_comfort_();
  void step_write_sensor_();
  void step_write_trigger_();
  void step_readback_();
  void step_set_time_();
  void step_set_name_();
  void step_set_pin_();
  void step_set_defaults_();
  void step_reboot_();
  void step_activate_();
  void step_prepare_ota_();
  void step_big_ota_prepare_();
  void step_start_ota_();
  void on_ota_done_(bool ok, Error err, const std::string &msg);
  void step_verify_reconnect_();
  void on_verify_connected_();
  void evaluate_verify_();
  void custom_cmd_(std::vector<uint8_t> payload, uint8_t expect, uint32_t timeout_ms,
                   std::function<void(bool ok, const uint8_t *data, size_t len)> cb);
  void on_notify_(uint16_t handle, const uint8_t *data, size_t len);
  void disconnect_and_finish_(bool ok, const std::string &msg);
  void arm_step_timeout_(uint32_t ms) { this->step_deadline_ = millis() + ms; }

  // ---- HTTP (httpd task) ----
  void handle_get_(AsyncWebServerRequest *req, const std::string &url);
  void handle_post_(AsyncWebServerRequest *req, const std::string &url, const std::string &body);
  void send_json_(AsyncWebServerRequest *req, int code, const std::string &json);
  std::string json_status_();
  void send_devices_chunked_(AsyncWebServerRequest *req);
  void send_firmware_chunked_(AsyncWebServerRequest *req);
  void send_log_chunked_(AsyncWebServerRequest *req, uint32_t since);
  std::string json_device_(const XiaomiDevice &d, bool full);
  std::string json_ota_status_();
  std::string json_notify_(uint64_t mac, uint32_t since);
  void load_manifest_pref_();
  bool save_manifest_pref_(const std::string &json);
  void device_to_json_(JsonObject o, const XiaomiDevice &d, bool full);
  void handle_sse_(AsyncWebServerRequest *req);
  static void sse_free_ctx_(void *ctx);
  void sse_pump_();
  bool parse_job_from_json_(const std::string &url_tail, uint64_t mac, const std::string &body, Job &job, std::string &err);
  bool parse_mac_(const std::string &s, uint64_t &mac) const;
  // uploads
  struct Upload {
    bool active{false};
    bool failed{false};
    std::string err;
    std::string name;
    std::string version;
    std::string source;
    ImageKind kind{ImageKind::USER_UPLOAD};
    uint64_t hw_ids{0};
    size_t total{0};
    size_t received{0};
    // finish (validation + header write + rescan) runs on the main loop; the httpd task waits for `done`
    std::atomic<bool> finish_requested{false};
    std::atomic<bool> done{false};
    bool ok{false};
    std::string result_id;
    size_t result_size{0};
    uint32_t result_crc{0};
    void reset() {
      active = failed = ok = false; err.clear(); name.clear(); version.clear(); source.clear(); result_id.clear();
      kind = ImageKind::USER_UPLOAD; hw_ids = 0; total = received = result_size = 0; result_crc = 0;
      finish_requested = false; done = false;
    }
  } upload_;
  std::string body_;

  // ---- members ----
  web_server_base::WebServerBase *base_;
  TelinkBleClient *client_{nullptr};
  TelinkOtaProtocol ota_;
  MiAuth mi_auth_;
  FirmwareStore store_;
  std::unique_ptr<LocalFirmwareProvider> provider_;
  std::vector<BundledImage> bundled_;
  const char *manifest_json_{""};
  std::string remote_url_;
  std::vector<std::unique_ptr<XiaomiDevice>> devices_;
  std::vector<DeviceEntities> entities_;
  std::vector<std::pair<uint64_t, std::string>> configured_aliases_;
  uint64_t test_mac_{0};
  bool auto_identify_{true};
  uint32_t identify_interval_s_{6 * 3600};
  uint32_t last_auto_identify_ms_{0};
  const uint8_t *html_{nullptr}, *js_{nullptr}, *css_{nullptr}, *pvvx_js_{nullptr};
  size_t html_len_{0}, js_len_{0}, css_len_{0}, pvvx_js_len_{0};
  std::string assets_url_;
  ESPPreferenceObject manifest_pref_;
  std::string runtime_manifest_;
  ESPPreferenceObject pref_;

  // session
  SessionState state_{SessionState::IDLE};
  Job job_;
  XiaomiDevice *dev_{nullptr};  // device of the running session
  uint32_t step_deadline_{0};
  uint32_t session_started_{0};
  uint8_t connect_attempts_{0};
  uint8_t verify_attempts_{0};
  uint32_t reconnect_at_{0};
  uint32_t ota_end_ms_{0};
  bool disconnect_requested_{false};
  bool verify_pending_{false};
  std::string finish_msg_;
  bool finish_ok_{false};
  GattChar ch_ota_, ch_custom_, ch_mi10_, ch_mi19_, ch_mi_speed_, ch_mi_temp_, ch_env_temp_, ch_env_humi_, ch_batt_;
  GattChar dis_[6];
  // custom command wait
  uint8_t expect_cmd_{0};
  std::function<void(bool, const uint8_t *, size_t)> cmd_cb_;
  uint32_t cmd_deadline_{0};
  bool cmd_waiting_{false};
  bool stock_temp_seen_{false};
  std::string target_version_;
  std::string installed_before_;
  std::string name_before_;
  ImageReader ota_reader_;
  size_t ota_size_{0};
  FirmwareInfo ota_fw_;

  // queues (protected by mutex_)
  Mutex mutex_;
  std::deque<Job> job_queue_;
  std::deque<uint64_t> update_queue_;
  std::deque<LogLine> log_;
  uint32_t log_seq_{0};
  std::deque<std::string> events_;
  std::deque<NotifyLine> notify_;
  uint32_t notify_seq_{0};
  std::deque<std::vector<uint8_t>> cmd_queue_;
  bool hold_release_{false};
  uint32_t hold_last_activity_{0};
  std::vector<SseClient *> sse_;
  std::atomic<bool> scan_requested_{false};
  std::atomic<bool> check_online_requested_{false};
  std::atomic<bool> update_all_requested_{false};
  std::atomic<bool> recompute_requested_{false};  // set from the httpd task, handled in loop()
  uint32_t last_scan_epoch_{0};
  uint32_t last_ota_epoch_{0};
  std::string last_ota_result_;
  Error last_error_{Error::NONE};
  std::string last_error_msg_;
  uint32_t last_progress_event_ms_{0};
  unsigned last_pct10_{0};
  uint32_t last_global_publish_ms_{0};
  bool needs_global_publish_{false};

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *status_sensor_{nullptr};
  text_sensor::TextSensor *last_error_sensor_{nullptr};
#endif
#ifdef USE_SENSOR
  sensor::Sensor *device_count_sensor_{nullptr};
  sensor::Sensor *ota_progress_sensor_{nullptr};
  sensor::Sensor *updates_available_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *ota_active_sensor_{nullptr};
#endif
};

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
