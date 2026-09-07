// HTTP API, JSON serialisation and Server-Sent Events for XiaomiEspFlasher.
// NOTE: canHandle/handleRequest/handleBody run on the esp_http_server task; everything that touches the
// device registry takes mutex_, and every action is queued to the main loop (request_job & friends).
#include "xiaomi_esp_flasher.h"
#ifdef USE_ESP32
#include <cstring>
#include <cmath>
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"
#include "esphome/components/network/util.h"
#include "esphome/components/web_server_idf/web_server_idf.h"
#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <esp_chip_info.h>
#include <esp_system.h>
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

namespace esphome {
namespace xiaomi_esp_flasher {

static const char *const TAG = "xiaomi_flasher.web";

static std::string url_of(AsyncWebServerRequest *req) {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef u = req->url_to(std::span<char, AsyncWebServerRequest::URL_BUF_SIZE>(buf));
  return std::string(u.c_str(), u.size());
}

bool XiaomiEspFlasher::canHandle(AsyncWebServerRequest *request) const {
  std::string url = url_of(request);
  if (url == "/" || url == "/index.html" || url == "/app.js" || url == "/style.css")
    return true;
  return url.rfind("/api/", 0) == 0;
}

void XiaomiEspFlasher::send_json_(AsyncWebServerRequest *req, int code, const std::string &json) {
  AsyncWebServerResponse *r = req->beginResponse(code, "application/json", json);
  r->addHeader("Cache-Control", "no-store");
  req->send(r);
}

// ------------------------------------------------------------------------------------------------ uploads (raw body)

void XiaomiEspFlasher::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  std::string url = url_of(request);
  if (url == "/api/firmware/upload") {
    if (index == 0) {
      this->upload_ = Upload{};
      this->upload_.active = true;
      this->upload_.total = total;
      this->upload_.name = request->hasArg("name") ? request->arg("name") : "upload.bin";
      if (this->session_active() && this->job_.type == JobType::FLASH) {
        this->upload_.failed = true;
        this->upload_.err = "BUSY: OTA in progress";
        return;
      }
      std::string err;
      if (!this->store_.begin_write(total, err)) {
        this->upload_.failed = true;
        this->upload_.err = err;
        return;
      }
    }
    if (this->upload_.failed)
      return;
    std::string err;
    if (!this->store_.write_chunk(data, len, err)) {
      this->upload_.failed = true;
      this->upload_.err = err;
      this->store_.abort_write();
      return;
    }
    this->upload_.received += len;
    return;
  }
  // JSON bodies: keep small (<= 4 KB)
  if (index == 0)
    this->body_.clear();
  if (total > 4096) {
    this->body_.clear();
    return;
  }
  this->body_.append((const char *) data, len);
}

// ------------------------------------------------------------------------------------------------ request dispatch

void XiaomiEspFlasher::handleRequest(AsyncWebServerRequest *request) {
  std::string url = url_of(request);
  if (request->method() == HTTP_OPTIONS) {
    request->send(204);
    return;
  }
  if (request->method() == HTTP_GET) {
    this->handle_get_(request, url);
    return;
  }
  if (request->method() == HTTP_POST) {
    std::string body = this->body_;
    this->body_.clear();
    this->handle_post_(request, url, body);
    return;
  }
  request->send(404);
}

void XiaomiEspFlasher::handle_get_(AsyncWebServerRequest *req, const std::string &url) {
  auto serve = [&](const uint8_t *d, size_t n, const char *ct) {
    if (d == nullptr || n == 0) { req->send(404); return; }
    AsyncWebServerResponse *r = req->beginResponse(200, ct, d, n);
    r->addHeader("Content-Encoding", "gzip");
    r->addHeader("Cache-Control", "no-cache");
    req->send(r);
  };
  if (url == "/" || url == "/index.html") { serve(this->html_, this->html_len_, "text/html"); return; }
  if (url == "/app.js") { serve(this->js_, this->js_len_, "application/javascript"); return; }
  if (url == "/style.css") { serve(this->css_, this->css_len_, "text/css"); return; }
  if (url == "/api/status") { this->send_json_(req, 200, this->json_status_()); return; }
  if (url == "/api/devices") { this->send_json_(req, 200, this->json_devices_()); return; }
  if (url == "/api/firmware") { this->send_json_(req, 200, this->json_firmware_()); return; }
  if (url == "/api/ota/status") { this->send_json_(req, 200, this->json_ota_status_()); return; }
  if (url == "/api/log") {
    uint32_t since = req->hasArg("since") ? strtoul(req->arg("since").c_str(), nullptr, 10) : 0;
    this->send_json_(req, 200, this->json_log_(since));
    return;
  }
  if (url == "/api/events") { this->handle_sse_(req); return; }
  if (url.rfind("/api/device/", 0) == 0) {
    std::string rest = url.substr(12);
    std::string macs = rest.substr(0, rest.find('/'));
    std::string tail = rest.size() > macs.size() ? rest.substr(macs.size() + 1) : "";
    uint64_t mac;
    if (!this->parse_mac_(macs, mac)) { this->send_json_(req, 400, "{\"ok\":false,\"error\":\"INVALID_REQUEST\",\"msg\":\"bad mac\"}"); return; }
    LockGuard g(this->mutex_);
    const XiaomiDevice *d = this->find_device(mac);
    if (d == nullptr) { this->send_json_(req, 404, "{\"ok\":false,\"error\":\"DEVICE_NOT_FOUND\"}"); return; }
    if (tail == "" || tail == "config") { this->send_json_(req, 200, this->json_device_(*d, true)); return; }
  }
  req->send(404);
}

static uint64_t hw_ids_mask(const std::vector<int> &ids) {
  uint64_t m = 0;
  for (int i : ids) if (i >= 0 && i < 64) m |= 1ULL << i;
  return m;
}

void XiaomiEspFlasher::handle_post_(AsyncWebServerRequest *req, const std::string &url, const std::string &body) {
  auto ok = [&](const char *msg) { this->send_json_(req, 200, std::string("{\"ok\":true,\"queued\":true,\"msg\":\"") + msg + "\"}"); };
  auto bad = [&](int code, const char *err, const std::string &msg) {
    this->send_json_(req, code, std::string("{\"ok\":false,\"error\":\"") + err + "\",\"msg\":\"" + json_escape(msg) + "\"}");
  };
  if (url == "/api/scan") { this->request_scan(); ok("scan requested"); return; }
  if (url == "/api/queue/all") { this->request_update_all(); ok("update all queued"); return; }
  if (url == "/api/firmware/check") { this->request_check_online(); ok("online check requested"); return; }
  if (url == "/api/firmware/upload") {
    Upload u = this->upload_;
    this->upload_ = Upload{};
    if (!u.active) { bad(400, "INVALID_REQUEST", "no upload data"); return; }
    if (u.failed) { bad(422, u.err.rfind("NOT_ENOUGH", 0) == 0 ? "NOT_ENOUGH_STORAGE" : "INVALID_IMAGE", u.err); return; }
    // hardware ids the user declares the image for (query ?hw=0,3,4 ; default: all LYWSD03MMC ids)
    std::vector<int> ids = {0, 3, 4, 5, 10, 14};
    if (req->hasArg("hw")) {
      ids.clear();
      std::string s = req->arg("hw");
      size_t p = 0;
      while (p < s.size()) {
        size_t q = s.find(',', p);
        ids.push_back(atoi(s.substr(p, q == std::string::npos ? std::string::npos : q - p).c_str()));
        if (q == std::string::npos) break;
        p = q + 1;
      }
    }
    std::string version = req->hasArg("version") ? req->arg("version") : "";
    ImageKind kind = ImageKind::USER_UPLOAD;
    std::string err;
    if (!this->store_.finish_write(u.name, version, kind, hw_ids_mask(ids), "upload", err)) { bad(422, "INVALID_IMAGE", err); return; }
    this->logf("Uploaded firmware %s (%u bytes) stored", u.name.c_str(), (unsigned) u.total);
    FirmwareInfo fi;
    this->store_.get_info(fi);
    this->needs_global_publish_ = true;
    this->send_json_(req, 200, "{\"ok\":true,\"id\":\"" + fi.id + "\",\"size\":" + std::to_string(fi.size) + ",\"crc32\":\"" + format_hex(fi.crc32) + "\"}");
    return;
  }
  if (url.rfind("/api/device/", 0) == 0) {
    std::string rest = url.substr(12);
    std::string macs = rest.substr(0, rest.find('/'));
    std::string tail = rest.size() > macs.size() ? rest.substr(macs.size() + 1) : "";
    uint64_t mac;
    if (!this->parse_mac_(macs, mac)) { bad(400, "INVALID_REQUEST", "bad mac"); return; }
    if (tail == "add") {
      // manual add (diagnostic): creates a record so identification can be requested even before advertising is seen
      char m[18];
      uint8_t b[6];
      for (int j = 0; j < 6; j++) b[j] = (mac >> (8 * (5 - j))) & 0xff;
      snprintf(m, sizeof(m), "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
      XiaomiDevice *d = this->get_or_create_device_(mac, m, BLE_ADDR_TYPE_PUBLIC);
      d->user_added = true;
      ok("device added");
      return;
    }
    if (tail == "forget") { this->forget_device(mac); ok("forgotten"); return; }
    if (tail == "alias") {
      std::string alias;
      json::parse_json(body, [&](JsonObject o) { alias = o["alias"] | ""; return true; });
      this->set_alias(mac, alias);
      ok("alias set");
      return;
    }
    Job job;
    std::string err;
    if (!this->parse_job_from_json_(tail, mac, body, job, err)) { bad(400, "INVALID_REQUEST", err); return; }
    if (job.type == JobType::FLASH) {
      // hard gate: the GUI must send the compat token = firmware id it saw in the confirmation screen
      LockGuard g(this->mutex_);
      const XiaomiDevice *d = this->find_device(mac);
      if (d == nullptr) { bad(404, "DEVICE_NOT_FOUND", "unknown device"); return; }
      if (!d->identified) { bad(409, "FW_UNKNOWN", "identify the device first"); return; }
      FirmwareInfo fw;
      if (!this->provider_->get_firmware_info(job.firmware_id, fw)) { bad(404, "INVALID_REQUEST", "unknown firmware id"); return; }
      CompatResult c = check_compatibility(d->hw, fw);
      if (!c.ok) { bad(409, error_to_string(c.code), c.message); return; }
      if (fw.size == 0) { bad(409, "NOT_ENOUGH_STORAGE", "firmware not downloaded"); return; }
    }
    if (this->session_active() && this->job_.type == JobType::FLASH) { bad(409, "BUSY", "an OTA is running"); return; }
    if (!this->request_job(job, err)) { bad(409, err.c_str(), "job rejected"); return; }
    ok(job_type_name(job.type));
    return;
  }
  req->send(404);
}

bool XiaomiEspFlasher::parse_job_from_json_(const std::string &tail, uint64_t mac, const std::string &body, Job &job, std::string &err) {
  job.mac = mac;
  if (tail == "identify" || tail == "connect") { job.type = JobType::IDENTIFY; return true; }
  if (tail == "disconnect") { err = "connections are per-job; nothing to disconnect"; return false; }
  if (tail == "settime") { job.type = JobType::SET_TIME; return true; }
  if (tail == "reboot") { job.type = JobType::REBOOT; return true; }
  if (tail == "defaults") { job.type = JobType::SET_DEFAULTS; return true; }
  if (tail == "activate") {
    job.type = JobType::ACTIVATE;
    json::parse_json(body, [&](JsonObject o) {
      std::string t = o["token"] | "", b = o["bind_key"] | "";
      if (!t.empty() && !b.empty() && job.keys.set_from_hex(t, b)) job.use_login = true;
      return true;
    });
    return true;
  }
  if (tail == "flash") {
    job.type = JobType::FLASH;
    bool confirmed = false;
    json::parse_json(body, [&](JsonObject o) {
      job.firmware_id = o["firmware"] | "";
      confirmed = o["confirm"] | false;
      std::string t = o["token"] | "", b = o["bind_key"] | "";
      if (!t.empty() && !b.empty() && job.keys.set_from_hex(t, b)) job.use_login = true;
      return true;
    });
    if (!confirmed) { err = "flash requires {\"confirm\":true}"; return false; }
    if (job.firmware_id.empty()) { err = "flash requires a firmware id"; return false; }
    return true;
  }
  if (tail == "name") {
    job.type = JobType::SET_NAME;
    json::parse_json(body, [&](JsonObject o) { job.name = o["name"] | ""; return true; });
    if (job.name.size() > 18) { err = "name max 18 chars"; return false; }
    return true;
  }
  if (tail == "pin") {
    job.type = JobType::SET_PIN;
    bool okp = false;
    json::parse_json(body, [&](JsonObject o) { job.pin = o["pin"] | 0; okp = o["pin"].is<int>(); return true; });
    if (!okp || job.pin > 999999) { err = "pin must be 0..999999"; return false; }
    return true;
  }
  if (tail == "config") {
    job.type = JobType::WRITE_CONFIG;
    bool parsed = json::parse_json(body, [&](JsonObject o) {
      if (o["cfg"].is<JsonObject>()) {
        JsonObject c = o["cfg"];
        DeviceConfig &g = job.cfg;
        g.ver = c["ver"] | 0;
        g.flg = c["flg"] | 0;
        g.flg2 = c["flg2"] | 0;
        g.temp_offset = (int8_t) (c["temp_offset"] | 0);
        g.humi_offset = (int8_t) (c["humi_offset"] | 0);
        g.advertising_interval = c["advertising_interval"] | 40;
        g.measure_interval = c["measure_interval"] | 4;
        g.rf_tx_power = c["rf_tx_power"] | 191;
        g.connect_latency = c["connect_latency"] | 49;
        g.lcd_tint = c["lcd_tint"] | 55;
        g.hver = c["hver"] | 0;
        g.av_meas_mem = c["av_meas_mem"] | 0;
        // named fields override raw flags if present
        if (c["advertising_type"].is<int>()) g.set_advertising_type(c["advertising_type"]);
        if (c["comfort_smiley"].is<bool>()) g.set_comfort_smiley(c["comfort_smiley"]);
        if (c["show_clock"].is<bool>()) g.set_show_clock(c["show_clock"]);
        if (c["temp_fahrenheit"].is<bool>()) g.set_temp_fahrenheit(c["temp_fahrenheit"]);
        if (c["show_battery"].is<bool>()) g.set_show_battery(c["show_battery"]);
        if (c["tx_measures"].is<bool>()) g.set_tx_measures(c["tx_measures"]);
        if (c["lp_measures"].is<bool>()) g.set_lp_measures(c["lp_measures"]);
        if (c["smiley"].is<int>()) g.set_smiley(c["smiley"]);
        if (c["adv_crypto"].is<bool>()) g.set_adv_crypto(c["adv_crypto"]);
        if (c["adv_flags"].is<bool>()) g.set_adv_flags(c["adv_flags"]);
        if (c["bt5phy"].is<bool>()) g.set_bt5phy(c["bt5phy"]);
        if (c["longrange"].is<bool>()) g.set_longrange(c["longrange"]);
        if (c["screen_off"].is<bool>()) g.set_screen_off(c["screen_off"]);
        g.valid = true;
        job.has_cfg = true;
      }
      if (o["comfort"].is<JsonObject>()) {
        JsonObject c = o["comfort"];
        job.comfort.temp_lo = (int16_t) lroundf((c["temp_lo"] | 21.0f) * 100);
        job.comfort.temp_hi = (int16_t) lroundf((c["temp_hi"] | 26.0f) * 100);
        job.comfort.humi_lo = (uint16_t) lroundf((c["humi_lo"] | 30.0f) * 100);
        job.comfort.humi_hi = (uint16_t) lroundf((c["humi_hi"] | 60.0f) * 100);
        job.comfort.valid = true;
        job.has_comfort = true;
      }
      if (o["sensor"].is<JsonObject>()) {
        JsonObject c = o["sensor"];
        job.sensor.temp_k = c["temp_k"] | 0;
        job.sensor.humi_k = c["humi_k"] | 0;
        job.sensor.temp_z = (int16_t) (c["temp_z"] | 0);
        job.sensor.humi_z = (int16_t) (c["humi_z"] | 0);
        job.sensor.valid = true;
        job.has_sensor = true;
      }
      if (o["trigger"].is<JsonObject>()) {
        JsonObject c = o["trigger"];
        job.trigger.temp_threshold = (int16_t) (c["temp_threshold"] | 2100);
        job.trigger.humi_threshold = (int16_t) (c["humi_threshold"] | 5000);
        job.trigger.temp_hysteresis = (int16_t) (c["temp_hysteresis"] | -55);
        job.trigger.humi_hysteresis = (int16_t) (c["humi_hysteresis"] | 0);
        job.trigger.rds_rpint = c["rds_rpint"] | 3600;
        job.trigger.rds_type = c["rds_type"] | 0;
        job.trigger.valid = true;
        job.has_trigger = true;
      }
      return true;
    });
    if (!parsed) { err = "invalid JSON"; return false; }
    if (!job.has_cfg && !job.has_comfort && !job.has_sensor && !job.has_trigger) { err = "nothing to write"; return false; }
    return true;
  }
  err = "unknown action '" + tail + "'";
  return false;
}

// ------------------------------------------------------------------------------------------------ JSON

void XiaomiEspFlasher::device_to_json_(JsonObject o, const XiaomiDevice &d, bool full) {
  o["mac"] = d.mac;
  o["name"] = d.name;
  o["alias"] = d.alias;
  o["display_name"] = d.display_name();
  o["rssi"] = d.rssi;
  o["last_seen"] = d.last_seen_epoch;
  o["last_seen_ago"] = d.last_seen_ms ? (millis() - d.last_seen_ms) / 1000 : -1;
  o["status"] = device_status_name(d.status);
  o["adv_format"] = adv_format_name(d.adv.format);
  o["adv_encrypted"] = d.adv.encrypted;
  if (d.adv.valid) {
    if (!std::isnan(d.adv.temperature)) o["temperature"] = d.adv.temperature;
    if (!std::isnan(d.adv.humidity)) o["humidity"] = d.adv.humidity;
    if (!std::isnan(d.adv.battery_pct)) o["battery"] = d.adv.battery_pct;
    if (!std::isnan(d.adv.battery_mv)) o["battery_mv"] = d.adv.battery_mv;
  } else if (d.live.valid) {
    o["temperature"] = d.live.temp / 100.0f;
    o["humidity"] = d.live.humi / 100.0f;
    o["battery_mv"] = d.live.vbat_mv;
  }
  o["identified"] = d.identified;
  o["model"] = d.hw.model.empty() ? d.model_hint : d.hw.model;
  o["firmware_kind"] = firmware_kind_name(d.hw.kind);
  o["firmware"] = d.installed_version();
  o["hardware"] = d.hw.hw_string;
  o["hw_id"] = d.hw.hw_id;
  o["hw_name"] = d.hw.hw_id == HW_UNKNOWN ? "unknown" : hw_id_name(d.hw.hw_id);
  o["update_available"] = d.update_available;
  o["latest_version"] = d.latest_version;
  o["latest_firmware"] = d.latest_firmware_id;
  o["compatible"] = d.compat.ok;
  o["compat_code"] = error_to_string(d.compat.code);
  o["compat_message"] = d.compat.message;
  o["last_error"] = d.last_error == Error::NONE ? "" : error_to_string(d.last_error);
  o["last_error_message"] = d.last_error_message;
  o["ota_result"] = d.ota_result;
  o["last_ota"] = d.last_ota_epoch;
  o["user_added"] = d.user_added;
  o["session"] = (this->dev_ == &d) ? session_state_name(this->state_) : "";
  if (!full)
    return;
  JsonArray w = o["compat_warnings"].to<JsonArray>();
  for (auto &s : d.compat.warnings) w.add(s);
  JsonArray sv = o["services"].to<JsonArray>();
  for (auto &s : d.services) sv.add(s);
  JsonObject dis = o["dis"].to<JsonObject>();
  dis["model"] = d.dis_model; dis["serial"] = d.dis_serial; dis["firmware"] = d.dis_firmware;
  dis["hardware"] = d.dis_hardware; dis["software"] = d.dis_software; dis["manufacturer"] = d.dis_manufacturer;
  o["requires_cloud_token"] = d.hw.requires_cloud_token;
  o["big_ota"] = d.hw.big_ota;
  o["i2c_sensor"] = d.hw.i2c_sensor >> 1;
  o["i2c_lcd"] = d.hw.i2c_lcd >> 1;
  o["device_name"] = d.device_name;
  o["device_time"] = d.device_time;
  o["device_time_set"] = d.device_time_set;
  o["mac_from_device"] = d.mac_from_device;
  o["pin_required"] = d.pin_required;
  o["identified_at"] = d.identified_epoch;
  if (d.dev_id.valid) {
    JsonObject di = o["dev_id"].to<JsonObject>();
    di["hw_version"] = d.dev_id.hw_version; di["sw_version"] = d.dev_id.sw_version;
    di["services"] = d.dev_id.services; di["dev_spec_data"] = d.dev_id.dev_spec_data;
  }
  if (d.live.valid) {
    JsonObject l = o["live"].to<JsonObject>();
    l["temperature"] = d.live.temp / 100.0f; l["humidity"] = d.live.humi / 100.0f; l["battery_mv"] = d.live.vbat_mv;
  }
  if (d.cfg.valid) {
    const DeviceConfig &c = d.cfg;
    JsonObject cfg = o["cfg"].to<JsonObject>();
    cfg["ver"] = c.ver; cfg["version"] = c.version_string();
    cfg["flg"] = c.flg; cfg["flg2"] = c.flg2;
    cfg["temp_offset"] = c.temp_offset; cfg["humi_offset"] = c.humi_offset;
    cfg["offsets_in_cfg"] = c.offsets_in_cfg();
    cfg["advertising_interval"] = c.advertising_interval; cfg["measure_interval"] = c.measure_interval;
    cfg["rf_tx_power"] = c.rf_tx_power; cfg["connect_latency"] = c.connect_latency; cfg["lcd_tint"] = c.lcd_tint;
    cfg["hver"] = c.hver; cfg["av_meas_mem"] = c.av_meas_mem;
    cfg["advertising_type"] = c.advertising_type();
    cfg["advertising_type_name"] = advertising_type_name(c.advertising_type(), c.ver);
    cfg["comfort_smiley"] = c.comfort_smiley(); cfg["show_clock"] = c.show_clock();
    cfg["temp_fahrenheit"] = c.temp_fahrenheit(); cfg["show_battery"] = c.show_battery();
    cfg["tx_measures"] = c.tx_measures(); cfg["lp_measures"] = c.lp_measures();
    cfg["smiley"] = c.smiley(); cfg["adv_crypto"] = c.adv_crypto(); cfg["adv_flags"] = c.adv_flags();
    cfg["bt5phy"] = c.bt5phy(); cfg["longrange"] = c.longrange(); cfg["screen_off"] = c.screen_off();
    cfg["big_ota"] = c.big_ota();
  }
  if (d.comfort.valid) {
    JsonObject cm = o["comfort"].to<JsonObject>();
    cm["temp_lo"] = d.comfort.temp_lo / 100.0f; cm["temp_hi"] = d.comfort.temp_hi / 100.0f;
    cm["humi_lo"] = d.comfort.humi_lo / 100.0f; cm["humi_hi"] = d.comfort.humi_hi / 100.0f;
  }
  if (d.sensor_cfg.valid) {
    JsonObject s = o["sensor"].to<JsonObject>();
    s["temp_k"] = d.sensor_cfg.temp_k; s["humi_k"] = d.sensor_cfg.humi_k;
    s["temp_z"] = d.sensor_cfg.temp_z; s["humi_z"] = d.sensor_cfg.humi_z;
    s["id"] = d.sensor_cfg.id; s["i2c_addr"] = d.sensor_cfg.i2c_addr; s["sensor_type"] = d.sensor_cfg.sensor_type;
  }
  if (d.trigger.valid) {
    JsonObject t = o["trigger"].to<JsonObject>();
    t["temp_threshold"] = d.trigger.temp_threshold; t["humi_threshold"] = d.trigger.humi_threshold;
    t["temp_hysteresis"] = d.trigger.temp_hysteresis; t["humi_hysteresis"] = d.trigger.humi_hysteresis;
    t["rds_rpint"] = d.trigger.rds_rpint; t["rds_type"] = d.trigger.rds_type; t["flg"] = d.trigger.flg;
  }
}

std::string XiaomiEspFlasher::json_device_(const XiaomiDevice &d, bool full) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  this->device_to_json_(o, d, full);
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string XiaomiEspFlasher::json_devices_() {
  // one small JsonDocument per device instead of one big one: keeps the peak heap low on the ESP32-C3
  std::string out = "[";
  {
    LockGuard g(this->mutex_);
    bool first = true;
    for (auto &d : this->devices_) {
      JsonDocument doc;
      JsonObject o = doc.to<JsonObject>();
      this->device_to_json_(o, *d, false);
      if (!first)
        out += ",";
      first = false;
      std::string one;
      serializeJson(doc, one);  // serializeJson(doc, std::string&) replaces the target, so append via a temp
      out += one;
    }
  }
  out += "]";
  return out;
}

std::string XiaomiEspFlasher::json_status_() {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["app"] = "Xiaomi ESP Flasher";
  o["esphome_version"] = ESPHOME_VERSION;
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  const char *model = ci.model == CHIP_ESP32C3 ? "ESP32-C3" : ci.model == CHIP_ESP32 ? "ESP32" : ci.model == CHIP_ESP32S3 ? "ESP32-S3" : "ESP32-x";
  o["chip"] = model;
  o["chip_revision"] = ci.revision;
  o["reset_reason"] = (int) esp_reset_reason();
  o["free_heap"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  o["min_free_heap"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  o["largest_block"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  o["uptime_s"] = millis() / 1000;
  o["time"] = this->epoch_now_();
  o["time_str"] = this->epoch_str_(this->epoch_now_());
  {
    char ipbuf[48];
    o["ip"] = network::get_ip_addresses()[0].str_to(ipbuf);
  }
#ifdef USE_WIFI
  if (wifi::global_wifi_component) {
    o["wifi_rssi"] = wifi::global_wifi_component->wifi_rssi();
  }
#endif
  o["ble_state"] = this->parent_ ? (this->parent_->scan_running() ? "scanning" : "idle") : "n/a";
  o["ble_client"] = this->client_ ? espbt::client_state_to_string(this->client_->state()) : "n/a";
  o["session"] = session_state_name(this->state_);
  o["job"] = job_type_name(this->job_.type);
  o["session_device"] = this->dev_ ? this->dev_->mac : "";
  o["device_count"] = this->devices_.size();
  o["last_scan"] = this->last_scan_epoch_;
  o["last_ota"] = this->last_ota_epoch_;
  o["last_ota_result"] = this->last_ota_result_;
  o["last_error"] = this->last_error_ == Error::NONE ? "" : error_to_string(this->last_error_);
  o["last_error_message"] = this->last_error_msg_;
  o["queue"] = this->job_queue_.size() + this->update_queue_.size();
  o["fwstore"] = this->store_.available();
  o["fwstore_capacity"] = this->store_.capacity();
  o["manifest_version"] = this->provider_ ? version_from_bcd(this->provider_->manifest_version()) : "";
  o["remote_manifest"] = this->remote_url_;
  o["test_device"] = this->test_mac_ ? format_hex(this->test_mac_) : "";
  o["log_seq"] = this->log_seq_;
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string XiaomiEspFlasher::json_firmware_() {
  std::string out = "[";
  if (this->provider_) {
    bool first = true;
    for (auto &fi : this->provider_->get_available_firmwares()) {
      JsonDocument doc;
      JsonObject o = doc.to<JsonObject>();
      o["id"] = fi.id;
      o["name"] = fi.name;
      o["version"] = fi.version;
      o["kind"] = image_kind_name(fi.kind);
      o["size"] = fi.size;
      o["crc32"] = fi.crc32 ? format_hex(fi.crc32) : "";
      o["source"] = fi.source;
      o["available"] = fi.size != 0;
      JsonArray ids = o["hw_ids"].to<JsonArray>();
      for (int i : fi.hw_ids) ids.add(i);
      JsonArray names = o["hw_names"].to<JsonArray>();
      for (int i : fi.hw_ids) names.add(hw_id_name(i));
      if (!first)
        out += ",";
      first = false;
      std::string one;
      serializeJson(doc, one);  // serializeJson(doc, std::string&) replaces the target, so append via a temp
      out += one;
    }
  }
  out += "]";
  return out;
}

std::string XiaomiEspFlasher::json_log_(uint32_t since) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  JsonArray arr = root["lines"].to<JsonArray>();
  {
    LockGuard g(this->mutex_);
    root["seq"] = this->log_seq_;
    for (auto &l : this->log_) {
      if (l.seq <= since) continue;
      JsonObject o = arr.add<JsonObject>();
      o["seq"] = l.seq; o["t"] = l.epoch; o["ms"] = l.ms; o["msg"] = l.text;
    }
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string XiaomiEspFlasher::json_ota_status_() {
  const OtaProgress &p = this->ota_.progress();
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["session"] = session_state_name(this->state_);
  o["job"] = job_type_name(this->job_.type);
  o["device"] = this->dev_ ? this->dev_->mac : "";
  o["state"] = ota_state_name(p.state);
  o["progress"] = p.percent();
  o["bytes_sent"] = p.bytes_sent; o["total_bytes"] = p.bytes_total;
  o["blocks_sent"] = p.blocks_sent; o["blocks_total"] = p.blocks_total;
  o["speed"] = p.bytes_per_second(); o["elapsed_ms"] = p.elapsed_ms; o["retries"] = p.retries;
  o["device_status"] = p.device_status;
  o["last_error"] = p.error == Error::NONE ? "" : error_to_string(p.error);
  o["message"] = p.message;
  o["target_version"] = this->target_version_;
  std::string out;
  serializeJson(doc, out);
  return out;
}

// ------------------------------------------------------------------------------------------------ SSE

void XiaomiEspFlasher::sse_free_ctx_(void *ctx) {
  auto *c = (SseClient *) ctx;
  if (c != nullptr) c->fd.store(0);  // deleted later on the main loop
}

void XiaomiEspFlasher::handle_sse_(AsyncWebServerRequest *request) {
  httpd_req_t *req = *request;
  httpd_resp_set_status(req, HTTPD_200);
  httpd_resp_set_type(req, "text/event-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send_chunk(req, "\r\n", 2);
  auto *c = new SseClient();
  c->hd = req->handle;
  c->fd.store(httpd_req_to_sockfd(req));
  req->sess_ctx = c;
  req->free_ctx = &XiaomiEspFlasher::sse_free_ctx_;
  {
    LockGuard g(this->mutex_);
    if (this->sse_.size() >= 4) {  // keep the oldest 3, drop the oldest
      SseClient *old = this->sse_.front();
      this->sse_.erase(this->sse_.begin());
      if (old->fd.load() != 0) httpd_sess_trigger_close((httpd_handle_t) old->hd, old->fd.load());
      delete old;
    }
    this->sse_.push_back(c);
  }
  ESP_LOGD(TAG, "SSE client connected (fd %d)", c->fd.load());
}

static bool sse_send(SseClient *c, const std::string &payload) {
  // one HTTP chunk: hex length CRLF data CRLF ; data = "data: <json>\n\n"
  std::string data = "data: " + payload + "\n\n";
  char head[16];
  int hl = snprintf(head, sizeof(head), "%X\r\n", (unsigned) data.size());
  std::string frame(head, hl);
  frame += data;
  frame += "\r\n";
  int fd = c->fd.load();
  if (fd == 0) return false;
  int r = httpd_socket_send((httpd_handle_t) c->hd, fd, frame.data(), frame.size(), 0);
  return r >= 0 || r == HTTPD_SOCK_ERR_TIMEOUT;
}

void XiaomiEspFlasher::sse_pump_() {
  std::vector<std::string> pending;
  std::vector<SseClient *> clients;
  {
    LockGuard g(this->mutex_);
    if (this->sse_.empty()) { this->events_.clear(); return; }
    while (!this->events_.empty()) { pending.push_back(std::move(this->events_.front())); this->events_.pop_front(); }
    // drop dead clients
    for (auto it = this->sse_.begin(); it != this->sse_.end();) {
      if ((*it)->fd.load() == 0) { delete *it; it = this->sse_.erase(it); } else { clients.push_back(*it); ++it; }
    }
  }
  static uint32_t last_ping = 0;
  bool ping = millis() - last_ping > 15000;
  if (pending.empty() && !ping) return;
  if (ping) { last_ping = millis(); pending.push_back("{\"type\":\"ping\",\"t\":" + std::to_string(millis() / 1000) + "}"); }
  for (auto *c : clients)
    for (auto &e : pending)
      if (!sse_send(c, e)) { c->fd.store(0); break; }
}

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
