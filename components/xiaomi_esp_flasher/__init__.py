"""ESPHome external component: xiaomi_esp_flasher.

BLE gateway / flasher / configurator for Xiaomi & Telink thermometers running the pvvx
ATC_MiThermometer firmware (or the original Xiaomi firmware).
"""
import gzip
import json
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import (
    binary_sensor,
    button,
    esp32,
    esp32_ble,
    esp32_ble_client,
    esp32_ble_tracker,
    sensor,
    text_sensor,
    update,
    web_server_base,
)
from esphome.components.esp32_ble import BTLoggers
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import (
    CONF_BATTERY_VOLTAGE,
    CONF_DEVICE_ID,
    CONF_HUMIDITY,
    CONF_ID,
    CONF_MAC_ADDRESS,
    CONF_MODEL,
    CONF_NAME,
    CONF_TEMPERATURE,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_FIRMWARE,
    DEVICE_CLASS_HUMIDITY,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_UPDATE,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_PERCENT,
    UNIT_VOLT,
)
from esphome.core import CORE, ID, HexInt

AUTO_LOAD = [
    "esp32_ble_client",
    "web_server_base",
    "json",
    "watchdog",
    "sensor",
    "text_sensor",
    "binary_sensor",
    "button",
    "update",
]
DEPENDENCIES = ["esp32", "esp32_ble_tracker"]
CODEOWNERS = ["@tryb103"]

ns = cg.esphome_ns.namespace("xiaomi_esp_flasher")
XiaomiEspFlasher = ns.class_("XiaomiEspFlasher", cg.Component, esp32_ble_tracker.ESPBTDeviceListener)
TelinkBleClient = ns.class_("TelinkBleClient", esp32_ble_client.BLEClientBase)
XiaomiFirmwareUpdate = ns.class_("XiaomiFirmwareUpdate", update.UpdateEntity, cg.Component)
XiaomiFlasherButton = ns.class_("XiaomiFlasherButton", button.Button, cg.Component)
ButtonAction = ns.enum("ButtonAction", is_class=True)
DeviceEntities = ns.struct("DeviceEntities")

CONF_RSSI = "rssi"
CONF_BLE_CLIENT_ID = "ble_client_id"
CONF_TEST_DEVICE_MAC = "test_device_mac"
CONF_AUTO_IDENTIFY = "auto_identify"
CONF_IDENTIFY_INTERVAL = "identify_interval"
CONF_REMOTE_MANIFEST = "remote_manifest"
CONF_FIRMWARE_DIR = "firmware_dir"
CONF_WEB_DIR = "web_dir"
CONF_FWSTORE_SIZE = "fwstore_size"
CONF_DEVICES = "devices"
CONF_BATTERY = "battery"
CONF_FIRMWARE = "firmware"
CONF_HARDWARE = "hardware"
CONF_STATUS = "status"
CONF_LAST_SEEN = "last_seen"
CONF_UPDATE_STATUS = "update_status"
CONF_DEVICE_NAME = "device_name"
CONF_UPDATE_AVAILABLE = "update_available"
CONF_UPDATE = "update"
CONF_IDENTIFY_BUTTON = "identify_button"
CONF_FLASH_BUTTON = "flash_button"
CONF_SET_TIME_BUTTON = "set_time_button"
CONF_LAST_ERROR = "last_error"
CONF_DEVICE_COUNT = "device_count"
CONF_OTA_PROGRESS = "ota_progress"
CONF_UPDATES_AVAILABLE = "updates_available"
CONF_OTA_ACTIVE = "ota_active"
CONF_SCAN_BUTTON = "scan_button"
CONF_UPDATE_ALL_BUTTON = "update_all_button"
CONF_CHECK_ONLINE_BUTTON = "check_online_button"

DEVICE_ENTITY_DEFAULTS = {
    CONF_TEMPERATURE: "Temperature",
    CONF_HUMIDITY: "Humidity",
    CONF_BATTERY: "Battery",
    CONF_BATTERY_VOLTAGE: "Battery Voltage",
    CONF_RSSI: "RSSI",
    CONF_FIRMWARE: "Firmware Version",
    CONF_HARDWARE: "Hardware Version",
    CONF_MODEL: "Model",
    CONF_STATUS: "Status",
    CONF_LAST_SEEN: "Last Seen",
    CONF_UPDATE_STATUS: "Update Status",
    CONF_DEVICE_NAME: "Device Name",
    CONF_UPDATE_AVAILABLE: "Update Available",
    CONF_UPDATE: "Firmware Update",
    CONF_IDENTIFY_BUTTON: "Identify",
    CONF_FLASH_BUTTON: "Flash Firmware",
    CONF_SET_TIME_BUTTON: "Set Time",
}


