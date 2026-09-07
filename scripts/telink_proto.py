"""
Pure-python reference implementation of the pvvx / Telink protocol pieces used by
TelinkMiFlasher.html.  Used by scripts/test_telink.py (host side, bleak) and as the
reference for the C++ port in components/xiaomi_esp_flasher.

Every constant below is traced to the pvvx sources in reference/ (see docs/telink_ota_protocol.md).
"""
import struct
import zlib

# --- GATT UUIDs -----------------------------------------------------------------
UUID_OTA_SERVICE = "00010203-0405-0607-0809-0a0b0c0d1912"   # TelinkMiFlasher.html doConnect()
UUID_OTA_CHAR    = "00010203-0405-0607-0809-0a0b0c0d2b12"
UUID_CUSTOM_SERVICE = "00001f10-0000-1000-8000-00805f9b34fb"  # custom pvvx fw, app_att.c COMMAND_UUID16_SERVICE
UUID_CUSTOM_CHAR    = "00001f1f-0000-1000-8000-00805f9b34fb"  # COMMAND_UUID16_CHARACTERISTIC
UUID_DIS            = "0000180a-0000-1000-8000-00805f9b34fb"
UUID_DIS_MODEL      = "00002a24-0000-1000-8000-00805f9b34fb"
UUID_DIS_SERIAL     = "00002a25-0000-1000-8000-00805f9b34fb"
UUID_DIS_FIRMWARE   = "00002a26-0000-1000-8000-00805f9b34fb"
UUID_DIS_HARDWARE   = "00002a27-0000-1000-8000-00805f9b34fb"
UUID_DIS_SOFTWARE   = "00002a28-0000-1000-8000-00805f9b34fb"
UUID_DIS_MANUF      = "00002a29-0000-1000-8000-00805f9b34fb"
UUID_MI_SERVICE     = "0000fe95-0000-1000-8000-00805f9b34fb"   # stock Xiaomi: auth (miAuthorization)
UUID_MI_AUTH_10     = "00000010-0000-1000-8000-00805f9b34fb"
UUID_MI_AUTH_19     = "00000019-0000-1000-8000-00805f9b34fb"
UUID_MI_MAIN        = "ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6"   # stock Xiaomi main service
UUID_MI_SPEED       = "ebe0ccd8-7a0a-4b0c-8a1a-6ff2997da3a6"
UUID_MI_TEMP        = "ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6"
UUID_ENV_SENSING    = "0000181a-0000-1000-8000-00805f9b34fb"
UUID_BTHOME         = "0000fcd2-0000-1000-8000-00805f9b34fb"

# --- custom fw command ids (src/cmd_parser.h) ---------------------------------------
CMD_ID_DEV_ID   = 0x00
CMD_ID_DNAME    = 0x01
CMD_ID_GDEVS    = 0x02
CMD_ID_SEN_ID   = 0x05
CMD_ID_DEV_MAC  = 0x10
CMD_ID_MI_KALL  = 0x15
CMD_ID_BKEY     = 0x18
CMD_ID_COMFORT  = 0x20
CMD_ID_EXTDATA  = 0x22
CMD_ID_UTC_TIME = 0x23
CMD_ID_TADJUST  = 0x24
CMD_ID_CFS      = 0x25
CMD_ID_MEASURE  = 0x33
CMD_ID_TRG      = 0x44
CMD_ID_CFG      = 0x55
CMD_ID_CFG_DEF  = 0x56
CMD_ID_PINCODE  = 0x70
CMD_ID_MTU      = 0x71
CMD_ID_REBOOT   = 0x72
CMD_ID_SET_OTA  = 0x73

# --- Telink OTA (SDK ble_ll_ota.h + TelinkMiFlasher.html updateBegin/sendOTAblock/sendLastOTA) ---
CMD_OTA_FW_VERSION = 0xff00   # written LE as 00 ff
CMD_OTA_START      = 0xff01   # 01 ff
CMD_OTA_END        = 0xff02   # 02 ff
OTA_BLOCK_DATA     = 16
OTA_ERRORS = ["Success", "Lost one or more packets", "CRC error in data", "Writing data to flash",
              "Lost last one or more packets", "Timeout", "Firmware CRC check"]

MAX_BLE_OTA_SIZE = 0x20000  # 128KB  (TelinkMiFlasher.html)
MAX_EXT_OTA_SIZE = 0x34000  # 208KB

