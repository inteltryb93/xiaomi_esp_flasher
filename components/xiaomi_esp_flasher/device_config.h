#pragma once
// DeviceConfig: the configuration model of the pvvx custom firmware (cfg_t in src/app.h) plus the
// auxiliary blocks the TelinkMiFlasher GUI edits (comfort, sensor calibration, trigger, name, time).
// Encoding/decoding mirrors CustomBlkParse() / SendCustomConfig() / SendCmf() / setSensCfg() / sendTrg().
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace xiaomi_esp_flasher {

// Command ids: src/cmd_parser.h
enum CmdId : uint8_t {
  CMD_ID_DEV_ID = 0x00,
  CMD_ID_DNAME = 0x01,
  CMD_ID_GDEVS = 0x02,
  CMD_ID_SEN_ID = 0x05,
  CMD_ID_DEV_MAC = 0x10,
  CMD_ID_MI_KALL = 0x15,
  CMD_ID_BKEY = 0x18,
  CMD_ID_COMFORT = 0x20,
  CMD_ID_EXTDATA = 0x22,
  CMD_ID_UTC_TIME = 0x23,
  CMD_ID_TADJUST = 0x24,
  CMD_ID_CFS = 0x25,
  CMD_ID_CFS_DEF = 0x26,
  CMD_ID_MEASURE = 0x33,
  CMD_ID_TRG = 0x44,
  CMD_ID_CFG = 0x55,
  CMD_ID_CFG_DEF = 0x56,
  CMD_ID_PINCODE = 0x70,
  CMD_ID_MTU = 0x71,
  CMD_ID_REBOOT = 0x72,
  CMD_ID_SET_OTA = 0x73,
};

// Advertising formats – cfg.flg bits 0..1 (src/app.h: "0 - atc1441, 1 - Custom (pvvx), 2 - Mi, 3 - BTHome")
enum class AdvertisingType : uint8_t { ATC1441 = 0, PVVX = 1, MIJIA = 2, BTHOME = 3 };
const char *advertising_type_name(uint8_t v, uint8_t fw_ver);

struct DeviceConfig {
  // ---- raw cfg_t (CMD_ID_CFG) ----
  uint8_t ver{0};  // firmware version BCD, read-only
  uint8_t flg{0};
  uint8_t flg2{0};
  int8_t temp_offset{0};   // ver < 0x47: x0.1 °C ; ver >= 0x47: flg3 bits (adv delay / date flags)
  int8_t humi_offset{0};   // ver < 0x47: x0.1 %  ; ver >= 0x47: event_adv_cnt
  uint8_t advertising_interval{40};  // x62.5 ms
  uint8_t measure_interval{4};       // x adv interval
  uint8_t rf_tx_power{191};
  uint8_t connect_latency{49};       // (+1) x 20 ms
  uint8_t lcd_tint{55};              // x0.05 s
  uint8_t hver{0};                   // read-only
  uint8_t av_meas_mem{0};
  uint8_t raw_len{0};                // bytes received after the cmd id
  bool valid{false};

  // ---- decoded convenience accessors (cfg.flg / flg2 bit layout for LCD devices) ----
  uint8_t advertising_type() const { return flg & 3; }
  bool comfort_smiley() const { return flg & 4; }
  bool show_clock() const { return flg & 8; }        // show_time_smile (LYWSD03MMC "Clock")
  bool temp_fahrenheit() const { return flg & 16; }
  bool show_battery() const { return flg & 32; }
  bool tx_measures() const { return flg & 64; }
  bool lp_measures() const { return flg & 128; }
  uint8_t smiley() const { return flg2 & 7; }
  bool adv_crypto() const { return flg2 & 0x08; }
  bool adv_flags() const { return flg2 & 0x10; }
  bool bt5phy() const { return flg2 & 0x20; }
  bool longrange() const { return flg2 & 0x40; }
  bool screen_off() const { return flg2 & 0x80; }

  void set_bit(uint8_t &byte, uint8_t mask, bool on) { byte = on ? (byte | mask) : (byte & ~mask); }
  void set_advertising_type(uint8_t t) { flg = (flg & ~3) | (t & 3); }
  void set_comfort_smiley(bool v) { set_bit(flg, 4, v); }
  void set_show_clock(bool v) { set_bit(flg, 8, v); }
  void set_temp_fahrenheit(bool v) { set_bit(flg, 16, v); }
  void set_show_battery(bool v) { set_bit(flg, 32, v); }
  void set_tx_measures(bool v) { set_bit(flg, 64, v); }
  void set_lp_measures(bool v) { set_bit(flg, 128, v); }
  void set_smiley(uint8_t s) { flg2 = (flg2 & ~7) | (s & 7); }
  void set_adv_crypto(bool v) { set_bit(flg2, 0x08, v); }
  void set_adv_flags(bool v) { set_bit(flg2, 0x10, v); }
  void set_bt5phy(bool v) { set_bit(flg2, 0x20, v); }
  void set_longrange(bool v) { set_bit(flg2, 0x40, v); }
  void set_screen_off(bool v) { set_bit(flg2, 0x80, v); }

  bool offsets_in_cfg() const { return ver < 0x47; }  // >= 4.7 offsets live in sensor cfg (CMD_ID_CFS)
  bool big_ota() const { return ver > 0x45; }
  std::string version_string() const { return std::to_string(ver >> 4) + "." + std::to_string(ver & 0x0f); }

  // Decode a 0x55 notification (CustomBlkParse). Returns false if too short.
  bool decode(const uint8_t *data, size_t len);
  // Encode a 0x55 write (SendCustomConfig): "55" + 9 or 11 bytes depending on ver.
  std::vector<uint8_t> encode() const;
  void clamp();
};

// CMD_ID_COMFORT (0x20)
struct ComfortConfig {
  bool valid{false};
  int16_t temp_lo{2100}, temp_hi{2600};  // x0.01 °C
  uint16_t humi_lo{3000}, humi_hi{6000}; // x0.01 %
  bool decode(const uint8_t *data, size_t len);
  std::vector<uint8_t> encode() const;
};

// CMD_ID_CFS (0x25): sensor calibration; from fw 4.7 the temp/humi offsets are here (T = RegT*Tk/65536 + Tz)
struct SensorConfig {
  bool valid{false};
  uint32_t temp_k{0}, humi_k{0};  // x0.01
  int16_t temp_z{0}, humi_z{0};   // x0.01 -> these are the "offsets" of fw >= 4.7
  uint32_t id{0};
  uint8_t i2c_addr{0};
  uint8_t sensor_type{0xff};
  bool decode(const uint8_t *data, size_t len);
  std::vector<uint8_t> encode() const;
};

// CMD_ID_TRG (0x44)
struct TriggerConfig {
  bool valid{false};
  int16_t temp_threshold{2100}, humi_threshold{5000};
  int16_t temp_hysteresis{-55}, humi_hysteresis{0};
  uint16_t rds_rpint{3600};
  uint8_t rds_type{0};
  uint8_t flg{0};
  bool decode(const uint8_t *data, size_t len, uint8_t fw_ver);
  std::vector<uint8_t> encode(uint8_t fw_ver) const;
};

// CMD_ID_DEV_ID (0x00) -> dev_id_t
struct DevId {
  bool valid{false};
  uint8_t revision{0};
  uint16_t hw_version{0};
  uint16_t sw_version{0};
  uint16_t dev_spec_data{0};
  uint32_t services{0};
  bool decode(const uint8_t *data, size_t len);
};

// Measurement notification (0x33) in connected mode
struct Measurement {
  bool valid{false};
  uint16_t vbat_mv{0};
  int16_t temp{0};   // x0.01
  uint16_t humi{0};  // x0.01
  uint16_t count{0};
  uint8_t flg{0};
  bool decode(const uint8_t *data, size_t len);
};

std::vector<uint8_t> encode_set_time(uint32_t local_epoch);         // 0x23
std::vector<uint8_t> encode_device_name(const std::string &name);   // 0x01 (empty -> reset)
std::vector<uint8_t> encode_pin_code(uint32_t pin);                 // 0x70

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
