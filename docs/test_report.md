# Test report – real hardware run, 2026-09-07

Gateway: ESP32-C3 (QFN32 rev v0.4, 4 MB flash, USB-Serial/JTAG), ESPHome 2026.10.0-dev (local checkout), ESP-IDF 5.5.5,
IP 192.168.0.102 (`xiaomi-esp-flasher.local`), build `yaml/xiaomi_esp_flasher.yaml` (flash 1 669 398 B / 1 703 936 B).
Test device: **A4:C1:38:4A:E8:8C** (alias "Living Room"). Logs: `logs/run_full.log`, `logs/run_ota.log`,
`logs/host_*.log`; API transcript below reproduced with `scripts/api_test.sh`.

## Result summary for A4:C1:38:4A:E8:8C

```
Detected model:      LYWSD03MMC (DIS 0x2A24 "LYWSD03MMC", manufacturer "miaomiaoce.com", serial F1.0-CFMK-LB-FLD----)
Detected HW:         DIS 0x2A27 "B1.7"; pvvx cfg hw id 4 ("B1.6" table entry); dev_id hw_version 4;
                     I2C topology sensor 0x44 (SHT4x) + LCD 0x3C  => B1.7/B2.0 class (id 5)
                     -> flagged as warnings, all ids map to the same ATC image, compatibility PASSED
Original firmware:   pvvx custom 4.7 (cfg ver 0x47, DIS software "V4.7", DIS firmware "github.com/pvvx")
Target firmware:     ATC_v59.bin (manifest version 89 = 5.9), 86 308 bytes, CRC32 0x6F4C2B58 (bundled, offline)
Activation:          not required (custom firmware, no 0xFE95 auth service); MiAuth self-test at boot OK
Config read:         OK – cfg/comfort/sensor/trigger/name/time/mac/i2c identical to the host (bleak) reference
Config write:        OK – comfort temp_hi 26.0 -> 26.5 written, read back and verified by the ESP32,
                     confirmed by the host reference (COMFORT temp_hi 26.5), then restored to 26.0 (verified)
OTA:                 OK – 5395 blocks, 86 719 ms, 995 B/s, 0 retries, device OTA status 0 (Success)
Verification:        OK – reconnect after 8 s, DIS "V5.9", cfg ver 0x59 -> "Read new firmware version: 5.9"
Reconnect:           OK – first attempt (18:09:17 request, 18:09:27 connected)
Final firmware:      pvvx custom 5.9 (host reference: DIS software V5.9, DEV_ID sw_version 89, CFG ver 89)
Home Assistant:      update entity current=5.9 latest=5.9 (no update), binary "Update Available" false,
                     text "Update Status" = "Up-to-date", "Firmware Version" = "pvvx custom 5.9"
```

## Detailed timeline (ESP32 OTA log ring, `/api/log`)

```
18:07:40 Connected (MTU 23)
18:07:40 Service discovery
18:07:40 otaEnabled=1 customEnabled=1 miEnabled=0 devInfo=1
18:07:41 Hardware Revision String: B1.7
18:07:41 Software Revision String: V4.7
18:07:41 Firmware Revision String: github.com/pvvx
18:07:42 Device identified: LYWSD03MMC, pvvx custom firmware 4.7, HW B1.7 (id 4: LYWSD03MMC B1.6)
18:07:42 Live: 23.45 °C, 60.91 %, 0 mV
18:07:42 Update available: 4.7 -> 5.9
18:07:42 Warning: pvvx hw id 4 (B1.6) differs from I2C-detected class B1.7/B2.0; both use the same ATC image.
18:07:42 Warning: DIS hardware string B1.7 maps to id 5 while the firmware reports id 4.
18:07:42 Compatibility check: PASSED (hw B1.7 id 4, 4.7 -> 5.9)
18:07:42 OTA started                      (00ff, 01ff sent at 18:07:42.862)
18:09:09 Verifying... (device checks the image CRC and reboots)   (OTA send 5395 blocks - ok (86.7 s))
18:09:17 Reconnecting to verify the new firmware...
18:09:27 Reconnected
18:09:28 Software Revision String: V5.9
18:09:29 Device identified: LYWSD03MMC, pvvx custom firmware 5.9, HW B1.7 (id 4: LYWSD03MMC B1.6)
18:09:29 Firmware is up to date
18:09:29 Read new firmware version: 5.9
18:09:29 Done: OTA verified: firmware 4.7 -> 5.9
```