# hw_version_str table (TelinkMiFlasher.html const hw_version_str) index == hw id (bin/README.md)
HW_VERSION_STR = [
    'LYWSD03MMC B1.4', 'MHO-C401(old)', 'CGG1-M(2020,2021)', 'LYWSD03MMC B1.9', 'LYWSD03MMC B1.6',
    'LYWSD03MMC B1.7|B2.0', 'CGDK2', 'CGG1-M(2022)', 'MHO-C401(2022)', 'MJWSD05MMC(ch)',
    'LYWSD03MMC B1.5', 'MHO-C122', 'MJWSD05MMC(en)', 'MJWSD06MMC', 'LYWSD03MMC B1.6(2025)/B1.1', 'ID15',
    'TB03F', 'TS0201', 'TNKS', 'THB2', 'BTH01', 'TH05', 'TH03Z', 'THB1', 'TH05D', 'TH05F', 'THB3',
    'ZTH01', 'ZTH02', 'PLM1', 'TH03(DIY)', 'LKTMZL02', 'KEY2', 'ZTH05', 'TH04', 'CB3S', 'HS09',
    'ZY-ZTH02', 'ZY-ZTH02/03-Pro', 'ZG-227Z', 'TS0202_PIR1', 'TS0202_PIR2', 'HDP16', 'TN_6ATAG3',
    'ZG-303Z', 'ZBeacon-TH01', 'ZBeaconMC', 'ZBeaconMC2', 'RSH_HS03', 'LYWSD02MMC', 'ZG204ZL',
    'ZG204ZV', 'TS0201_WING', 'DIY-SCD41']

ID_SENSOR_STR = ["None", "SHTV3(C3)", "SHT4x", "SHT30", "CHT8305", "ANT20/30", "CHT8215", "INA226",
                 "MY18B20", "MY18B20x2", "HX71X", "PWMRH", "NTC", "INA3221", "SCD41", "BME280"]


def crc16_modbus(buf: bytes) -> int:
    """TelinkMiFlasher.html crc16_modbus (poly 0xA001 reflected, init 0xFFFF)."""
    crc = 0xFFFF
    for b in buf:
        crc ^= b
        for _ in range(8):
            odd = crc & 1
            crc >>= 1
            if odd:
                crc ^= 0xA001
    return crc


def ota_block(index: int, data16: bytes) -> bytes:
    """One OTA data packet: [idx LE16][16 data bytes][crc16 LE] (sendOTAblock)."""
    assert len(data16) == 16
    body = struct.pack("<H", index) + data16
    return body + struct.pack("<H", crc16_modbus(body))


def ota_end_packet(block_count: int) -> bytes:
    """sendLastOTA(): 02 ff + (n-1) LE16 + ~(n-1) LE16."""
    last = block_count - 1
    return struct.pack("<HHH", CMD_OTA_END, last & 0xffff, (~last) & 0xffff)


def pad_firmware(data: bytes) -> bytes:
    if len(data) % 16:
        data = data + b"\xff" * (16 - len(data) % 16)
    return data


def test_ota_firmware(data: bytes, fwmaxsize=MAX_BLE_OTA_SIZE, big_ota=False):
    """Port of testOTAFirmware(): returns ('ok'|'sign'|error string, info dict)."""
    info = {}
    fsize = len(data)
    if fsize < 1024:
        return "Wrong binary Telink OTA firmware size!", info
    if struct.unpack_from("<I", data, 0)[0] == 0x0beef11e:
        return "Zigbee OTA images are not handled by this tool", info
    if big_ota:
        if fsize > MAX_EXT_OTA_SIZE:
            return "Size firmware is more 208 kbytes!", info
    elif fsize > fwmaxsize:
        return f"Size firmware is more {fwmaxsize} bytes!", info
    if struct.unpack_from("<I", data, 8)[0] != 0x544c4e4b:  # 'KNLT'
        return "Incorrect head in Telink OTA binary firmware", info
    hsize = struct.unpack_from("<I", data, 0x18)[0]
    info["size_in_head"] = hsize
    if hsize > fsize or (hsize & 0x0f) != 4:
        return "Invalid size pointer in Telink OTA binary firmware!", info
    # TelinkMiFlasher.html crc32(): table-driven, init -1, NO final xor  => ~zlib.crc32
    calc = (~zlib.crc32(data[:hsize - 4])) & 0xffffffff
    stored = struct.unpack_from("<I", data, hsize - 4)[0]
    info["crc_calc"] = calc
    info["crc_stored"] = stored
    if calc != stored:
        return "Incorrect CRC in Telink OTA binary firmware!", info
    if hsize < fsize and fsize - hsize > 1024:
        if data[hsize:hsize + 12] == bytes.fromhex("000000004d49ef54464f5441"):
            return "sign", info
    return "ok", info


