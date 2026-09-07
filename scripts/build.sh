#!/bin/bash
# Compile the ESPHome project (uses the venv with the editable /home/mateusz/esphome checkout).
set -e
cd "$(dirname "$0")/.."
exec .venv/bin/esphome compile "${1:-yaml/xiaomi_esp_flasher.yaml}"