`GET /api/ota/status` afterwards: `state done, progress 100, bytes_sent 86308, blocks 5395/5395, speed 995.26 B/s,
elapsed_ms 86719, retries 0, device_status 0, target_version 5.9`.

## Comparison with pvvx (host reference `scripts/test_telink.py`, bleak on the PC adapter)

| Item | pvvx-style host run (before OTA) | ESP32 | after OTA (host) |
|---|---|---|---|
| Services | 181A, 1F10, 1800, 180A, OTA 1912, 1801, 180F | same 8 services (+FE95 listed in the ESP32 discovery) | same |
| DIS | LYWSD03MMC / F1.0-CFMK-LB-FLD---- / github.com/pvvx / B1.7 / V4.7 | identical | V5.9 |
| `00` dev id | hw 4, sw 0x47, services 0x000051FF | identical | sw 0x59, spec 2 (SHT4x) |
| `55` cfg | `5547871000002804a9313104b400` | identical (flg 135, flg2 16, adv 40, meas 4, rf 169, lat 49, tint 49, hver 4, av 180) | ver 0x59, flg3 10, event_adv_cnt 6 |
| `20` comfort | 21.00/26.00/30.00/60.00 | identical | identical |
| `25` sensor | Tk 17500 Hk 12500 Tz −4500 Hz −600, id 0x8510F5EC, i2c 0x88 | identical | – |
| `44` trigger | 21.00/50.00, hst −0.55/0, rds 3600/0x13 | identical | – |
| `01` name | ATC_4AE88C | identical | – |
| `02` I2C | sensor 0x88 (0x44), LCD 0x78 (0x3C) | identical | – |
| `10` MAC | A4:C1:38:4A:E8:8C rand 4C35 | identical | – |
| Activation | n/a (custom fw) | n/a | – |
| OTA | protocol identical (00ff / 01ff / blocks+CRC16 / read every 8 / 02ff); host run not executed to keep a single OTA on the device | 86.7 s | – |

Differences found: none in identification or configuration. The hw-id vs DIS-string inconsistency is a property of the
device (firmware 4.7 stored hw id 4 while the board is a B1.7 class) and is reported as warnings by the ESP32 exactly
because pvvx's own JavaScript ignores it (both ids select `ATC_vXX.bin`).

## Other devices seen during the run (auto-identify)

| MAC | Name | HW | Firmware | Update |
|---|---|---|---|---|
| A4:C1:38:0B:5F:4E | (no name in adv) | B1.4 (id 0) | pvvx 5.3 | 5.9 available |
| A4:C1:38:B8:31:E8 | P3Sypialni | B1.9 (id 3) | pvvx 5.3 | 5.9 available |
| A4:C1:38:B0:E4:4A | ATC_B0E44A | – | – | – |
| 3 more LYWSD03MMC seen | – | – | – | – |

They were only identified (read-only); no OTA was performed on them.

## Required test list

