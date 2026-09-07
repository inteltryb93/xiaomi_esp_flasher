#include "firmware_provider.h"
#ifdef USE_ESP32
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/json/json_util.h"
#include <cstring>
#ifdef USE_XIAOMI_FLASHER_REMOTE
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#endif

namespace esphome {
namespace xiaomi_esp_flasher {

static const char *const TAG = "xiaomi_flasher.fw";

std::string basename_of(const std::string &path) {
  size_t p = path.find_last_of('/');
  return p == std::string::npos ? path : path.substr(p + 1);
}

std::string version_from_bcd(int v) { return std::to_string((v >> 4) & 0x0f) + "." + std::to_string(v & 0x0f); }

// Original images carry the version in the file name: Original_OTA_Xiaomi_LYWSD03MMC_v1.0.0_0130.bin
static std::string version_from_filename(const std::string &name) {
  size_t p = name.find("_v");
  if (p == std::string::npos)
    return "";
  size_t e = name.find(".bin", p);
  std::string v = name.substr(p + 2, e == std::string::npos ? std::string::npos : e - p - 2);
  return v;
}

static void add_entries(JsonArray arr, ImageKind kind, const std::string &version, int version_num,
                        std::vector<ManifestEntry> &out) {
  int idx = 0;
  for (JsonVariant v : arr) {
    std::string f = v.as<const char *>() ? v.as<const char *>() : "";
    if (!f.empty() && f != "?" && f != "/") {
      ManifestEntry *e = nullptr;
      for (auto &x : out)
        if (x.file == f && x.kind == kind)
          e = &x;
      if (e == nullptr) {
        out.emplace_back();
        e = &out.back();
        e->file = f;
        e->name = basename_of(f);
        e->kind = kind;
        e->version = version.empty() ? version_from_filename(e->name) : version;
        e->version_num = version_num;
      }
      e->hw_ids.push_back(idx);
    }
    idx++;
  }
}

bool parse_manifest(const std::string &json, std::vector<ManifestEntry> &out, std::string &err) {
  out.clear();
  JsonDocument doc = json::parse_json(json);
  if (doc.isNull() || !doc.is<JsonObject>()) {
    err = "invalid firmware.json";
    return false;
  }
  JsonObject root = doc.as<JsonObject>();
  int ver = root["version"] | 0;
  int betaver = root["betaver"] | 0;
  if (ver == 0) {
    err = "firmware.json has no version";
    return false;
  }
  if (root["custom"].is<JsonArray>())
    add_entries(root["custom"].as<JsonArray>(), ImageKind::CUSTOM, version_from_bcd(ver), ver, out);
  if (root["betafw"].is<JsonArray>() && betaver)
    add_entries(root["betafw"].as<JsonArray>(), ImageKind::BETA, version_from_bcd(betaver), betaver, out);
  if (root["original"].is<JsonArray>())
    add_entries(root["original"].as<JsonArray>(), ImageKind::ORIGINAL, "", 0, out);
  if (root["signed"].is<JsonArray>())
    add_entries(root["signed"].as<JsonArray>(), ImageKind::SIGNED, "", 0, out);
  return true;
}

// ------------------------------------------------------------------ LocalFirmwareProvider

void LocalFirmwareProvider::setup() {
  std::string err;
  if (!parse_manifest(this->manifest_json_, this->bundled_, err)) {
    ESP_LOGE(TAG, "bundled manifest: %s", err.c_str());
    return;
  }
  JsonDocument doc = json::parse_json(this->manifest_json_);
  this->manifest_version_ = doc["version"] | 0;
  int with_image = 0;
  for (auto &e : this->bundled_)
    if (this->find_bundled_(e.name) != nullptr)
      with_image++;
  ESP_LOGI(TAG, "bundled manifest version %d (%s): %u entries, %d with image bytes", this->manifest_version_,
           version_from_bcd(this->manifest_version_).c_str(), (unsigned) this->bundled_.size(), with_image);
}

const BundledImage *LocalFirmwareProvider::find_bundled_(const std::string &name) {
  for (auto &img : this->images_)
    if (name == img.name)
      return &img;
  return nullptr;
}

void LocalFirmwareProvider::build_info_(const ManifestEntry &e, FirmwareInfo &fi, const std::string &prefix,
                                        const std::string &source) {
  fi = FirmwareInfo{};
  fi.id = prefix + e.name;
  fi.name = e.name;
  fi.version = e.version;
  fi.version_num = e.version_num;
  fi.kind = e.kind;
  fi.hw_ids = e.hw_ids;
  fi.source = source;
  const BundledImage *img = this->find_bundled_(e.name);
  if (img != nullptr) {
    fi.size = img->size;
    TelinkImageInfo info;
    auto rd = [img](size_t off, uint8_t *buf, size_t len) {
      if (off + len > img->size) return false;
      memcpy(buf, img->data + off, len);
      return true;
    };
    if (validate_telink_image(img->size, rd, info, MAX_EXT_OTA_SIZE) == "ok")
      fi.crc32 = info.crc_stored;
  }
}

std::vector<FirmwareInfo> LocalFirmwareProvider::get_available_firmwares() {
  std::vector<FirmwareInfo> out;
  for (auto &e : this->bundled_) {
    FirmwareInfo fi;
    this->build_info_(e, fi, "bundled:", "bundled");
    if (fi.size == 0)
      fi.source = "manifest only (no image bundled)";
    out.push_back(fi);
  }
  for (auto &e : this->remote_) {
    FirmwareInfo fi;
    this->build_info_(e, fi, "remote:", this->remote_base_ + e.file);
    fi.size = 0;  // unknown until downloaded
    // if the store holds this exact file, expose it as downloaded
    if (this->store_ != nullptr && this->store_->has_image() && e.name == this->store_->header().name) {
      fi.size = this->store_->header().size;
      fi.crc32 = this->store_->header().crc32;
    }
    out.push_back(fi);
  }
  if (this->store_ != nullptr) {
    FirmwareInfo fi;
    if (this->store_->get_info(fi))
      out.push_back(fi);
  }
  return out;
}

bool LocalFirmwareProvider::get_firmware_info(const std::string &id, FirmwareInfo &out) {
  for (auto &fi : this->get_available_firmwares())
    if (fi.id == id) {
      out = fi;
      return true;
    }
  return false;
}

bool LocalFirmwareProvider::get_latest_for_device(const HardwareInfo &hw, FirmwareInfo &out) {
  // newest CUSTOM image (bundled or stored) compatible with the hardware; remote-only entries count as
  // "known latest version" but are not flashable until downloaded
  bool found = false;
  FirmwareInfo best;
  for (auto &fi : this->get_available_firmwares()) {
    if (fi.kind != ImageKind::CUSTOM)
      continue;
    if (!is_firmware_compatible(hw, fi) && fi.size != 0)
      continue;
    bool listed = false;
    for (int id : fi.hw_ids)
      if (id == hw.hw_id)
        listed = true;
    if (!listed)
      continue;
    if (!found || compare_versions(fi.version, best.version) > 0 || (compare_versions(fi.version, best.version) == 0 && best.size == 0 && fi.size != 0)) {
      best = fi;
      found = true;
    }
  }
  if (found)
    out = best;
  return found;
}

bool LocalFirmwareProvider::open_image(const std::string &id, ImageReader &reader, size_t &size) {
  if (id.rfind("bundled:", 0) == 0) {
    const BundledImage *img = this->find_bundled_(id.substr(8));
    if (img == nullptr)
      return false;
    reader = [img](size_t off, uint8_t *buf, size_t len) {
      if (off + len > img->size) return false;
      memcpy(buf, img->data + off, len);
      return true;
    };
    size = img->size;
    return true;
  }
  if (this->store_ != nullptr && this->store_->has_image()) {
    std::string nm = this->store_->header().name;
    if (id == "store:" + nm || id == "remote:" + nm) {
      reader = this->store_->reader();
      size = this->store_->header().size;
      return true;
    }
  }
  return false;
}

void LocalFirmwareProvider::set_remote_entries(std::vector<ManifestEntry> entries, const std::string &base_url) {
  this->remote_ = std::move(entries);
  this->remote_base_ = base_url;
}

#ifdef USE_XIAOMI_FLASHER_REMOTE
// ------------------------------------------------------------------ RemoteGithubFirmwareProvider

struct DlCtx {
  FirmwareStore *store;
  std::string *buf;
  std::string err;
  size_t total;
  size_t received;
  const std::function<void(size_t, size_t)> *progress;
  bool failed;
};

static esp_err_t http_event(esp_http_client_event_t *evt) {
  auto *ctx = (DlCtx *) evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA && ctx != nullptr && !ctx->failed) {
    if (ctx->buf != nullptr) {
      if (ctx->buf->size() + evt->data_len > 16384) {
        ctx->err = "manifest too large";
        ctx->failed = true;
        return ESP_FAIL;
      }
      ctx->buf->append((const char *) evt->data, evt->data_len);
    } else if (ctx->store != nullptr) {
      if (ctx->total == 0) {
        ctx->err = "no Content-Length";
        ctx->failed = true;
        return ESP_FAIL;
      }
      if (!ctx->store->write_chunk((const uint8_t *) evt->data, evt->data_len, ctx->err)) {
        ctx->failed = true;
        return ESP_FAIL;
      }
      ctx->received += evt->data_len;
      if (ctx->progress != nullptr && *ctx->progress)
        (*ctx->progress)(ctx->received, ctx->total);
    }
    App.feed_wdt();
  } else if (evt->event_id == HTTP_EVENT_ON_HEADER && ctx != nullptr && ctx->store != nullptr) {
    if (strcasecmp(evt->header_key, "Content-Length") == 0) {
      ctx->total = strtoul(evt->header_value, nullptr, 10);
      if (!ctx->store->begin_write(ctx->total, ctx->err)) {
        ctx->failed = true;
        return ESP_FAIL;
      }
    }
  }
  return ESP_OK;
}

static bool http_get(const std::string &url, DlCtx &ctx, std::string &err) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.event_handler = http_event;
  cfg.user_data = &ctx;
  cfg.timeout_ms = 15000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 1024;
  cfg.max_redirection_count = 3;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    err = "http client init failed";
    return false;
  }
  esp_err_t e = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (ctx.failed) {
    err = ctx.err;
    return false;
  }
  if (e != ESP_OK) {
    err = std::string("http error ") + esp_err_to_name(e);
    return false;
  }
  if (status != 200) {
    err = "http status " + std::to_string(status);
    return false;
  }
  return true;
}

bool RemoteGithubFirmwareProvider::fetch_manifest(std::string &json, std::string &err) {
  json.clear();
  DlCtx ctx{nullptr, &json, "", 0, 0, nullptr, false};
  ESP_LOGI(TAG, "fetching %s", this->manifest_url_.c_str());
  return http_get(this->manifest_url_, ctx, err);
}

bool RemoteGithubFirmwareProvider::download_image(const std::string &url, FirmwareStore &store, const ManifestEntry &e,
                                                  std::string &err, const std::function<void(size_t, size_t)> &progress) {
  DlCtx ctx{&store, nullptr, "", 0, 0, &progress, false};
  ESP_LOGI(TAG, "downloading %s", url.c_str());
  if (!http_get(url, ctx, err)) {
    store.abort_write();
    return false;
  }
  uint64_t ids = 0;
  for (int id : e.hw_ids)
    if (id >= 0 && id < 64)
      ids |= 1ULL << id;
  return store.finish_write(e.name, e.version, e.kind, ids, url, err);
}
#endif

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
