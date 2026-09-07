# Xiaomi ESP Flasher

ESP32 BLE gateway, **OTA flasher** and configurator for Xiaomi / Telink thermometers supported by
[pvvx/ATC_MiThermometer](https://github.com/pvvx/ATC_MiThermometer) (LYWSD03MMC and friends), built as an
ESPHome external component with its own web GUI, JSON API and Home Assistant entities.

```
 Browser / Home Assistant ── WiFi ── ESP32-C3 (ESPHome + xiaomi_esp_flasher) ── BLE ── LYWSD03MMC …
```

Proven end-to-end on real hardware (see `docs/test_report.md`): scan → identify (model / HW / FW) → read & write
configuration → Telink OTA 4.7 → 5.9 → verify → Home Assistant update entity flips to "up-to-date".

## Features

* BLE scanning with decoding of BTHome v2, pvvx custom, atc1441 and MiBeacon advertisements
* GATT identification: Device Information Service, pvvx custom command channel (0x1F1F), Telink OTA service,
  Xiaomi authentication service; hardware id / revision detection incl. I2C topology cross-check
* Compatibility matrix (`compatibility.cpp`) – unknown or inconsistent hardware **blocks** flashing
* Telink OTA exactly as `TelinkMiFlasher.html` / `TelinkOTA.html` do it (see `docs/telink_ota_protocol.md`),
  state machine with timeouts, real progress (bytes/blocks/speed/retries), verification by reconnect
* Xiaomi "Do Activation" / login (ECDH P‑256, HKDF, AES‑CCM, HMAC) for devices still on the original firmware
* Configuration model of the pvvx firmware: offsets, comfort, smiley/clock/battery display, advertising type &
  interval, TX power, latency, LCD refresh, averaging, sensor calibration, trigger/reed switch, name, PIN, time
* Firmware sources: bundled manifest + image (offline), user upload (validated, stored in a flash partition),
  optional online `firmware.json` + download from GitHub
* Web GUI (responsive, light/dark, no internet needed): device table, device page (overview / firmware / configure
  tabs / diagnostics), OTA progress bar, OTA log, firmware repository, diagnostics page, confirmation dialogs
* JSON API + Server-Sent Events
* Home Assistant: per-thermometer sensors, diagnostics, `update` entity (Install = OTA from HA), buttons,
  "Update all" queue (sequential, verified one by one)
* Persistence of known devices / aliases / last OTA result in NVS

## Layout

```
yaml/xiaomi_esp_flasher.yaml   ESPHome configuration (devices, entities, WiFi via yaml/secrets.yaml)
components/xiaomi_esp_flasher/ the external component (C++ + codegen)
web/                           GUI sources (embedded gzip'd at build time)
firmware/                      bundled pvvx manifest.json + ATC image (scripts/update_firmware.sh refreshes)
scripts/                       build.sh flash_esp.sh monitor.sh api_test.sh test_telink.py (host reference) …
docs/                          architecture, ESPHome notes, protocol, compatibility, testing, test report
reference/                     pvvx repositories (cloned, not part of the build)
```

## 1. Prepare ESPHome

The project uses the ESPHome checkout in `/home/mateusz/esphome` installed *editable* into a project venv:

```bash
cd /home/mateusz/xiaomi_esp_flasher
python3 -m venv .venv
.venv/bin/pip install -e /home/mateusz/esphome        # ESPHome 2026.10.0-dev
.venv/bin/pip install bleak cryptography aioesphomeapi # optional: host-side test tools
```

## 2. Configure

`yaml/secrets.yaml`:
```yaml
wifi_ssid: "..."
wifi_password: "..."
```

`yaml/xiaomi_esp_flasher.yaml` – relevant part:
```yaml
xiaomi_esp_flasher:
  test_device_mac: "A4:C1:38:4A:E8:8C"   # optional diagnostic device (always tracked)
  auto_identify: true                     # identify unknown / stale devices automatically (one at a time)
  identify_interval: 6h                   # re-identify to keep versions fresh
  remote_manifest: https://raw.githubusercontent.com/pvvx/ATC_MiThermometer/master/firmware.json  # optional
  fwstore_size: 0x40000                   # flash partition for uploaded/downloaded images
  devices:                                # thermometers that get Home Assistant entities
    - mac_address: "A4:C1:38:4A:E8:8C"
      name: Living Room                   # alias; entities become "Living Room Temperature" etc.
      # device_id: living_room_dev        # optional ESPHome sub-device
      # rssi: false                       # any entity can be disabled with `false` or customised
```
Devices not listed are still discovered, identified and flashable from the GUI; they simply have no HA entities
(ESPHome entities are compile-time).

## 3. Compile

```bash
scripts/build.sh                    # = .venv/bin/esphome compile yaml/xiaomi_esp_flasher.yaml
```
ESP32-C3 4 MB: ~1.67 MB per app slot. Measured flash budget (2026-09-07):

| Build variant | Flash used |
|---|---|
| full, HTTPS with CA bundle (original) | 1 690 954 B (99.2 %) |
| full, HTTPS **without certificate verification** (current default) | 1 671 082 B (98.1 %) |
| without `remote_manifest` (no TLS stack / HTTP client at all) | 1 577 466 B (92.6 %) |

The GUI (index.html + app.js + style.css, gzip) is only 15.6 KB (~1 %) and must work offline, so it stays in
flash; the bundled `ATC_v59.bin` is 86 KB; the TLS stack + HTTP client needed for GitHub (which refuses plain
HTTP) is ~90 KB. Certificate verification is deliberately disabled (`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`):
every downloaded image is validated by Telink header + CRC32 anyway. Drop `remote_manifest` if you need ~95 KB
of headroom (firmware can still be uploaded through the GUI). Note the TLS handshake needs ~30 KB of heap
(min free heap ≈ 20 KB during "Check online").

## 4. Flash the ESP32

First time (writes the partition table incl. `fwstore`) over USB, later over WiFi:
```bash
scripts/flash_esp.sh yaml/xiaomi_esp_flasher.yaml serial    # USB (waits for a free port)
scripts/flash_esp.sh                                        # OTA over WiFi (mDNS), serial fallback
scripts/monitor.sh yaml/xiaomi_esp_flasher.yaml 120         # logs for 120 s
```
Note: whenever the partition layout changes, a serial flash is required (ESP OTA keeps the old table).

## 5. WiFi and GUI

After boot the ESP32 joins WiFi (fallback AP "Xiaomi ESP Flasher AP") and serves `http://<ip>/`
(`http://xiaomi-esp-flasher.local/`). Diagnostics page shows chip, ESPHome version, IP, WiFi RSSI, heap, BLE state,
device count, last scan / OTA / error.

## 6. Discovering thermometers

Scanning is continuous; the table lists MAC, name, model, HW, firmware, temperature, humidity, battery, RSSI,
update state and status. Click a row for the device page; **Identify / Refresh** connects and reads everything.
"Add MAC…" registers a device manually (diagnostics).

## 7. Updating firmware

Device page → Firmware tab: detected hardware, installed firmware, compatibility check, target firmware select.
**Update firmware** shows a confirmation screen (device, HW, current, target, compatibility, size, CRC32, warnings)
and only then starts: `Connecting → Identifying → (Activating) → Ready → Writing xx% → Verifying → Rebooting →
Reconnecting → Done`. Progress comes from the number of blocks actually acknowledged. Failures are reported with
a code (below); nothing is retried automatically after a failed OTA – identify the device again first.
"Update all" queues every device with an available, compatible update and runs them one after another.

Firmware page: bundled/stored/remote images, "Check online (GitHub)", upload of a custom `.bin`
(validated: Telink header, size pointer, CRC32; you must declare the hardware ids).

## 8. Configuration

Device page → Configure: tabs General / Display / Advertising / Comfort / Time / Security / Advanced.
`Read Config` (re-identifies), `Set Defaults` (form only), `Send Config` (writes, reads back, compares).
`Set time` sends the ESP32 clock (SNTP, local time as pvvx does). Name / PIN / reboot / factory defaults have
their own buttons with confirmations.

## 9. Home Assistant

Add the ESPHome device (native API). Entities per configured thermometer: Temperature, Humidity, Battery,
Battery Voltage, RSSI, Firmware Version, Hardware Version, Model, Status, Last Seen, Update Status, Device Name,
Update Available (binary), **Firmware Update** (`update` entity: installed/latest version, Install = OTA, Check =
identify), buttons Identify / Flash Firmware / Set Time. Global: Flasher Status, Last Error, Detected
Thermometers, Thermometers Needing Update, OTA Progress, OTA Active, BLE Scan, Update All, Check Firmware Online.

## 10. Troubleshooting

* **Serial port "busy"** after each reset (Fedora, USB-Serial/JTAG): a root prober holds the port for minutes –
  use OTA (`scripts/flash_esp.sh`) or wait (`scripts/wait_port.sh`).
* **`fwstore: false`** in diagnostics: the partition table was never flashed over USB.
* **Update blocked: HW_UNKNOWN / INCOMPATIBLE_FIRMWARE**: the compatibility module refused – see the device
  page warnings; never bypass it.
* **ACTIVATION_REQUIRED**: original firmware 2.1.1_0159 (new B1.6 batches) needs the Mi-Home token; enter token
  and bind key on the Firmware tab (advanced) to use login instead of registration.
* **Connection reset / GUI unreachable**: sockets exhausted – the component reserves 6 lwIP sockets; keep at
  most a few browser tabs open.
* **Slow OTA (~1 kB/s)**: MTU 23 and the peripheral's connection interval; ~90 s for an 86 KB image is normal.
* Logs: `scripts/monitor.sh`, the OTA Log panel, `GET /api/log`. The shipped YAML logs the flasher at
  `VERY_VERBOSE` (full protocol traces) while the BLE stack tags stay at `DEBUG`: platform-wide `VERY_VERBOSE`
  makes the single-core ESP32-C3 miss GATT connections and stall HTTP. Stream verbose logs over USB, not the API.

## API

| Method | Path | Description |
|---|---|---|
| GET | `/api/status` | gateway diagnostics |
| GET | `/api/devices` | device table |
| GET | `/api/device/<mac>` | full device record (cfg, comfort, sensor, trigger, DIS, services …) |
| POST | `/api/device/<mac>/identify` | connect + identify (= Read Config) |
| POST | `/api/device/<mac>/config` | `{"cfg":{…},"comfort":{…},"sensor":{…},"trigger":{…}}` write + verify |
| POST | `/api/device/<mac>/defaults` | device factory defaults (`56`) |
| POST | `/api/device/<mac>/settime` `/name` `/pin` `/reboot` | as named (`{"name":"…"}`, `{"pin":123456}`) |
| POST | `/api/device/<mac>/activate` | Mi activation (optional `{"token":…,"bind_key":…}` = login) |
| POST | `/api/device/<mac>/flash` | `{"firmware":"<id>","confirm":true}` – 409 unless compatible |
| POST | `/api/device/<mac>/add` `/forget` `/alias` | registry management |
| GET | `/api/firmware` | available images (bundled / store / remote) |
| POST | `/api/firmware/upload?name=&version=&hw=0,3` | raw `.bin` body (octet-stream) |
| POST | `/api/firmware/check` | fetch online manifest (if configured) |
| POST | `/api/scan`, `/api/queue/all` | scan / update all |
| GET | `/api/log?since=N`, `/api/ota/status`, `/api/events` (SSE) | log ring, OTA progress, live events |

Error codes: `BLE_TIMEOUT DEVICE_NOT_FOUND DEVICE_UNSUPPORTED HW_UNKNOWN FW_UNKNOWN ACTIVATION_FAILED
ACTIVATION_REQUIRED INCOMPATIBLE_FIRMWARE OTA_WRITE_FAILED OTA_VERIFY_FAILED OTA_DEVICE_ERROR DEVICE_DISCONNECTED
NOT_ENOUGH_MEMORY NOT_ENOUGH_STORAGE INVALID_IMAGE BUSY INVALID_REQUEST CONFIG_MISMATCH DOWNLOAD_FAILED INTERNAL_ERROR`.

## Safety rules implemented

1. No OTA without a fresh identification (HW + FW) in the same connection.
2. `check_compatibility()` must pass (hardware id listed for the image, image validated, size limit, cloud-token
   firmware refused, original images only as explicit conversion, hw id 14 never gets an original image).
3. GUI confirmation with all facts; API requires `confirm:true`.
4. One OTA at a time; a failed OTA clears the queue and is never retried automatically.
5. Device-reported OTA errors (packet loss, CRC, flash, timeout, image check) abort immediately – the Telink
   bootloader keeps the old firmware until the new image passes its CRC.

## Limitations

* Tested on LYWSD03MMC only (custom firmware 4.7 → 5.9); Mi activation implemented from the pvvx source but not
  exercised on hardware (no device with original firmware available).
* Signed original images (`sign_*.bin` companions) are not supported.
* ESP32-C3 MTU 23 → ~1 kB/s OTA; Zigbee images are not handled.
* Home Assistant entities exist only for devices listed in YAML.