| Test | Result |
|---|---|
| test_ble_scan | PASS – `ble_state: scanning`, 5 devices within 60 s of boot |
| test_device_discovery | PASS – BTHome v2 adv decoded (23.76 °C, 61.31 %, 100 %) |
| test_gatt_connect | PASS – `Connected (MTU 23)`, 8 services |
| test_device_identification | PASS – see table |
| test_hw_detection | PASS – B1.7 / id 4 / I2C class B1.7 with warnings |
| test_fw_detection | PASS – 4.7 detected, latest 5.9, update_available true |
| test_config_read | PASS – all blocks identical to the host reference |
| test_config_write | PASS – comfort write verified by read-back on ESP32 and by host reference |
| test_activation | PARTIAL – protocol implemented and crypto self-test passes (`MiAuth self test ok`, vectors match `scripts/mi_auth_reference.py`); not exercised on hardware because no device with original Xiaomi firmware was available |
| test_ota_single_block | PASS – first status read after block 8 returned 0 (`device_status` stayed 0 for all 674 reads) |
| test_ota_full_image | PASS – 5395/5395 blocks |
| test_ota_verification | PASS – 5.9 read back |
| test_reconnect | PASS – reconnect after the device reboot on first attempt |
| test_error_handling | PASS – `flash` with unknown firmware id → 404 INVALID_REQUEST; `flash` without `confirm` → 400 INVALID_REQUEST; upload of 3000 random bytes → 422 `INVALID_IMAGE: Incorrect head in Telink OTA binary firmware`; identify of an unregistered MAC → DEVICE_NOT_FOUND; (absent-device BLE_TIMEOUT path exercised implicitly by the connect retries seen for A4:C1:38:B8:31:E8 at 18:03 – 3 attempts, reason 133) |
| test_homeassistant_entities | PASS – 26 entities via the native API; update entity transitions available → no update |

## Known deviations / limitations observed

* ESP32-C3 + Bluedroid negotiates MTU 23 (pvvx firmware default); OTA throughput ≈ 1 kB/s (86 s for 86 KB).
* During identification of other devices the scanner is paused, so advertising-based values refresh less often
  (the offline threshold was raised to 10 min for this reason).
* The USB-Serial/JTAG port of the ESP32-C3 becomes "busy" for several minutes after each reset on this PC
  (a root-owned prober); OTA over WiFi is used for all updates after the first flash.

## Second cycle: downgrade to an older compatible firmware (task §54)

* `ATC_v58.bin` (86 340 B, CRC32 0x683AFF6F) was taken from the pvvx git history (commit c794aa2…) and uploaded through
  `POST /api/firmware/upload?name=ATC_v58.bin&version=5.8&hw=0,3,4,5,10,14` → stored in the `fwstore` partition
  (`store:ATC_v58.bin`, validated on the ESP32: header, size pointer, CRC32 identical to the host check).
* `POST /api/device/A4:C1:38:4A:E8:8C/flash {"firmware":"store:ATC_v58.bin","confirm":true}` – OTA 5.9 → 5.8:
  `OTA send 5397 blocks - ok (115.7 s)` (≈746 B/s with VERY_VERBOSE logging streamed over the native API),
  reconnect after 8 s, `Software Revision String: V5.8` read back – **the thermometer accepted the older image**
  (the Telink bootloader has no downgrade protection; pvvx custom firmware keeps its configuration).
* During the verification step (right after the `55` config response, while sending `20`) the **ESP32 rebooted**
  (no panic text reached the network log). Cause under investigation with a serial monitor; the thermometer itself
  was unaffected (verified afterwards: V5.8, config intact). Suspected: heap exhaustion caused by VERY_VERBOSE
  logging over the API (min free heap dropped to 22 KB in an earlier run) – see "Known deviations".
* After a rebuild (lwIP socket reservation for the HTTP server, flasher tags at VERY_VERBOSE, BLE stack tags at DEBUG,
  reset reason reporting) the cycle was closed: `store:ATC_v58.bin` → `bundled:ATC_v59.bin`, **5.8 → 5.9**,
  `OTA send 5395 blocks - ok`, 89 357 ms, 966 B/s, 0 retries, device status 0, reconnect 18:35:46 → 18:36:06,
  `Read new firmware version: 5.9`, `Done: OTA verified: firmware 5.8 -> 5.9`; Home Assistant update entity
  `current=5.9 latest=5.9 in_progress=False`, "Update Available" false. Log: `logs/run_vv_upgrade.log`.
* Set time (`POST /api/device/<mac>/settime`): `Device time set to 1788806214 (device reports 1788806214)` =
  2026-09-07 18:36:54 local (Europe/Warsaw), matching pvvx `setDevTime()` semantics (local time).
