#pragma once
// FirmwareProvider: where firmware images and their metadata come from.
//   * ManifestProvider  – parses a pvvx firmware.json (bundled copy at build time, or fetched from GitHub)
//   * BundledImages     – image bytes compiled into the ESP32 application (from firmware/ at build time)
//   * FirmwareStore     – one uploaded/downloaded image in flash
//   * RemoteGithubFirmwareProvider – fetches firmware.json and downloads .bin files over HTTPS into the store
// The hub merges all sources into one list and uses compatibility.cpp to pick the latest for a device.
#include "esphome/core/defines.h"
#ifdef USE_ESP32
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "compatibility.h"
#include "telink_image.h"
#include "firmware_store.h"

namespace esphome {
namespace xiaomi_esp_flasher {

struct BundledImage {
  const char *name;
  const uint8_t *data;
  size_t size;
};

class FirmwareProvider {
 public:
  virtual ~FirmwareProvider() = default;
  virtual const char *name() const = 0;
  virtual std::vector<FirmwareInfo> get_available_firmwares() = 0;
  virtual bool get_firmware_info(const std::string &id, FirmwareInfo &out) = 0;
  // latest *custom* image usable for this hardware (nullptr semantics: false)
  virtual bool get_latest_for_device(const HardwareInfo &hw, FirmwareInfo &out) = 0;
  // access to the image bytes; false when not locally available (e.g. remote not downloaded yet)
  virtual bool open_image(const std::string &id, ImageReader &reader, size_t &size) = 0;
};

// Parses pvvx firmware.json: {"version": 89, "betaver": 96, "custom": [...], "betafw": [...], "original": [...], "signed": [...]}
struct ManifestEntry {
  std::string file;      // e.g. "bin/ATC_v59.bin" or full URL
  std::string name;      // basename
  ImageKind kind;
  std::string version;   // "5.9" for custom/beta (from version byte), or parsed from the file name for originals
  int version_num;       // custom: BCD byte value (0x59 = 89); original: 0
  std::vector<int> hw_ids;
};
bool parse_manifest(const std::string &json, std::vector<ManifestEntry> &out, std::string &err);
std::string basename_of(const std::string &path);
std::string version_from_bcd(int v);

class LocalFirmwareProvider : public FirmwareProvider {
 public:
  LocalFirmwareProvider(const char *manifest_json, const std::vector<BundledImage> &images, FirmwareStore *store)
      : manifest_json_(manifest_json), images_(images), store_(store) {}
  void setup();
  const char *name() const override { return "local"; }
  std::vector<FirmwareInfo> get_available_firmwares() override;
  bool get_firmware_info(const std::string &id, FirmwareInfo &out) override;
  bool get_latest_for_device(const HardwareInfo &hw, FirmwareInfo &out) override;
  bool open_image(const std::string &id, ImageReader &reader, size_t &size) override;
  // remote manifest (same format) merged in by the remote provider
  void set_remote_entries(std::vector<ManifestEntry> entries, const std::string &base_url);
  // replace the manifest at runtime (pushed by the browser from GitHub); returns false if it does not parse
  bool set_manifest(const std::string &json, std::string &err);
  const std::vector<ManifestEntry> &remote_entries() const { return this->remote_; }
  const std::string &remote_base_url() const { return this->remote_base_; }
  int manifest_version() const { return this->manifest_version_; }

 protected:
  void build_info_(const ManifestEntry &e, FirmwareInfo &fi, const std::string &prefix, const std::string &source);
  const BundledImage *find_bundled_(const std::string &name);
  const char *manifest_json_;
  std::string runtime_manifest_;
  std::vector<BundledImage> images_;
  FirmwareStore *store_;
  std::vector<ManifestEntry> bundled_;
  std::vector<ManifestEntry> remote_;
  std::string remote_base_;
  int manifest_version_{0};
};

#ifdef USE_XIAOMI_FLASHER_REMOTE
// Downloads firmware.json and images from GitHub over HTTPS (esp_http_client + certificate bundle).
class RemoteGithubFirmwareProvider {
 public:
  explicit RemoteGithubFirmwareProvider(const std::string &manifest_url) : manifest_url_(manifest_url) {}
  // blocking; run from the main loop with the watchdog fed – manifest is ~3 KB
  bool fetch_manifest(std::string &json, std::string &err);
  // blocking download straight into the FirmwareStore
  bool download_image(const std::string &url, FirmwareStore &store, const ManifestEntry &e, std::string &err,
                      const std::function<void(size_t, size_t)> &progress);
  const std::string &manifest_url() const { return this->manifest_url_; }

 protected:
  std::string manifest_url_;
};
#endif

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
#endif
