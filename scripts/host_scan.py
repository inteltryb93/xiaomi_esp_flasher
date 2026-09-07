#!/usr/bin/env python3
"""Host-side BLE scan helper (bleak). Lists Xiaomi/Telink thermometers and dumps raw advertising."""
import asyncio, sys, time
from bleak import BleakScanner

TARGET = sys.argv[1].upper() if len(sys.argv) > 1 else None
DUR = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
seen = {}

def cb(dev, adv):
    if TARGET and dev.address.upper() != TARGET:
        return
    key = dev.address
    first = key not in seen
    seen[key] = time.time()
    if first or TARGET:
        print(f"{time.strftime('%H:%M:%S')} {dev.address} rssi={adv.rssi} name={adv.local_name!r}")
        for u, d in adv.service_data.items():
            print(f"    svc {u}: {d.hex()}")
        for m, d in adv.manufacturer_data.items():
            print(f"    mfg {m:04x}: {d.hex()}")
        if adv.service_uuids:
            print(f"    uuids {adv.service_uuids}")

async def main():
    async with BleakScanner(cb, scanning_mode="active"):
        await asyncio.sleep(DUR)
    print(f"done, {len(seen)} devices")

asyncio.run(main())