* Heap: `min_free_heap` reached 9 344 B during the OTA while `esphome logs` streamed VERY_VERBOSE output over the
  native API – the earlier ESP32 reset (reason 3 = software reset after panic) is attributed to heap exhaustion under
  platform-wide VERY_VERBOSE logging; with BLE stack tags at DEBUG and `max_connections: 1` the run completed.
  Recommendation for verbose test sessions: stream logs over USB serial, not the API.
* Back to original Xiaomi firmware: `Original_OTA_Xiaomi_LYWSD03MMC_v1.0.0_0130.bin` is listed for hw ids
  0,3,4,5,10 (B1.4..B2.0) in the manifest and would be accepted by the compatibility module as an explicit
  "original" conversion with a warning; it was **not** flashed on the test device because coming back requires
  the Mi activation path that could not be validated on hardware in this session.

## Incident: OTA_VERIFY_FAILED on A4:C1:38:93:72:DB (P3Korytarz, B1.4, pvvx 4.7) – root cause and fix

Symptom (user run, 19:00–19:03): all 5395 blocks sent (every 8-block status read returned 0), `02ff` sent,
device disconnected, after reconnect it still reported **V4.7** → `OTA_VERIFY_FAILED`. Link quality was poor
(RSSI −89…−96, 3 connect attempts, 653 B/s instead of ~990 B/s).

Root cause – a defect in the ESP32 port, not in the thermometer: `on_ota_done_()` called `end_connection()`
immediately after the local completion event of the `02ff` write. In Bluedroid a write-without-response is
"complete" when it is *queued*, so on a slow link the tail of the transfer (last data blocks + end packet) was still
in the controller queue and was discarded by our disconnect. The Telink SDK then saw an incomplete image
(`OTA_DATA_UNCOMPLETE` / missing end command → timeout) and – by design – kept the old firmware and rebooted.
pvvx's browser flasher never disconnects; the device drops the link itself after checking the image.
On the good link of the first tests the queue happened to be empty, which is why 4.7→5.9, 5.9→5.8 and 5.8→5.9
succeeded before.

Fix (`telink_ota.cpp` `send_end_()`, `xiaomi_esp_flasher.cpp` `on_ota_done_()`): after `02ff` the ESP32 issues a
**read** of the OTA characteristic (ATT requests are ordered behind queued commands, so the response proves the
peripheral consumed everything and returns the SDK result byte), never disconnects itself (fallback after 25 s),
and reconnects 4 s after the device drops the link. Auto-identify additionally skips devices weaker than −95 dBm.

Retry with the fix (19:09–19:12, log `logs/run_vv_p3korytarz.log`): 5395 blocks, 128.9 s, 670 B/s, 0 retries;
the device rebooted before answering the final read (`final status read failed (133)`, i.e. link dropped by the
peer – expected on success), reconnect on the 3rd attempt (weak link), `Software Revision String: V5.9`,
`Done: OTA verified: firmware 4.7 -> 5.9`. Home Assistant/GUI: `firmware 5.9`, `ota_result success 4.7 -> 5.9`.
The thermometer is not defective; it behaved exactly as the Telink bootloader should on an incomplete image.

## Online firmware check (GitHub over TLS without certificate verification)

`POST /api/firmware/check` → `Fetching firmware manifest from GitHub...` → `Online manifest: 44 entries` (≈1 s).
Remote entries are merged with the bundled manifest by file name (list stays at 45 entries, images available
offline keep `bundled:` ids; remote-only images would appear as `remote:`). A first attempt with two separate
lists panicked the ESP32 (reset reason 4) while building a ~25 KB JSON document with only 25 KB free heap – fixed
by merging the lists and serialising both `/api/firmware` and `/api/devices` entry by entry. Min free heap during
the TLS handshake: 19.8 KB. Flash: 1 671 082 B (98.1 %) with TLS, 1 577 466 B (92.6 %) without `remote_manifest`.
