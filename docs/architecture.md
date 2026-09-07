# Architecture

```
                         WiFi                                   BLE
 Browser / Home Assistant ──── ESP32-C3 (ESPHome + xiaomi_esp_flasher) ──── Xiaomi / Telink thermometers
```

## Layers (components/xiaomi_esp_flasher)

| Layer | Files | Responsibility |
|---|---|---|
| GATT client | `telink_ble_client.{h,cpp}` (`TelinkBleClient : esp32_ble_client::BLEClientBase`) | connect / discovery / read / write / subscribe / disconnect with callbacks and per-operation timeouts. No protocol knowledge. |
| Telink OTA | `telink_ota.{h,cpp}` (`TelinkOtaProtocol`) | the OTA transfer only: `00ff`, `01ff`, 16-byte blocks with CRC16, status read every 8 blocks, `02ff` end. Explicit state (`STARTING → WRITING ⇄ CHECKING → FINISHING → DONE/FAILED`), real progress from sent blocks. |
| Mi activation | `mi_auth.{h,cpp}` (`MiAuth`) | Xiaomi registration + login on 0xFE95 (ECDH P‑256, HKDF, AES‑CCM, HMAC). Only used on stock firmware. |
| Device model | `xiaomi_device.{h,cpp}` (`XiaomiDevice`, advertising parsers) | identity, hardware, firmware, configuration snapshot, measurements, eligibility, last error / OTA result. |
| Config model | `device_config.{h,cpp}` (`DeviceConfig`, `ComfortConfig`, `SensorConfig`, `TriggerConfig`, `DevId`, `Measurement`) | byte-exact codec of the pvvx command frames. |
| Compatibility | `compatibility.{h,cpp}` | `HardwareInfo` × `FirmwareInfo` → `CompatResult`; semantic version compare; LYWSD03MMC revision table. Single authority. |
| Image validation | `telink_image.{h,cpp}` | Telink header / size pointer / CRC32 (pvvx variant), CRC16-Modbus. |
| Firmware sources | `firmware_provider.{h,cpp}` (`FirmwareProvider`, `LocalFirmwareProvider`, `RemoteGithubFirmwareProvider`), `firmware_store.{h,cpp}` (`FirmwareStore`) | manifest parsing (`firmware.json`), bundled images (rodata), uploaded/downloaded image in the `fwstore` partition. |
| Hub | `xiaomi_esp_flasher.{h,cpp}`, `xiaomi_esp_flasher_web.cpp` | scan listener, device registry, **session state machine**, HTTP API + SSE, HA entities, persistence. |
| Codegen | `__init__.py` | YAML schema, entity generation per device, asset/firmware embedding, partition + sdkconfig. |

## Session state machine (one BLE job at a time)

```
IDLE ──job──▶ CONNECTING ─▶ CONNECTED ─▶ IDENTIFYING ─┬─▶ (identify/read)  ─▶ disconnect ─▶ SUCCESS
                 │ (3 attempts)                       ├─▶ WRITING (config/time/name/pin)     ─▶ SUCCESS
                 ▼                                    ├─▶ ACTIVATING (stock fw) ─▶ READY ─┐
               ERROR ◀──── timeout / disconnect ◀─────┴─▶ READY ─▶ [ERASING] ─▶ WRITING ──┴─▶ REBOOTING
                                                                                              ▼
                                                              SUCCESS ◀─ VERIFYING ◀─ RECONNECTING (≤6 tries)
```
Every state has a deadline (`step_deadline_`) checked in `loop()`; a failure aborts OTA/auth, disconnects,
records the error on the device and clears the update queue (no automatic retry after a failed OTA).

Jobs: `IDENTIFY, READ_CONFIG, WRITE_CONFIG, SET_DEFAULTS, SET_TIME, SET_NAME, SET_PIN, REBOOT, ACTIVATE, FLASH`.
Sources of jobs: HTTP API, HA buttons / update entity, the update queue (`Update all`), auto-identify.

## Identification (Phase 5/6)
1. service list → firmware kind (`0x1F10` custom, `ebe0ccb0`/`0xFE95` stock, OTA service present?)
2. DIS strings 0x2A24..0x2A29
3. custom fw: subscribe 0x1F1F, commands `00 55 20 25 44 01 23 02 10`, then read 0x2A6E/0x2A6F/0x2A19
4. stock fw: subscribe `ebe0ccc1` for one live sample (≤ 8 s)
5. `HardwareInfo` derivation (see docs/compatibility.md) → eligibility (`update_available`, `compat`)
6. persist (NVS) → publish entities → SSE `device` event

## Threads
* main loop: BLE events, session, entities, SSE pump, log ring
* httpd task: JSON building (under `mutex_`), uploads written straight to the `fwstore` partition, actions queued
* BT task: never runs our code (ESPHome marshals events to the main loop)

## Memory
* firmware image bytes are never copied to RAM: OTA reads 16 bytes at a time through an `ImageReader`
  (rodata for bundled images, `esp_partition_read` for stored ones)
* JSON documents are built per request; device list ≈ 600 B/device
* log ring: 150 lines, SSE backlog 60 events

## HTTP API
See README.md §API. All POST actions return `{"ok":true,"queued":true}` immediately; results arrive via
`/api/events` (SSE) and `/api/log`. `POST /api/device/<mac>/flash` requires `{"firmware":"<id>","confirm":true}`
and is refused (HTTP 409) unless the device is identified and `check_compatibility()` passes.
