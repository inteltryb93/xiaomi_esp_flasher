# Testing

All tests run against real hardware: ESP32-C3 (`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*`,
WiFi `xiaomi-esp-flasher.local`) and the LYWSD03MMC `A4:C1:38:4A:E8:8C`.  Results of the actual runs are in
`docs/test_report.md`; raw logs in `logs/`.

Two tool chains are used:

* **host reference** – `scripts/test_telink.py` (bleak on the PC's Bluetooth adapter) reproduces the pvvx
  TelinkMiFlasher steps and serves as the "pvvx comparison" (§39 of the task): identification, config read/write,
  time set, OTA. It logs to `logs/host_*.log`.
* **ESP32 implementation** – the ESPHome build (`scripts/build.sh`, `scripts/flash_esp.sh`), driven through the
  HTTP API (`curl`), observed through `scripts/monitor.sh` (ESPHome logs) and `/api/log` / `/api/events`.

| Test | Procedure | Pass criterion |
|---|---|---|
| `test_ble_scan` | boot ESP32; `curl /api/status` → `ble_state`; `curl /api/devices` | `ble_state == scanning`, device list non-empty within 30 s |
| `test_device_discovery` | `curl /api/devices` | entry for `A4:C1:38:4A:E8:8C` with `name ATC_…`, `adv_format`, temperature/humidity/battery from BTHome |
| `test_gatt_connect` | `POST /api/device/<mac>/identify`; watch log | `Connected (MTU …)`, `Service discovery`, service list contains OTA service + 0x1F10 |
| `test_device_identification` | same job | `firmware_kind`, `model`, DIS strings, `dev_id`, `cfg` present in `GET /api/device/<mac>` |
| `test_hw_detection` | same job | `hw_id` / `hardware` consistent with the host reference (`hver=4`, DIS `B1.7`, I2C 0x44/0x3C) and warnings listed |
| `test_fw_detection` | same job | `firmware == "4.7"` before OTA, `update_available == true`, `latest_version == "5.9"` |
| `test_config_read` | identify → `GET /api/device/<mac>` | `cfg`, `comfort`, `sensor`, `trigger`, `device_name`, `device_time` equal the host reference values |
| `test_config_write` | `POST /api/device/<mac>/config` with a changed value (e.g. comfort temp_hi 26.5) → read back → restore | log `Configuration successfully written (verified)`; read-back equals request; host reference confirms the value |
| `test_activation` | only possible on stock firmware. `MiAuth::self_test()` (known-answer vectors from `scripts/mi_auth_reference.py`) runs at boot; the on-device flow is exercised when a stock device is available (`POST /api/device/<mac>/activate`) | self test prints the same hkdf/ccm/hmac hex as the Python reference |
| `test_ota_single_block` | first 8 blocks of the OTA: log at DEBUG shows write of block 0 and the status read after block 7 with status 0 | status byte 0 (`Success`) |
| `test_ota_full_image` | `POST /api/device/<mac>/flash {"firmware":"bundled:ATC_v59.bin","confirm":true}` | `OTA send N blocks - ok`, progress reaches 100 %, elapsed/speed reported |
| `test_ota_verification` | automatic after OTA: reconnect + identify | `Read new firmware version: 5.9`, `ota_result == "success 4.7 -> 5.9"` |
| `test_reconnect` | part of the OTA job (RECONNECTING/VERIFYING states) and re-identification afterwards | device reconnects within 6 attempts, `session` returns to `SUCCESS` |
| `test_error_handling` | (a) flash with an id not listed for the hardware → HTTP 409 `INCOMPATIBLE_FIRMWARE`; (b) identify a MAC that is not in range → `BLE_TIMEOUT`/`DEVICE_NOT_FOUND`; (c) upload a non-Telink file → 422 `INVALID_IMAGE` | error codes as listed in README §Errors, device `last_error` set, update queue cleared |
| `test_homeassistant_entities` | `esphome logs` shows the entity list; the native API (`aioesphomeapi`) lists entities; HA shows the `update` entity state | update entity `latest_version`/`current_version` follow the device; state `available`→`no update` after OTA |

Helper commands:

```bash
scripts/build.sh                      # compile
scripts/flash_esp.sh                  # OTA over WiFi (serial fallback)
scripts/monitor.sh yaml/xiaomi_esp_flasher.yaml 120   # 2 min of logs
curl -s http://xiaomi-esp-flasher.local/api/devices | python3 -m json.tool
curl -s -X POST http://xiaomi-esp-flasher.local/api/device/A4:C1:38:4A:E8:8C/identify
curl -s http://xiaomi-esp-flasher.local/api/log | python3 -m json.tool
.venv/bin/python scripts/test_telink.py A4:C1:38:4A:E8:8C identify     # host reference
```
