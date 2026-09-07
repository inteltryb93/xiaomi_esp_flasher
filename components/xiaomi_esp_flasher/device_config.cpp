#include "device_config.h"
#include <cstring>

namespace esphome {
namespace xiaomi_esp_flasher {

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t) p[0] | ((uint16_t) p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t) rd16(p) | ((uint32_t) rd16(p + 2) << 16); }
static inline void wr16(std::vector<uint8_t> &v, uint16_t x) { v.push_back(x & 0xff); v.push_back(x >> 8); }
static inline void wr32(std::vector<uint8_t> &v, uint32_t x) { wr16(v, x & 0xffff); wr16(v, x >> 16); }

const char *advertising_type_name(uint8_t v, uint8_t fw_ver) {
  // TelinkMiFlasher.html CustomConfig(): option labels for LCD devices
  switch (v & 3) {
    case 0: return "ATC1441";
    case 1: return "PVVX (Custom)";
    case 2: return "MIJIA (MiHome)";
    default: return fw_ver >= 0x45 ? "BTHome v2" : (fw_ver >= 0x37 ? "BTHome v1" : "All");
  }
}

bool DeviceConfig::decode(const uint8_t *d, size_t len) {
  // CustomBlkParse: blkid == 0x55 && len(after id) >= 9
  if (len < 10 || d[0] != CMD_ID_CFG)
    return false;
  size_t ln = len - 1;
  ver = d[1];
  flg = d[2];
  flg2 = d[3];
  temp_offset = (int8_t) d[4];
  humi_offset = (int8_t) d[5];
  advertising_interval = d[6];
  measure_interval = d[7];
  rf_tx_power = d[8];
  connect_latency = d[9];
  lcd_tint = ln >= 10 ? d[10] : 55;
  hver = ln >= 11 ? d[11] : 0x80;
  if (ver < 0x36)
    hver &= 0x87;
  av_meas_mem = ln >= 12 ? d[12] : 0;
  raw_len = ln;
  valid = true;
  return true;
}

void DeviceConfig::clamp() {
  if (advertising_interval < 1) advertising_interval = 1;
  if (measure_interval < 1) measure_interval = 1;
  if (rf_tx_power & 0x80) {  // VANT
    if (rf_tx_power > 191) rf_tx_power = 191;
    if (rf_tx_power < 130) rf_tx_power = 130;
  } else {  // VBAT
    if (rf_tx_power < 23) rf_tx_power = 23;
    if (rf_tx_power > 63) rf_tx_power = 63;
  }
  if (ver > 0x40 && connect_latency > 49) connect_latency = 49;
  if (lcd_tint < 10) lcd_tint = 10;
  if (ver < 0x47) {
    if (temp_offset < -127) temp_offset = -127;
    if (humi_offset < -127) humi_offset = -127;
  } else if (ver > 0x50) {
    if ((uint8_t) humi_offset < 6) humi_offset = 5;
  }
}

std::vector<uint8_t> DeviceConfig::encode() const {
  // SendCustomConfig(): '55'+flg+flg2+toff+hoff+adv+meas+rf+lat+lcd_tint[+hver+av_meas_mem if ver >= 0x20]
  std::vector<uint8_t> v = {CMD_ID_CFG, flg, flg2, (uint8_t) temp_offset, (uint8_t) humi_offset,
                            advertising_interval, measure_interval, rf_tx_power, connect_latency, lcd_tint};
  if (ver >= 0x20) {
    v.push_back(hver);
    v.push_back(av_meas_mem);
  }
  return v;
}

bool ComfortConfig::decode(const uint8_t *d, size_t len) {
  if (len < 9 || d[0] != CMD_ID_COMFORT)
    return false;
  temp_lo = (int16_t) rd16(d + 1);
  temp_hi = (int16_t) rd16(d + 3);
  humi_lo = rd16(d + 5);
  humi_hi = rd16(d + 7);
  valid = true;
  return true;
}
std::vector<uint8_t> ComfortConfig::encode() const {
  std::vector<uint8_t> v = {CMD_ID_COMFORT};
  wr16(v, (uint16_t) temp_lo); wr16(v, (uint16_t) temp_hi); wr16(v, humi_lo); wr16(v, humi_hi);
  return v;
}

