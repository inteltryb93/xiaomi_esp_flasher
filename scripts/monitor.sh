#!/bin/bash
# Stream ESPHome logs (network first, serial fallback). Usage: monitor.sh [yaml] [seconds]
cd "$(dirname "$0")/.."
YAML="${1:-yaml/xiaomi_esp_flasher.yaml}"
SECS="${2:-0}"
NAME=$(sed -n 's/^  name: //p' "$YAML" | head -1)
IP=$(avahi-resolve -4 -n "${NAME}.local" 2>/dev/null | awk '{print $2}')
DEV="$IP"
[ -z "$DEV" ] && DEV=$(ls /dev/serial/by-id/*Espressif* 2>/dev/null | head -1)
if [ "$SECS" -gt 0 ]; then exec timeout "$SECS" .venv/bin/esphome logs --device "$DEV" "$YAML"; fi
exec .venv/bin/esphome logs --device "$DEV" "$YAML"
