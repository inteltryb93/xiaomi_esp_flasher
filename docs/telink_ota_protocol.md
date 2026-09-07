# Telink / pvvx BLE protocol as used by TelinkMiFlasher.html

This document is a step-by-step reconstruction of what `pvvx.github.io/ATC_MiThermometer/TelinkMiFlasher.html`
(v14.1, cloned 2026-09-07 into `reference/pvvx.github.io`) and `TelinkOTA.html` do on the wire.
Every statement carries a reference to the JavaScript function, the firmware source
(`reference/ATC_MiThermometer/src`) or the Telink SDK header (`reference/ATC_MiThermometer/SDK`).
The C++ port lives in `components/xiaomi_esp_flasher/` and the Python reference in `scripts/telink_proto.py`.

## 1. GATT services and characteristics

| Service | Characteristic | Purpose | Source |
|---|---|---|---|
| `00010203-0405-0607-0809-0a0b0c0d1912` | `00010203-0405-0607-0809-0a0b0c0d2b12` (read, write-without-response) | Telink OTA data channel (`otaEnabled`) | `doConnect()`; `app_att.c` `my_OtaServiceUUID/my_OtaUUID`, `TELINK_OTA_UUID_SERVICE` |
| `0x1F10` | `0x1F1F` (read, write-w/o-resp, notify) | pvvx custom firmware command channel (`customEnabled`, `settingsCharacteristics`) | `customAction()`; `app_att.c` `COMMAND_UUID16_SERVICE/CHARACTERISTIC` |
| `0x180A` Device Information | `0x2A24` model, `0x2A25` serial, `0x2A26` firmware rev, `0x2A27` hardware rev, `0x2A28` software rev, `0x2A29` manufacturer | version strings (`devInfEnabled`) | `getDevVersion()`; `app_att.c` `USE_DEVICE_INFO_CHR_UUID` block |
| `0xFE95` Xiaomi | `0x0010` (notify+write: auth control), `0x0019` (notify+write: auth data) | Mi registration / login ("Do Activation") on **stock** firmware (`miEnabled` / `miAuthorization()`) | `miAuthorization()`, `startRegister()`, `sendLogin()` |
| `ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6` | `ebe0ccd8-…` (write: "speed"), `ebe0ccc1-…` (notify: temp/hum/batt), `ebe0ccb7-…` (clock) | stock Xiaomi main service | `miAction()` |
| `0x181A` Environmental Sensing | `0x2A1F`, `0x2A6E`, `0x2A6F` | temperature/humidity on custom fw (`devIdEnabled`) | `doConnect()` service list, `app_att.c` |
| `0x180F` Battery | `0x2A19` | battery level | `app_att.c` |
| `0xFCD2` BTHome | – | only advertised (service data) on custom fw when adv type = BTHome | `catchAdvertisement()`, `bthome_beacon.c` |

`doConnect()` enumerates all primary services and sets the flags `otaEnabled`, `customEnabled`, `miEnabled`,
`devIdEnabled`, `devTest` (0xFFE0), `bthomeEnabled`, `devInfEnabled`, `EnabledQinping` from that list.
The branch taken afterwards:

* `otaEnabled == false` → no Telink OTA service → flashing impossible from this tool (THB2 devices etc.).
* `miEnabled` → stock Xiaomi firmware: read DIS, then `miAction()` → `miAuthorization()` (activation needed before OTA).
* `customEnabled` → pvvx custom firmware: `customAction()` subscribes to `0x1F1F`, reads it, then sends `33 C8`
  ("query 200 measurements", `CMD_ID_MEASURE`). The config is fetched with the **Get Config** button = `55`.
* neither → unknown Telink firmware: treated like Mi (`miAuthorization()`), activation may be skipped.

### Observed on the test device A4:C1:38:4A:E8:8C (host run `logs/host_*.log`)

Services: 0x181A, 0x1F10, 0x1800, 0x180A, OTA `…1912`, 0x1801, 0x180F. DIS: model `LYWSD03MMC`, serial
`F1.0-CFMK-LB-FLD----`, firmware `github.com/pvvx`, hardware `B1.7`, software `V4.7`, manufacturer `miaomiaoce.com`.
→ custom firmware path, `otaEnabled=true`, `miEnabled=false`.