bool SensorConfig::decode(const uint8_t *d, size_t len) {
  // CustomBlkParse blkid 0x25/0x26 && len(after id) > 16
  if (len < 18 || (d[0] != CMD_ID_CFS && d[0] != CMD_ID_CFS_DEF))
    return false;
  temp_k = rd32(d + 1);
  humi_k = rd32(d + 5);
  temp_z = (int16_t) rd16(d + 9);
  humi_z = (int16_t) rd16(d + 11);
  if (len > 18) {  // ver 4.9+
    id = rd32(d + 13);
    i2c_addr = d[17];
    sensor_type = d[18];
  } else {
    id = ((uint32_t) d[13] << 24) | ((uint32_t) d[14] << 16) | ((uint32_t) d[15] << 8) | d[16];
    i2c_addr = d[17];
    sensor_type = 0xff;
  }
  valid = true;
  return true;
}
std::vector<uint8_t> SensorConfig::encode() const {
  // setSensCfg(): 25 + temp_k u32 + humi_k u32 + temp_z i16 + humi_z i16
  std::vector<uint8_t> v = {CMD_ID_CFS};
  wr32(v, temp_k); wr32(v, humi_k); wr16(v, (uint16_t) temp_z); wr16(v, (uint16_t) humi_z);
  return v;
}

bool TriggerConfig::decode(const uint8_t *d, size_t len, uint8_t fw_ver) {
  if (len < 8 || d[0] != CMD_ID_TRG)
    return false;
  size_t ln = len - 1;
  temp_threshold = (int16_t) rd16(d + 1);
  humi_threshold = (int16_t) rd16(d + 3);
  rds_type = 0; rds_rpint = 0;
  if (ln >= 9) {
    temp_hysteresis = (int16_t) rd16(d + 5);
    humi_hysteresis = (int16_t) rd16(d + 7);
    if (ln >= 12 && fw_ver >= 0x37) {
      rds_rpint = rd16(d + 9);
      rds_type = d[11];
      flg = d[12];
    } else {
      flg = d[9];
    }
  } else {
    temp_hysteresis = (int8_t) d[5] * 10;
    humi_hysteresis = (int8_t) d[6] * 10;
    flg = d[7];
  }
  valid = true;
  return true;
}
std::vector<uint8_t> TriggerConfig::encode(uint8_t fw_ver) const {
  std::vector<uint8_t> v = {CMD_ID_TRG};
  wr16(v, (uint16_t) temp_threshold); wr16(v, (uint16_t) humi_threshold);
  if (fw_ver >= 0x26) {
    wr16(v, (uint16_t) temp_hysteresis); wr16(v, (uint16_t) humi_hysteresis);
    if (fw_ver >= 0x37) { wr16(v, rds_rpint); v.push_back(rds_type); }
  } else {
    v.push_back((uint8_t) (temp_hysteresis / 10)); v.push_back((uint8_t) (humi_hysteresis / 10));
  }
  return v;
}

bool DevId::decode(const uint8_t *d, size_t len) {
  if (len < 12 || d[0] != CMD_ID_DEV_ID)
    return false;
  revision = d[1];
  hw_version = rd16(d + 2);
  sw_version = rd16(d + 4);
  dev_spec_data = rd16(d + 6);
  services = rd32(d + 8);
  valid = true;
  return true;
}

bool Measurement::decode(const uint8_t *d, size_t len) {
  // CustomBlkParse blkid 0x33 && len(after id) >= 8 ; echo "33 ff" is shorter and ignored
  if (len < 9 || d[0] != CMD_ID_MEASURE)
    return false;
  vbat_mv = rd16(d + 1);
  temp = (int16_t) rd16(d + 3);
  humi = rd16(d + 5);
  count = rd16(d + 7);
  flg = len > 9 ? d[9] : 0;
  valid = true;
  return true;
}

std::vector<uint8_t> encode_set_time(uint32_t t) {
  std::vector<uint8_t> v = {CMD_ID_UTC_TIME};
  wr32(v, t);
  return v;
}
std::vector<uint8_t> encode_device_name(const std::string &name) {
  std::vector<uint8_t> v = {CMD_ID_DNAME};
  if (name.empty()) {
    v.push_back(0);  // CleanDevName(): "01 00"
  } else {
    for (size_t i = 0; i < name.size() && i < 18; i++)
      v.push_back((uint8_t) name[i]);
  }
  return v;
}
std::vector<uint8_t> encode_pin_code(uint32_t pin) {
  std::vector<uint8_t> v = {CMD_ID_PINCODE};
  wr32(v, pin);
  return v;
}

}  // namespace xiaomi_esp_flasher
}  // namespace esphome
