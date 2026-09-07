#!/bin/bash
# Wait until the ESP32 USB-Serial/JTAG port can be opened, print its path.
for i in $(seq 1 ${1:-120}); do
  PORT=$(ls /dev/serial/by-id/*Espressif* 2>/dev/null | head -1)
  if [ -n "$PORT" ] && python3 -c "import os,sys; fd=os.open(sys.argv[1], os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK); os.close(fd)" "$PORT" 2>/dev/null; then
    echo "$PORT"; exit 0
  fi
  sleep 5
done
exit 1
