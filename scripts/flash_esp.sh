#!/bin/bash
# Flash the ESP32. Usage: flash_esp.sh [yaml] [serial|ota|<port-or-ip>]
# Default: OTA over WiFi when the device answers on mDNS, else USB serial.
set -e
cd "$(dirname "$0")/.."
YAML="${1:-yaml/xiaomi_esp_flasher.yaml}"
MODE="${2:-auto}"
NAME=$(grep -E '^\s+name:' "$YAML" | head -1 | awk '{print $2}')
[ "$NAME" = '${name}' ] && NAME=$(grep -E '^\s+name:' "$YAML" | head -1 | awk '{print $2}') && NAME=$(sed -n 's/^  name: //p' "$YAML" | head -1)
if [ "$MODE" = "serial" ]; then
  PORT=$(scripts/wait_port.sh 60) || { echo "serial port busy"; exit 1; }
  exec .venv/bin/esphome run --no-logs --device "$PORT" "$YAML"
elif [ "$MODE" = "ota" ] || [ "$MODE" = "auto" ]; then
  IP=$(avahi-resolve -4 -n "${NAME}.local" 2>/dev/null | awk '{print $2}')
  if [ -n "$IP" ]; then exec .venv/bin/esphome run --no-logs --device "$IP" "$YAML"; fi
  [ "$MODE" = "ota" ] && { echo "device not reachable"; exit 1; }
  PORT=$(scripts/wait_port.sh 60) || { echo "serial port busy and device not on WiFi"; exit 1; }
  exec .venv/bin/esphome run --no-logs --device "$PORT" "$YAML"
else
  exec .venv/bin/esphome run --no-logs --device "$MODE" "$YAML"
fi
