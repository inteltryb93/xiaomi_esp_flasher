# ESPHome architecture notes (checkout /home/mateusz/esphome, 2026.10.0-dev, esp-idf 5.5.5, ESP32-C3)

Written before implementation, updated with what was actually used.  Line references are to the local checkout.

## What is reused from ESPHome

| Need | ESPHome API used | Notes |
|---|---|---|
| BLE stack init (Bluedroid), event fan-out | `esp32_ble` (`ESP32BLE`, `global_ble`) via `esp32_ble_tracker` | events are queued from the BT task and dispatched on the **main loop** (`esp32_ble/ble.cpp` `loop()`), so all our BLE callbacks run on the main loop – no locking against `loop()` needed. |
| BLE scanning + advertisement parsing | `esp32_ble_tracker::ESP32BLETracker`, listener interface `ble_device_base::ESPBTDeviceListener::parse_device(const ESPBTDevice&)`, `ESPBTDevice::get_service_datas()/get_name()/get_rssi()/address_uint64()/get_address_type()` | registered from Python with `esp32_ble_tracker.register_ble_device(hub, config)`; scan parameters from YAML (`scan_parameters`). `parse_device` returns `false` so other listeners still see the packets. |
| GATT client (connect, discovery, MTU, notify registration, CCCD write, disconnect) | `esp32_ble_client::BLEClientBase` (`ble_client_base.h`) subclassed by `TelinkBleClient` | registered as a tracker client with `esp32_ble_tracker.register_client()` (this also defines `USE_ESP32_BLE_DEVICE` so `get_service()/get_characteristic()` compile). The tracker stops the scan and calls `connect()` when the client is put into `ClientState::DISCOVERED` – the same mechanism `parse_device()` uses with `auto_connect`. MTU is requested automatically on `ESP_GATTC_CONNECT_EVT` (`mtu_` member). |
| Characteristic read/write | raw IDF calls `esp_ble_gattc_read_char()` / `esp_ble_gattc_write_char()` + `ESP_GATTC_READ_CHAR_EVT` / `WRITE_CHAR_EVT` / `NOTIFY_EVT` in our `gattc_event_handler()` override | `BLECharacteristic` only offers `write_value()`; there is no read helper, so the client layer implements one-op-at-a-time read/write/subscribe with callbacks + timeouts. |
| Connection slots | `esp32_ble.consume_connection_slots(1, "xiaomi_esp_flasher")` + `esp32_ble: max_connections` | one sequential connection at a time. |
| HTTP server | `web_server_base::WebServerBase` (`add_handler()`, `init()`), `AsyncWebHandler` interface of `web_server_idf` (`canHandle/handleRequest/handleBody`) | `handleBody` streams raw POST bodies (`application/octet-stream`) chunk by chunk – used for firmware uploads straight into flash. Handlers run on the **httpd task**; shared state is protected with `esphome::Mutex`, actions are queued to the main loop. |
| Server-Sent Events | hand-rolled on `httpd_req_t` exactly like `web_server_idf::AsyncEventSourceResponse` (chunked transfer, `httpd_socket_send`, `free_ctx` for disconnect) | the in-tree `AsyncEventSource` is hard-wired to the `web_server` component and cannot be reused. |
| JSON | `esphome/components/json` (ArduinoJson 7.4.3): `json::parse_json`, `json::JsonBuilder`, `JsonDocument` | |
| Home Assistant entities | `sensor::Sensor`, `text_sensor::TextSensor`, `binary_sensor::BinarySensor`, `button::Button`, `update::UpdateEntity` | created from the hub's Python config (`devices:` list) with `sensor.new_sensor()` etc.; the update entity subclass implements `perform()/check()`; HA "Install" calls `perform(false)` through `api_connection.cpp` `on_update_command_request`. |
| Persistence | `global_preferences->make_preference<T>(hash)` (NVS) | known-device table (12 × 100 B). Firmware images are **not** stored in NVS. |
| Firmware storage | custom raw partition added with `esp32.add_partition("fwstore", 0x40, 0x00, size)` and accessed with `esp_partition_*` | default 256 KiB; app slots shrink accordingly (~1.66 MB each on 4 MB). |
| Scheduler / non-blocking | `Component::set_timeout/set_interval/defer/enable_loop_soon_any_context`, `App.feed_wdt()` | OTA and identification are callback chains driven by GATT events; the state machine has explicit per-step deadlines evaluated in `loop()`. |
| Time | `time: platform: sntp` sets the system clock; we use libc `time()`/`localtime_r()` | pvvx "Set time" sends local time (`Date.now() - tz offset`), computed from `tm_gmtoff`. |
| HTTPS download (optional) | ESP-IDF `esp_http_client` + `esp_crt_bundle_attach` (sdkconfig `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`) | enabled only when `remote_manifest` is configured (`USE_XIAOMI_FLASHER_REMOTE`). |
| Crypto for Mi activation | mbedtls bundled with ESP-IDF: `mbedtls_ecdh`, `mbedtls_hkdf`, `mbedtls_ccm`, `mbedtls_md_hmac`, `esp_fill_random` | no ESPHome wrapper exists; used directly. |
| Logging | `ESP_LOGx` with per-file tags `xiaomi_flasher`, `xiaomi_flasher.ble`, `.ota`, `.miauth`, `.web`, `.fw`, `.store` | the OTA log ring (150 lines) mirrors INFO lines to the GUI. |

## What had to be written (and why)

* `telink_ble_client.*` – ESPHome has no request/response abstraction for GATT reads/writes with completion callbacks and timeouts; `ble_client` exposes only `write_value()` and per-node event fan-out.
* `telink_ota.*` – Telink SDK OTA protocol (not present anywhere in ESPHome).
* `mi_auth.*` – Xiaomi registration/login protocol + key derivation.
* `device_config.*` – pvvx configuration model and codec.
* `compatibility.*` – hardware/firmware matrix (single source of truth).
* `firmware_store.*`, `firmware_provider.*` – raw-partition image staging, pvvx `firmware.json` parsing, bundled/uploaded/remote sources behind one `FirmwareProvider` interface.
* `xiaomi_esp_flasher.*` (+ `_web.cpp`) – device registry, session state machine, HTTP API, SSE, HA entities, persistence.
* `web/` – standalone GUI (no CDN assets), embedded gzip'd in flash at build time by the component's `__init__.py`.

## Things that differ from older ESPHome versions (verified in this checkout)

* `ESP32_BLE_TRACKER_SCHEMA` → `ESP_BLE_DEVICE_SCHEMA`; `register_ble_tracker()` → `register_ble_device()`.
* `ESPBTDevice::address_str()` is deprecated → `address_str_to(char[18])`; `get_name()` returns `StringRef`.
* No `BLEClientBase::set_remote_mtu()`; no `BLECharacteristic::read()`.
* `AsyncWebServerRequest::url()` does not exist on IDF → `url_to(std::span<char, URL_BUF_SIZE>)`.
* `cv.only_with_esp_idf` no longer exists (esp-idf is the only ESP32 framework); `CONF_RSSI` is not in `esphome.const`.
* Partition table is generated from `esp32.add_partition()` calls; custom raw partitions need numeric type/subtype.
