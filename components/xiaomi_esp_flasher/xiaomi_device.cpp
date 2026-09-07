#include "xiaomi_device.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace xiaomi_esp_flasher {

const char *device_status_name(DeviceStatus s) {
  switch (s) {
    case DeviceStatus::DISCOVERED: return "discovered";
    case DeviceStatus::IDENTIFYING: return "identifying";
    case DeviceStatus::IDENTIFIED: return "identified";
    case DeviceStatus::CONNECTING: return "connecting";
    case DeviceStatus::CONNECTED: return "connected";
    case DeviceStatus::UPDATING: return "updating";
    case DeviceStatus::UPDATED: return "updated";
    case DeviceStatus::FAILED: return "failed";
    case DeviceStatus::OFFLINE: return "offline";
  }
  return "?";
}

const char *adv_format_name(AdvFormat f) {
  switch (f) {
    case AdvFormat::BTHOME_V2: return "BTHome v2";
    case AdvFormat::PVVX_CUSTOM: return "PVVX custom";
    case AdvFormat::ATC1441: return "ATC1441";
    case AdvFormat::MIJIA: return "MiBeacon";
    case AdvFormat::BTHOME_V1: return "BTHome v1";
    case AdvFormat::UNKNOWN: return "unknown";
    default: return "none";
  }
}

std::string json_escape(const std::string &s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char ch : s) {
    switch (ch) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((unsigned char) ch < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", ch); o += b; }
        else o += ch;
    }
  }
  return o;
}

static inline int16_t i16le(const uint8_t *p) { return (int16_t) (p[0] | (p[1] << 8)); }
static inline uint16_t u16le(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }

// Mi device ids of interest (TelinkMiFlasher.html tabid2name)
static const char *mi_id_name(uint16_t id) {
  switch (id) {
    case 0x055B: return "LYWSD03MMC";
    case 0x045B: return "LYWSD02";
    case 0x0387: return "MHO-C401";
    case 0x066F: return "CGDK2";
    case 0x0347: return "CGG1";
    case 0x0B48: return "CGG1-ENCRYPTED";
    case 0x2832: return "MJWSD05MMC";
    case 0x55B5: return "MJWSD06MMC";
    case 0x01AA: return "LYWSDCGQ";
    default: return nullptr;
  }
}