def _fill_device_defaults(conf):
    """Generate default entity configs ("<alias> Temperature" ...) for keys the user did not give."""
    conf = dict(conf)
    alias = conf.get(CONF_NAME, str(conf[CONF_MAC_ADDRESS]))
    for key, label in DEVICE_ENTITY_DEFAULTS.items():
        if key in conf:
            if conf[key] is False:
                del conf[key]
            continue
        conf[key] = {CONF_NAME: f"{alias} {label}"}
    if CONF_DEVICE_ID in conf:
        for key in DEVICE_ENTITY_DEFAULTS:
            if key in conf and CONF_DEVICE_ID not in conf[key]:
                conf[key][CONF_DEVICE_ID] = conf[CONF_DEVICE_ID]
    return conf


def _entity_or_false(schema):
    return cv.Any(cv.boolean, schema)


DEVICE_SCHEMA = cv.All(
    _fill_device_defaults,
    cv.Schema(
        {
            cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
            cv.Optional(CONF_NAME): cv.string,
            cv.Optional(CONF_DEVICE_ID): cv.string,
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS, accuracy_decimals=2,
                device_class=DEVICE_CLASS_TEMPERATURE, state_class=STATE_CLASS_MEASUREMENT),
            cv.Optional(CONF_HUMIDITY): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT, accuracy_decimals=2,
                device_class=DEVICE_CLASS_HUMIDITY, state_class=STATE_CLASS_MEASUREMENT),
            cv.Optional(CONF_BATTERY): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT, accuracy_decimals=0,
                device_class=DEVICE_CLASS_BATTERY, state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
            cv.Optional(CONF_BATTERY_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement=UNIT_VOLT, accuracy_decimals=3,
                device_class=DEVICE_CLASS_VOLTAGE, state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
            cv.Optional(CONF_RSSI): sensor.sensor_schema(
                unit_of_measurement=UNIT_DECIBEL_MILLIWATT, accuracy_decimals=0,
                device_class=DEVICE_CLASS_SIGNAL_STRENGTH, state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
            cv.Optional(CONF_FIRMWARE): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:chip"),
            cv.Optional(CONF_HARDWARE): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:memory"),
            cv.Optional(CONF_MODEL): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:thermometer"),
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:bluetooth"),
            cv.Optional(CONF_LAST_SEEN): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:clock-outline"),
            cv.Optional(CONF_UPDATE_STATUS): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:update"),
            cv.Optional(CONF_DEVICE_NAME): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:tag"),
            cv.Optional(CONF_UPDATE_AVAILABLE): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_UPDATE),
            cv.Optional(CONF_UPDATE): update.update_schema(XiaomiFirmwareUpdate, device_class=DEVICE_CLASS_FIRMWARE),
            cv.Optional(CONF_IDENTIFY_BUTTON): button.button_schema(XiaomiFlasherButton, icon="mdi:magnify", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
            cv.Optional(CONF_FLASH_BUTTON): button.button_schema(XiaomiFlasherButton, icon="mdi:flash", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
            cv.Optional(CONF_SET_TIME_BUTTON): button.button_schema(XiaomiFlasherButton, icon="mdi:clock", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        }
    ),
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(XiaomiEspFlasher),
            cv.GenerateID(CONF_BLE_CLIENT_ID): cv.declare_id(TelinkBleClient),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
            cv.Optional(CONF_TEST_DEVICE_MAC): cv.mac_address,
            cv.Optional(CONF_AUTO_IDENTIFY, default=True): cv.boolean,
            cv.Optional(CONF_IDENTIFY_INTERVAL, default="6h"): cv.positive_time_period_seconds,
            cv.Optional(CONF_REMOTE_MANIFEST): cv.url,
            cv.Optional(CONF_FIRMWARE_DIR, default="../firmware"): cv.directory,
            cv.Optional(CONF_WEB_DIR, default="../web"): cv.directory,
            cv.Optional(CONF_FWSTORE_SIZE, default=0x40000): cv.int_range(min=0x10000, max=0x100000),
            cv.Optional(CONF_DEVICES, default=[]): cv.ensure_list(DEVICE_SCHEMA),
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:state-machine"),
            cv.Optional(CONF_LAST_ERROR): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:alert"),
            cv.Optional(CONF_DEVICE_COUNT): sensor.sensor_schema(accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:counter"),
            cv.Optional(CONF_OTA_PROGRESS): sensor.sensor_schema(unit_of_measurement=UNIT_PERCENT, accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:progress-upload"),
            cv.Optional(CONF_UPDATES_AVAILABLE): sensor.sensor_schema(accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:update"),
            cv.Optional(CONF_OTA_ACTIVE): binary_sensor.binary_sensor_schema(device_class="running", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
            cv.Optional(CONF_SCAN_BUTTON): button.button_schema(XiaomiFlasherButton, icon="mdi:bluetooth-audio"),
            cv.Optional(CONF_UPDATE_ALL_BUTTON): button.button_schema(XiaomiFlasherButton, icon="mdi:update"),
            cv.Optional(CONF_CHECK_ONLINE_BUTTON): button.button_schema(XiaomiFlasherButton, icon="mdi:cloud-download"),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA),
    esp32_ble.consume_connection_slots(1, "xiaomi_esp_flasher"),
    cv.only_on_esp32,
)


def _final_validate(config):
    """Reserve lwIP sockets for the HTTP server (GUI, API polling, SSE, uploads) – like web_server does."""
    from esphome.components import socket

    socket.consume_sockets(6, "xiaomi_esp_flasher")(config)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


def _add_gz_asset(name: str, path: Path) -> tuple[str, str]:
    data = gzip.compress(path.read_bytes(), mtime=0)
    arr = ", ".join(str(b) for b in data)
    cg.add_global(cg.RawExpression(f"static const uint8_t XF_ASSET_{name}[{len(data)}] PROGMEM = {{{arr}}}"))
    return f"XF_ASSET_{name}", str(len(data))


async def _new_button(conf, hub, action, mac=0):
    var = await button.new_button(conf)
    await cg.register_component(var, conf)
    cg.add(var.set_parent(hub))
    cg.add(var.set_action(action))
    if mac:
        cg.add(var.set_mac(mac))
    return var


async def to_code(config):
    esp32_ble.register_bt_logger(BTLoggers.GATT, BTLoggers.SMP)
    cg.add_define("USE_ESP32_BLE_UUID")
    # mbedtls features needed by the Mi activation (ECDH P-256 / HKDF / CCM are enabled by default in IDF 5, be explicit)
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_HKDF_C", True)
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CCM_C", True)
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_ECDH_C", True)
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED", True)
    # raw partition for uploaded / downloaded firmware images
    esp32.add_partition("fwstore", 0x40, 0x00, config[CONF_FWSTORE_SIZE])

    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    hub = cg.new_Pvariable(config[CONF_ID], base)
    await cg.register_component(hub, config)
    await esp32_ble_tracker.register_ble_device(hub, config)

    client = cg.new_Pvariable(config[CONF_BLE_CLIENT_ID])
    await cg.register_component(client, {})
    await esp32_ble_tracker.register_client(client, config)
    cg.add(hub.set_client(client))

    if CONF_TEST_DEVICE_MAC in config:
        cg.add(hub.set_test_device_mac(config[CONF_TEST_DEVICE_MAC].as_hex))
    cg.add(hub.set_auto_identify(config[CONF_AUTO_IDENTIFY]))
    cg.add(hub.set_identify_interval(config[CONF_IDENTIFY_INTERVAL]))

    if CONF_REMOTE_MANIFEST in config:
        cg.add_define("USE_XIAOMI_FLASHER_REMOTE")
        cg.add(hub.set_remote_manifest_url(config[CONF_REMOTE_MANIFEST]))
        esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
        esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN", True)
        esp32.add_idf_sdkconfig_option("CONFIG_ESP_TLS_INSECURE", False)
        for comp in ("esp_http_client", "esp-tls", "mbedtls"):
            try:
                esp32.include_builtin_idf_component(comp)
            except Exception:  # noqa: BLE001 - older/newer API differences
                pass

    # ---- bundled firmware (manifest.json + *.bin from firmware_dir) ----
    fw_dir = Path(CORE.relative_config_path(config[CONF_FIRMWARE_DIR]))
    manifest_path = fw_dir / "manifest.json"
    manifest_text = manifest_path.read_text() if manifest_path.exists() else '{"version":0}'
    json.loads(manifest_text)  # validate
    cg.add(hub.set_manifest_json(manifest_text))
    for i, bin_path in enumerate(sorted(fw_dir.glob("*.bin"))):
        data = bin_path.read_bytes()
        arr_id = cg.RawExpression(f"XF_FW_{i}")
        cg.add_global(cg.RawExpression(
            f"static const uint8_t XF_FW_{i}[{len(data)}] PROGMEM = {{{', '.join(str(b) for b in data)}}}"))
        cg.add(hub.add_bundled_image(bin_path.name, arr_id, len(data)))

    # ---- web assets (gzip'd, served from flash) ----
    web_dir = Path(CORE.relative_config_path(config[CONF_WEB_DIR]))
    html = _add_gz_asset("HTML", web_dir / "index.html")
    js = _add_gz_asset("JS", web_dir / "app.js")
    css = _add_gz_asset("CSS", web_dir / "style.css")
    cg.add(hub.set_assets(cg.RawExpression(html[0]), cg.RawExpression(html[1]), cg.RawExpression(js[0]),
                          cg.RawExpression(js[1]), cg.RawExpression(css[0]), cg.RawExpression(css[1])))

    # ---- global entities ----
    if CONF_STATUS in config:
        cg.add(hub.set_status_text_sensor(await text_sensor.new_text_sensor(config[CONF_STATUS])))
    if CONF_LAST_ERROR in config:
        cg.add(hub.set_last_error_text_sensor(await text_sensor.new_text_sensor(config[CONF_LAST_ERROR])))
    if CONF_DEVICE_COUNT in config:
        cg.add(hub.set_device_count_sensor(await sensor.new_sensor(config[CONF_DEVICE_COUNT])))
    if CONF_OTA_PROGRESS in config:
        cg.add(hub.set_ota_progress_sensor(await sensor.new_sensor(config[CONF_OTA_PROGRESS])))
    if CONF_UPDATES_AVAILABLE in config:
        cg.add(hub.set_updates_available_sensor(await sensor.new_sensor(config[CONF_UPDATES_AVAILABLE])))
    if CONF_OTA_ACTIVE in config:
        cg.add(hub.set_ota_active_binary_sensor(await binary_sensor.new_binary_sensor(config[CONF_OTA_ACTIVE])))
    if CONF_SCAN_BUTTON in config:
        await _new_button(config[CONF_SCAN_BUTTON], hub, ButtonAction.SCAN)
    if CONF_UPDATE_ALL_BUTTON in config:
        await _new_button(config[CONF_UPDATE_ALL_BUTTON], hub, ButtonAction.UPDATE_ALL)
    if CONF_CHECK_ONLINE_BUTTON in config:
        await _new_button(config[CONF_CHECK_ONLINE_BUTTON], hub, ButtonAction.CHECK_ONLINE)

    # ---- per-device entities ----
    for dev in config[CONF_DEVICES]:
        mac = dev[CONF_MAC_ADDRESS].as_hex
        cg.add(hub.add_known_device(mac, dev.get(CONF_NAME, "")))
        ent_var = cg.variable(ID(f"xf_ent_{str(dev[CONF_MAC_ADDRESS]).replace(':', '')}", is_declaration=True, type=DeviceEntities), cg.RawExpression("{}"))
        cg.add(cg.RawExpression(f"{ent_var}.mac = {mac}"))
        for key, setter in (
            (CONF_TEMPERATURE, "temperature"), (CONF_HUMIDITY, "humidity"), (CONF_BATTERY, "battery"),
            (CONF_BATTERY_VOLTAGE, "battery_voltage"), (CONF_RSSI, "rssi"),
        ):
            if key in dev:
                s = await sensor.new_sensor(dev[key])
                cg.add(cg.RawExpression(f"{ent_var}.{setter} = {s}"))
        for key, setter in (
            (CONF_FIRMWARE, "firmware"), (CONF_HARDWARE, "hardware"), (CONF_MODEL, "model"), (CONF_STATUS, "status"),
            (CONF_LAST_SEEN, "last_seen"), (CONF_UPDATE_STATUS, "update_status"), (CONF_DEVICE_NAME, "device_name"),
        ):
            if key in dev:
                s = await text_sensor.new_text_sensor(dev[key])
                cg.add(cg.RawExpression(f"{ent_var}.{setter} = {s}"))
        if CONF_UPDATE_AVAILABLE in dev:
            s = await binary_sensor.new_binary_sensor(dev[CONF_UPDATE_AVAILABLE])
            cg.add(cg.RawExpression(f"{ent_var}.update_available = {s}"))
        if CONF_UPDATE in dev:
            u = await update.new_update(dev[CONF_UPDATE])
            await cg.register_component(u, dev[CONF_UPDATE])
            cg.add(u.set_parent(hub))
            cg.add(u.set_mac(mac))
            cg.add(cg.RawExpression(f"{ent_var}.update = {u}"))
        if CONF_IDENTIFY_BUTTON in dev:
            await _new_button(dev[CONF_IDENTIFY_BUTTON], hub, ButtonAction.IDENTIFY, mac)
        if CONF_FLASH_BUTTON in dev:
            await _new_button(dev[CONF_FLASH_BUTTON], hub, ButtonAction.FLASH, mac)
        if CONF_SET_TIME_BUTTON in dev:
            await _new_button(dev[CONF_SET_TIME_BUTTON], hub, ButtonAction.SET_TIME, mac)
        cg.add(hub.add_device_entities(ent_var))
