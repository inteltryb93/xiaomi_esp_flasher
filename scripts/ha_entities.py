#!/usr/bin/env python3
"""List entities and current states through the ESPHome native API (what Home Assistant sees)."""
import asyncio, sys
from aioesphomeapi import APIClient

async def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "xiaomi-esp-flasher.local"
    c = APIClient(host, 6053, None)
    await c.connect(login=True)
    info = await c.device_info()
    print("device:", info.name, info.esphome_version, info.model)
    ents, _ = await c.list_entities_services()
    byid = {e.key: e for e in ents}
    for e in ents:
        print(f"{type(e).__name__:28s} {e.object_id:45s} {e.name}")
    states = {}
    def cb(s): states[s.key] = s
    c.subscribe_states(cb)
    await asyncio.sleep(3)
    print("---- states ----")
    for k, s in states.items():
        e = byid.get(k)
        name = e.name if e else k
        val = getattr(s, "state", None)
        extra = ""
        if hasattr(s, "current_version"):
            extra = f" current={s.current_version} latest={s.latest_version} has_progress={s.has_progress} progress={s.progress} in_progress={s.in_progress} summary={s.release_summary!r}"
        print(f"{name:45s} {val}{extra}")
    await c.disconnect()

asyncio.run(main())