bool parse_advertisement(uint16_t uuid16, const uint8_t *d, size_t n, AdvMeasurement &out, std::string &model_hint,
                         std::string &mi_fw_hint) {
  if (uuid16 == 0xFCD2) {  // BTHome (bthome_beacon.c): byte0 = device info; bit0 encryption, bits5-7 version
    if (n < 1)
      return false;
    out.format = AdvFormat::BTHOME_V2;
    out.encrypted = d[0] & 0x01;
    if (out.encrypted) {
      out.valid = true;
      return true;
    }
    size_t i = 1;
    while (i < n) {
      uint8_t id = d[i++];
      switch (id) {
        case 0x00: if (i + 1 > n) return true; out.counter = d[i]; i += 1; break;                         // packet id
        case 0x01: if (i + 1 > n) return true; out.battery_pct = d[i]; i += 1; break;                     // battery %
        case 0x02: if (i + 2 > n) return true; out.temperature = i16le(d + i) / 100.0f; i += 2; break;     // temp 0.01
        case 0x03: if (i + 2 > n) return true; out.humidity = u16le(d + i) / 100.0f; i += 2; break;        // humi 0.01
        case 0x0C: if (i + 2 > n) return true; out.battery_mv = u16le(d + i); i += 2; break;               // voltage 0.001 V
        case 0x2E: if (i + 1 > n) return true; out.humidity = d[i]; i += 1; break;                        // humi %
        case 0x45: if (i + 2 > n) return true; out.temperature = i16le(d + i) / 10.0f; i += 2; break;      // temp 0.1
        case 0x10: case 0x11: case 0x0F: case 0x15: case 0x16: case 0x21: i += 1; break;                  // binary sensors
        case 0x3A: i += 1; break;                                                                         // button
        case 0x3C: case 0x3D: i += 2; break;
        case 0x3E: i += 4; break;
        case 0x50: i += 4; break;
        default: out.valid = true; return true;  // unknown object length: stop parsing
      }
    }
    out.valid = !std::isnan(out.temperature) || !std::isnan(out.humidity) || !std::isnan(out.battery_pct);
    return true;
  }
  if (uuid16 == 0x181A) {
    if (n == 15) {  // pvvx custom: mac[6] LE, temp i16 x0.01, humi u16 x0.01, vbat u16 mV, bat u8, cnt u8, flags u8
      out.format = AdvFormat::PVVX_CUSTOM;
      out.temperature = i16le(d + 6) / 100.0f;
      out.humidity = u16le(d + 8) / 100.0f;
      out.battery_mv = u16le(d + 10);
      out.battery_pct = d[12];
      out.counter = d[13];
      out.valid = true;
      return true;
    }
    if (n == 13) {  // atc1441: mac[6] BE, temp i16 BE x0.1, humi u8, bat u8, vbat u16 BE, cnt u8
      out.format = AdvFormat::ATC1441;
      out.temperature = (int16_t) ((d[6] << 8) | d[7]) / 10.0f;
      out.humidity = d[8];
      out.battery_pct = d[9];
      out.battery_mv = (d[10] << 8) | d[11];
      out.counter = d[12];
      out.valid = true;
      return true;
    }
    if (n == 11 || n == 19) {  // pvvx custom encrypted (11) / bthome-like
      out.format = AdvFormat::PVVX_CUSTOM;
      out.encrypted = true;
      out.valid = true;
      return true;
    }
    out.format = AdvFormat::UNKNOWN;
    return false;
  }
  if (uuid16 == 0x181C) {
    out.format = AdvFormat::BTHOME_V1;
    out.valid = true;
    return true;
  }
  if (uuid16 == 0xFE95) {  // MiBeacon (catchAdvertisement)
    if (n < 5)
      return false;
    out.format = AdvFormat::MIJIA;
    uint16_t ctrl = u16le(d);
    size_t i = 2;
    if ((ctrl & 0xf000) < 0x2000)
      return false;
    uint16_t dev_id = u16le(d + i);
    out.counter = d[i + 2];
    i += 3;
    const char *nm = mi_id_name(dev_id);
    if (nm != nullptr)
      model_hint = nm;
    if (ctrl & 0x10) i += 6;        // MAC
    if (ctrl & 0x20) {              // capability
      if (i >= n) return true;
      uint8_t cap = d[i++];
      if (cap & 0x20) i += 2;
    }
    if (ctrl & 0x40) {
      if (ctrl & 8) {
        out.encrypted = true;
        out.valid = true;
        return true;
      }
      if (i + 3 > n) return true;
      uint16_t data_id = u16le(d + i);
      uint8_t dl = d[i + 2];
      i += 3;
      if (i + dl > n) return true;
      switch (data_id) {
        case 0x1004: if (dl == 2) out.temperature = i16le(d + i) / 10.0f; break;
        case 0x1006: if (dl == 2) out.humidity = i16le(d + i) / 10.0f; break;
        case 0x100A: if (dl >= 1) out.battery_pct = d[i]; break;
        case 0x100D: if (dl == 4) { out.temperature = i16le(d + i) / 10.0f; out.humidity = i16le(d + i + 2) / 10.0f; } break;
        default: break;
      }
      out.valid = true;
    }
    return true;
  }
  return false;
}

std::string XiaomiDevice::installed_version() const {
  if (this->hw.kind == FirmwareKind::CUSTOM_PVVX && this->cfg.valid)
    return this->cfg.version_string();
  if (!this->hw.fw_version.empty())
    return this->hw.fw_version;
  if (!this->dis_firmware.empty())
    return this->dis_firmware;
  return "";
}

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