def parse_cfg(value: bytes):
    """CustomBlkParse() for blkid 0x55: [0x55][ver][flg][flg2][toff][hoff][adv_int][meas_int][rf][lat][lcd_tint][hver][av_mem]"""
    if len(value) < 10 or value[0] != CMD_ID_CFG:
        return None
    ln = len(value) - 1
    cfg = dict(
        ver=value[1], flg=value[2], flg2=value[3],
        temp_offset=struct.unpack("b", value[4:5])[0],
        humi_offset=struct.unpack("b", value[5:6])[0],
        advertising_interval=value[6], measure_interval=value[7],
        rf_tx_power=value[8], connect_latency=value[9],
        lcd_tint=value[10] if ln >= 10 else 55,
        hver=value[11] if ln >= 11 else 0x80,
        av_meas_mem=value[12] if ln >= 12 else 0,
    )
    if cfg["ver"] < 0x36:
        cfg["hver"] &= 0x87
    if cfg["ver"] < 0x48:
        hwid = cfg["hver"] & 0x0f
        if ln >= 11 and hwid == 0x0f:
            hwid = 16 + (cfg["lcd_tint"] & 0x7f)
    else:
        hwid = cfg["hver"]
    cfg["hw_id"] = hwid
    cfg["hw_name"] = HW_VERSION_STR[hwid] if hwid < len(HW_VERSION_STR) else f"Unknown/DIY({hwid})"
    cfg["sw_version"] = f"{cfg['ver'] >> 4}.{cfg['ver'] & 0x0f}"
    cfg["big_ota"] = cfg["ver"] > 0x45 or hwid in (9, 11, 12, 13)
    return cfg


def parse_dev_id(value: bytes):
    """CMD_ID_DEV_ID response: dev_id_t (cmd_parser.h)"""
    if len(value) < 12 or value[0] != CMD_ID_DEV_ID:
        return None
    rev, hw, sw, spec, services = struct.unpack_from("<BHHHI", value, 1)
    return dict(revision=rev, hw_version=hw, sw_version=sw, dev_spec_data=spec, services=services,
                sensor1=ID_SENSOR_STR[spec & 0xff] if (spec & 0xff) < len(ID_SENSOR_STR) else spec & 0xff,
                sensor2=ID_SENSOR_STR[spec >> 8] if (spec >> 8) < len(ID_SENSOR_STR) else spec >> 8,
                hw_name=HW_VERSION_STR[hw] if hw < len(HW_VERSION_STR) else f"?({hw})")


def parse_comfort(value: bytes):
    if len(value) < 9 or value[0] != CMD_ID_COMFORT:
        return None
    tlo, thi, hlo, hhi = struct.unpack_from("<hhHH", value, 1)
    return dict(temp_lo=tlo / 100, temp_hi=thi / 100, humi_lo=hlo / 100, humi_hi=hhi / 100)


def parse_measure(value: bytes):
    if len(value) < 9 or value[0] != CMD_ID_MEASURE:
        return None
    vbat, temp, humi, count = struct.unpack_from("<HhHH", value, 1)
    return dict(vbat_mv=vbat, temp=temp / 100, humi=humi / 100, count=count,
                flg=value[9] if len(value) > 9 else None)


def parse_trg(value: bytes, cfg_ver: int):
    if len(value) < 8 or value[0] != CMD_ID_TRG:
        return None
    ln = len(value) - 1
    tthr, hthr = struct.unpack_from("<hh", value, 1)
    d = dict(temp_threshold=tthr / 100, humi_threshold=hthr / 100)
    if ln >= 9:
        thst, hhst = struct.unpack_from("<hh", value, 5)
        d.update(temp_hysteresis=thst / 100, humi_hysteresis=hhst / 100)
        if ln >= 12 and cfg_ver >= 0x37:
            d["rds_rpint"], d["rds_type"], d["flg"] = struct.unpack_from("<HBB", value, 9)
        else:
            d["flg"] = value[9]
    return d


def parse_time(value: bytes):
    if len(value) < 5 or value[0] != CMD_ID_UTC_TIME:
        return None
    d = dict(utc=struct.unpack_from("<I", value, 1)[0])
    if len(value) >= 9:
        d["last_set"] = struct.unpack_from("<I", value, 5)[0]
    return d


def encode_cfg(cfg: dict) -> bytes:
    """SendCustomConfig(): 55 + flg flg2 toff hoff adv meas rf lat lcd_tint hver av_meas_mem (ver>=0x20)."""
    b = bytes([CMD_ID_CFG, cfg["flg"] & 0xff, cfg["flg2"] & 0xff, cfg["temp_offset"] & 0xff,
               cfg["humi_offset"] & 0xff, cfg["advertising_interval"] & 0xff, cfg["measure_interval"] & 0xff,
               cfg["rf_tx_power"] & 0xff, cfg["connect_latency"] & 0xff, cfg["lcd_tint"] & 0xff])
    if cfg["ver"] >= 0x20:
        b += bytes([cfg["hver"] & 0xff, cfg["av_meas_mem"] & 0xff])
    return b


def encode_comfort(tlo, thi, hlo, hhi) -> bytes:
    return bytes([CMD_ID_COMFORT]) + struct.pack("<hhHH", round(tlo * 100), round(thi * 100),
                                                  round(hlo * 100), round(hhi * 100))


def encode_time(utc_local: int) -> bytes:
    """setDevTime(): 0x23 + LE32 (time is local time: Date.now() - timezoneOffset)."""
    return bytes([CMD_ID_UTC_TIME]) + struct.pack("<I", utc_local & 0xffffffff)