## 2. Version / hardware detection

### 2.1 Stock Xiaomi firmware
* Hardware revision string `0x2A27` (e.g. `B1.4`, `B1.5`, `B1.6`, `B1.7`, `B1.9`, `B2.0`, `B1.1`) and firmware
  revision `0x2A26` (e.g. `1.0.0_0130`, `2.1.1_0159`) are read via `getDevVersion()`.
* `miAuthorization()` maps `bluetoothDevice.name == 'LYWSD03MMC'` + the 4 first chars of the HW string to
  `cfg.hver`: `B1.4|0000→0x80`, `B1.5→10`, `B1.6→3`, `B1.9→4`, `B1.7|B2.0→5`, `B1.1→14`, else `0`.
  `hwver_id = cfg.hver & 0x0f`. (Note: `bin/README.md` documents 3 = B1.9 and 4 = B1.6, i.e. the JS
  literals for B1.6/B1.9 are swapped relative to the firmware table; both indexes point to the same
  `ATC_vXX.bin`, so pvvx never noticed. Our compatibility module uses the README/firmware table and treats
  the stock DIS string as the source of truth.)
* Firmware `2.1.1_0159` (new B1.6 batches) cannot be activated locally: it requires the Mi-Home token
  (`fsignota = "sign_2.1.1_0159c.bin"`, issue #602). The tool must refuse to flash such a device unless
  the user supplies token+bindkey for `keyMiLogin()`.

### 2.2 pvvx custom firmware
* `CMD_ID_CFG` (`55`) response (`CustomBlkParse` blkid 0x55, `cmd_parser.c`, `app.h cfg_t`):

  | byte | field | notes |
  |---|---|---|
  | 0 | `0x55` | |
  | 1 | `ver` | firmware version, BCD: `0x47` = "4.7" (`app_config.h VERSION`) |
  | 2 | `flg` | bit0..1 advertising_type (0 atc1441, 1 pvvx custom, 2 Mi, 3 BTHome), bit2 comfort_smiley, bit3 show_time_smile (clock), bit4 temp_F_or_C, bit5 show_batt_enabled, bit6 tx_measures, bit7 lp_measures |
  | 3 | `flg2` | bit0..2 smiley (0..7), bit3 adv_crypto, bit4 adv_flags, bit5 bt5phy, bit6 longrange, bit7 screen_off (ver ≥ 4.3) |
  | 4 | `temp_offset` | int8; ver < 0x47: ×0.1 °C (−12.7..12.7); ver ≥ 0x47: `flg3` (bit0..3 adv_interval_delay, bit6 date_ddmm, bit7 day-of-week flag) |
  | 5 | `humi_offset` | int8; ver < 0x47: ×0.1 %; ver ≥ 0x47: `event_adv_cnt` (5..255) |
  | 6 | `advertising_interval` | ×62.5 ms (1..160) |
  | 7 | `measure_interval` | × advertising interval (1..25) |
  | 8 | `rf_tx_power` | 130..191 (VANT) or 23..63 (VBAT) |
  | 9 | `connect_latency` | (+1) × 20 ms |
  | 10 | `lcd_tint` (`min_step_time_update_lcd`) | ×0.05 s (10..255) |
  | 11 | `hw_ver` | read-only; `hwver_id = hw_ver & 0x0f` for ver < 0x48 (0x0f → ext id 16 + `lcd_tint&0x7f`), `hw_ver` as-is for ver ≥ 0x48 |
  | 12 | `averaging_measurements` | ×measure interval, 0 = off |

  Writing: `55` + the same bytes (without `ver`) → `SendCustomConfig()`; firmware `memcpy`s into `cfg`,
  runs `test_config()`, saves to flash (`flash_write_cfg`) and answers with `ble_send_cfg()` (`cmd_parser.c:348`).
  `56` = set defaults (`CMD_ID_CFG_DEF`) → device restores `def_cfg` and answers with the same frame.

* Since firmware 4.7 the temperature/humidity offsets moved to **sensor settings** `CMD_ID_CFS` (`25`):
  response `[25][temp_k u32][humi_k u32][temp_z i16][humi_z i16][id u32][i2c_addr u8][sensor_type u8]`
  (`CustomBlkParse` blkid 0x25). `T = RegT*Tk/65536 + Tz`, values ×0.01. Writing `25 + 12 bytes` (`setSensCfg()`);
  `26` = default sensor config.
* `CMD_ID_DEV_ID` (`00`) → `dev_id_t` (`cmd_parser.h`): `[00][revision][hw_version u16][sw_version u16 BCD][dev_spec_data u16][services u32]`.
  On LYWSD03MMC `hw_version = cfg.hw_ver` (`cmd_parser.c:292`).
* `CMD_ID_GDEVS` (`02`) → `[02][sensor i2c addr<<1][lcd i2c addr<<1][rtc]`; used to cross-check the HW class
  (`app.c set_hw_version()` table: LCD 0x3C+SHTC3 = B1.4, UART+SHTC3 = B1.5, UART+SHT4x = B1.6,
  0x3C+SHT4x = B1.7/B2.0, 0x3E = B1.9, SPI = B1.1). Test device: `02 88 78` → sensor 0x44 (SHT4x), LCD 0x3C → B1.7/B2.0.
* Other read commands used by the GUI: `20` comfort (`[20][tlo i16][thi i16][hlo u16][hhi u16]` ×0.01),
  `44` trigger (`[44][tthr i16][hthr i16][thst i16][hhst i16][rds_rpint u16][rds_type u8][flg u8]`),
  `01` device name (`[01][utf8…]`), `23` time (`[23][utc u32][last_set u32]`), `10` MAC
  (`[10][len][mac[6] reversed][rand[2]]`), `05` sensor id, `18` bind key, `24` clock step, `22` ext data.

## 3. "Do Activation" (stock Xiaomi firmware only)

`sendRegister()` → `miAuthorization()` state machine on `0xFE95`. All values are hex strings written as raw bytes.

1. Subscribe notifications on `0x0010` and `0x0019`.
2. Write `a2 00 00 00` to `0x0010` (`sendRegister`, `mode_activation = 1`, `state = 0`), generate an ECDH P‑256
   key pair (`doGenerate()`, WebCrypto; own public key exported raw = `04 || X || Y`, 65 bytes).
3. Device answers on `0x0019`:
   * `00 00 02 00 01 00 00 00 …` → device sends its "new id" (`device_new_id = payload[8:]`), host writes `00 00 01 00`
     then after 250 ms `startRegister()`: write `15 00 00 00` to `0x0010`, `state = 1`, write `00 00 00 03 04 00` to `0x0019`.
   * `00 00 00 00 01 00` → not activated, host writes `00 00 01 01`; `00 00 00 00 02 00` → already activated, same.
   * `01 00 …`/`02 00 …` while activated: known id fragments, then `00 00 01 00` and `startRegister()`.
   * `00 00 01 01` in `state 1` → `state = 2`, host sends its public key **without the leading 04** in 4 frames:
     `01 00`+bytes[0..17], `02 00`+[18..35], `03 00`+[36..53], `04 00`+[54..63] (each frame = 2-byte header + 18 bytes).
   * `00 00 00 03 04 00` → host writes `00 00 01 01`.
   * In `state 2` the device's public key arrives as `01 00 …`, `02 00 …`, `03 00 …`, `04 00 …` (64 bytes, prefix `04` added).
     After the 4th frame host writes `00 00 01 00` and computes `makeSharedKey()`:
     `shared = ECDH(own_priv, dev_pub)` (32 bytes X coordinate), `derived = HKDF-SHA256(shared, salt=∅, info="mible-setup-info", 64 bytes)`.
     `token = derived[0:12]`, `bind_key = derived[12:28]`, `A = derived[28:44]`.
     `mi_write_did = AES-CCM(key=A, nonce=101112131415161718191A1B, aad="devID" (6465764944), tag 4 bytes).encrypt(device_new_id)`
     (`sjcl.mode.ccm.encrypt(..., 32)` → 32-bit tag). Then host writes `00 00 00 00 02 00`.
   * `00 00 01 01` in `state 2` → `state = 3`, host writes `mi_write_did` in 2 frames `01 00`+[0..17], `02 00`+[18..].
   * `00 00 01 00` in `state 3` → `state = 0`, host writes `13 00 00 00` to `0x0010` (REG_VERIFY_SUCC).
4. `0x0010` notifications: `11000000` REG_SUCCESS → `sendLogin()`; `12000000` REG_FAILED; `21000000` LOG_SUCCESS →
   `is_logged_in = true` (OTA allowed); `e2000000` ERR_REPEAT_LOGIN also counts as logged in; `e0000000`
   not registered, `23000000` login failed, `22000000` invalid LTMK.
5. `sendLogin()`: `mi_random_key` = 16 random bytes, write `24 00 00 00` to `0x0010`, `00 00 00 0b 01 00` to `0x0019`,
   then the login state machine (`mode_activation == 0`):
   `00 00 01 01` → `01 00`+random; `00 00 00 0d 01 00` → `00 00 01 01`; `01 00 …` device random → `do_login_generate()`,
   write `00 00 01 00`; `00 00 00 0c 02 00` → `00 00 01 01`; `01 00`/`02 00` device info (32 bytes) compared to
   `expected_device_infos`; then `00 00 01 00`, `00 00 00 0a 02 00`; `00 00 01 01` → send `mi_device_info_send`
   in 2 frames. Keys: `derived = HKDF-SHA256(token, salt = rand_host||rand_dev, info="mible-login-info", 64 bytes)`,
   `expected = HMAC-SHA256(derived[0:16], rand_dev||rand_host)`, `send = HMAC-SHA256(derived[16:32], rand_host||rand_dev)`.
   Newer devices use the short variant (`0000020d`/`0000020c` frames, states 12–14).
6. After `LOG_SUCCESS` the OTA can start. Before OTA on Mi firmware the host writes `1e 00 00` to `ebe0ccd8` ("speed").

The C++ port implements this with mbedtls (ECDH P‑256 via `mbedtls_ecdh`, HKDF via `mbedtls_hkdf`,
AES‑CCM via `mbedtls_ccm_encrypt_and_tag`, HMAC via `mbedtls_md_hmac`). It is only used when the device
exposes `0xFE95`; the test device runs custom firmware, so activation could not be exercised on real hardware
(see `docs/test_report.md`).

## 4. Telink OTA

Sources: `updateBegin()`, `sendOTAblock()`, `sendLastOTA()`, `getHexCRC()`, `crc16_modbus()` in TelinkMiFlasher.html /
TelinkOTA.html; `SDK/components/stack/ble/service/ble_ll_ota.h`; `ble.c app_enter_ota_mode()`.

### 4.1 Preconditions
* Image validated by `testOTAFirmware()` (see §5).
* Size ≤ 128 KiB (`MAX_BLE_OTA_SIZE`), or ≤ 208 KiB when `bigOtaEnabled` (custom fw `ver > 0x45`, or hw 9/11/12/13)
  — bigger images first need `73 00 00 04 00 <size_k LE32>` on `0x1F1F` (clear ext OTA region) and wait for `73 03 …`.
* On stock firmware: logged in (`is_logged_in`). On custom firmware: nothing else.
* `fwmaxsize` per model: LYWSD02MMC 128K, MJWSD05MMC 208K.

### 4.2 Sequence (all writes to the OTA characteristic are write-without-response)
1. (Mi fw only) write `1e 00 00` to `ebe0ccd8`.
2. wait 500 ms.
3. write `00 ff` (`CMD_OTA_FW_VERSION`, LE16).
4. write `01 ff` (`CMD_OTA_START`) → firmware `app_enter_ota_mode()`: latency 0, OTA timeout 16 s (`bls_ota_setTimeout`).
5. wait 300 ms.
6. for every 16-byte block `i` (image padded with `0xFF` to a multiple of 16):
   `[i LE16][16 data bytes][crc16 LE16]` where crc16 = Modbus CRC‑16 (init 0xFFFF, poly 0xA001 reflected)
   over the 18 bytes `[i LE16][data]`.
   After every 8th block (`(i+1) % 8 == 0`) **read** the OTA characteristic; first byte is the SDK result
   (`OTA_SUCCESS=0, PACKET_LOSS=1, DATA_CRC_ERR=2, WRITE_FLASH_ERR=3, DATA_UNCOMPLETE=4, TIMEOUT=5, FW_CHECK_ERR=6`).
   Non-zero aborts (`showOtaError`). The read also acts as flow control (write-without-response would otherwise
   overflow the link).
7. end: `02 ff` + `(n-1) LE16` + `(~(n-1)) & 0xFFFF LE16` (`sendLastOTA`). The device verifies the image CRC,
   reboots into the new firmware and drops the link. Success is reported as "Update done".
8. **Never disconnect from the central side after `02 ff`.** In Bluedroid a write-without-response is reported
   complete (`ESP_GATTC_WRITE_CHAR_EVT`) when it is *queued*, not when the peripheral has received it; closing the
   link right after the end packet discards the queued tail (observed 2026-09-07 on a weak link, RSSI −89:
   all 5395 blocks "sent", device kept firmware 4.7 = the SDK rejected the image as incomplete). The pvvx browser
   flasher never disconnects – the thermometer drops the link itself when it reboots. The ESP32 port therefore
   issues one **read** of the OTA characteristic after `02 ff`: ATT requests are ordered behind the queued
   commands, so the response proves the peripheral consumed everything and carries the final SDK result
   (`0` success → reboot follows; `4` "Lost last one or more packets", `6` "Firmware CRC check" → abort). Only if
   the device still holds the link 25 s later does the ESP32 disconnect.
9. Verification after reboot is done by reconnecting and re-reading the DIS/`55` config (new `ver`).

Timeouts: the firmware aborts OTA if no packet arrives for 16 s; the browser sends as fast as the link allows.
Retry semantics: a data block can be re-sent only if the previous write failed at the transport layer *before*
the device consumed it; a status read returning `PACKET_LOSS` or `CRC` cannot be repaired — the OTA must be
aborted and restarted from `01 ff` (the SDK clears the new-firmware area on start, `bls_ota_clearNewFwDataArea`).
The ESP32 port therefore retries only GATT write failures for the *same* index, and never resends after a status error.

## 5. Firmware image validation (`testOTAFirmware()`)
* size ≥ 1024;
* `u32 @0x00 == 0x0beef11e` → Zigbee OTA container (not used here);
* `u32 @0x08 == 0x544c4e4b` (`'KNLT'`), `hsize = u32 @0x18`, `hsize ≤ size` and `hsize & 0x0f == 4`;
* CRC32 over `[0, hsize-4)` equals `u32 @hsize-4`. **The CRC32 variant is the table CRC with init `0xFFFFFFFF` and
  no final inversion** (`crc32()` in TelinkMiFlasher.html) → `~zlib.crc32()`; verified on `ATC_v59.bin`
  (0x6F4C2B58) and `Original_OTA_Xiaomi_LYWSD03MMC_v1.0.0_0130.bin`.
* if extra data after `hsize` starts with `00000000 4d49ef54 464f5441` → signed original image ("sign").

## 6. firmware.json
`{"version": 89, "betaver": 96, "custom": [...], "original": [...], "signed": [...]}` — arrays indexed by
`hwver_id` (table in `bin/README.md`); `"?"` = no image. `version` is the BCD firmware byte in decimal
(89 = 0x59 = "5.9"). File names embed the version (`ATC_v59.bin`). `menuUpgrade()` shows one button per array.

## 7. Custom-firmware commands used for configuration (write to `0x1F1F`)
| cmd | payload | GUI function |
|---|---|---|
| `55 …` | cfg bytes (§2.2) | `SendCustomConfig()` |
| `56` | – | Set default |
| `20 tlo thi hlo hhi` (i16,i16,u16,u16 ×0.01) | comfort | `SendCmf()` |
| `25 …` | sensor Tk/Hk/Tz/Hz | `setSensCfg()` |
| `44 …` | trigger/reed | `sendTrg()` |
| `23 t LE32` | set clock; t = `Date.now()/1000 - tzoffset` i.e. **local time** | `setDevTime()` |
| `01 name` (1..18 chars) / `01 00` | device name / reset | `sendDevName()`, `CleanDevName()` |
| `70 pin LE32` | PIN 0..999999, 0 disables | `sendPinCode()` |
| `71 mtu` | request MTU exchange | `setNewTBKey()` |
| `72` | reboot on disconnect | `CMD_ID_REBOOT` |
| `33 ff` / `33 00` | start/stop measurement notifications | Start/Stop Tx Measure |
