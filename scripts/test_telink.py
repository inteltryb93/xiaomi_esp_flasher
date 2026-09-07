#!/usr/bin/env python3
"""
Host-side protocol prover (bleak).  Mirrors TelinkMiFlasher.html step by step so the ESP32
implementation can be compared against a known-good reference run.

Usage:
  test_telink.py MAC identify            # connect, enumerate, read DIS + custom config
  test_telink.py MAC config-set KEY=VAL  # write one cfg field (e.g. temp_offset=-5) and read back
  test_telink.py MAC settime
  test_telink.py MAC ota FILE.bin        # Telink OTA (custom fw only, needs explicit --yes)
"""
import asyncio, sys, time, struct, datetime
from bleak import BleakClient, BleakScanner
import telink_proto as tp

LOG = open(f"/home/mateusz/xiaomi_esp_flasher/logs/host_{time.strftime('%Y%m%d_%H%M%S')}.log", "w")


def log(msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    LOG.write(line + "\n"); LOG.flush()


class Dev:
    def __init__(self, mac):
        self.mac = mac
        self.client = None
        self.q = asyncio.Queue()
        self.services = []
        self.cfg = None
        self.ota_status = []

    async def connect(self):
        log(f"scanning for {self.mac}")
        d = await BleakScanner.find_device_by_address(self.mac, timeout=30)
        if d is None:
            raise RuntimeError("DEVICE_NOT_FOUND")
        log(f"found {d.address} name={d.name!r}")
        self.client = BleakClient(d, timeout=30)
        await self.client.connect()
        log(f"connected mtu={self.client.mtu_size}")
        for s in self.client.services:
            self.services.append(s.uuid)
            log(f"service {s.uuid}")
            for c in s.characteristics:
                log(f"    char {c.uuid} handle={c.handle} props={','.join(c.properties)}")

    async def read_dis(self):
        out = {}
        if tp.UUID_DIS not in self.services:
            log("no Device Information Service")
            return out
        for name, u in (("model", tp.UUID_DIS_MODEL), ("serial", tp.UUID_DIS_SERIAL),
                        ("firmware", tp.UUID_DIS_FIRMWARE), ("hardware", tp.UUID_DIS_HARDWARE),
                        ("software", tp.UUID_DIS_SOFTWARE), ("manufacturer", tp.UUID_DIS_MANUF)):
            try:
                v = await self.client.read_gatt_char(u)
                out[name] = v.decode("utf-8", "replace")
                log(f"DIS {name}: {out[name]!r}")
            except Exception as e:
                log(f"DIS {name}: n/a ({e})")
        return out

    def _notify(self, _h, data: bytearray):
        data = bytes(data)
        log(f"  <- 1f1f {data.hex()}")
        self.q.put_nowait(data)

    async def custom_cmd(self, payload: bytes, expect=None, timeout=3.0):
        """write to 0x1f1f, wait for notification with matching first byte"""
        while not self.q.empty():
            self.q.get_nowait()
        log(f"  -> 1f1f {payload.hex()}")
        await self.client.write_gatt_char(tp.UUID_CUSTOM_CHAR, payload, response=True)
        exp = payload[0] if expect is None else expect
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                r = await asyncio.wait_for(self.q.get(), timeout - (time.time() - t0))
            except asyncio.TimeoutError:
                break
            if r and r[0] == exp:
                return r
        return None

    async def identify(self):
        dis = await self.read_dis()
        info = {"dis": dis, "services": self.services}
        info["ota_enabled"] = tp.UUID_OTA_SERVICE in self.services
        info["custom"] = tp.UUID_CUSTOM_SERVICE in self.services
        info["mi_stock"] = tp.UUID_MI_MAIN in self.services
        info["bthome"] = tp.UUID_BTHOME in self.services
        log(f"otaEnabled={info['ota_enabled']} customEnabled={info['custom']} miEnabled={info['mi_stock']}")
        if info["custom"]:
            await self.client.start_notify(tp.UUID_CUSTOM_CHAR, self._notify)
            r = await self.custom_cmd(bytes([tp.CMD_ID_DEV_ID]))
            info["dev_id"] = tp.parse_dev_id(r) if r else None
            log(f"DEV_ID: {info['dev_id']}")
            r = await self.custom_cmd(bytes([tp.CMD_ID_CFG]))
            self.cfg = tp.parse_cfg(r) if r else None
            info["cfg"] = self.cfg
            log(f"CFG: {self.cfg}")
            if self.cfg:
                log(f"Hardware Version: {self.cfg['hw_name']} {dis.get('hardware','')}, Software Version: {self.cfg['sw_version']}")
            r = await self.custom_cmd(bytes([tp.CMD_ID_COMFORT]))
            info["comfort"] = tp.parse_comfort(r) if r else None
            log(f"COMFORT: {info['comfort']}")
            r = await self.custom_cmd(bytes([tp.CMD_ID_TRG]))
            info["trg"] = tp.parse_trg(r, self.cfg["ver"] if self.cfg else 0) if r else None
            log(f"TRG: {info['trg']}")
            r = await self.custom_cmd(bytes([tp.CMD_ID_DNAME]))
            info["name"] = r[1:].decode("utf-8", "replace") if r else None
            log(f"DNAME: {info['name']!r}")
            r = await self.custom_cmd(bytes([tp.CMD_ID_UTC_TIME]))
            info["time"] = tp.parse_time(r) if r else None
            if info["time"]:
                log(f"TIME: {info['time']} = {datetime.datetime.utcfromtimestamp(info['time']['utc'])} (device local)")
            r = await self.custom_cmd(bytes([tp.CMD_ID_DEV_MAC]))
            log(f"MAC blk: {r.hex() if r else None}")
            r = await self.custom_cmd(bytes([tp.CMD_ID_SEN_ID]))
            log(f"SENSOR ID: {r.hex() if r else None}")
            r = await self.custom_cmd(bytes([tp.CMD_ID_GDEVS]))
            log(f"I2C DEVS: {r.hex() if r else None}")
            r = await self.custom_cmd(bytes([tp.CMD_ID_MEASURE, 0xff]), expect=tp.CMD_ID_MEASURE, timeout=15)
            info["measure"] = tp.parse_measure(r) if r else None
            log(f"MEASURE: {info['measure']}")
            await self.custom_cmd(bytes([tp.CMD_ID_MEASURE, 0x00]), timeout=1)
        return info

    async def set_cfg_field(self, key, val):
        assert self.cfg, "read cfg first"
        new = dict(self.cfg)
        new[key] = val
        payload = tp.encode_cfg(new)
        r = await self.custom_cmd(payload)
        back = tp.parse_cfg(r) if r else None
        log(f"after write: {back}")
        return back

    async def ota(self, path, yes=False):
        data = open(path, "rb").read()
        res, inf = tp.test_ota_firmware(data, big_ota=self.cfg["big_ota"] if self.cfg else False)
        log(f"image check: {res} {inf}")
        if res != "ok":
            raise RuntimeError(res)
        if not yes:
            log("dry run (pass --yes to flash)")
            return
        if not tp.UUID_OTA_SERVICE in self.services:
            raise RuntimeError("no OTA service")
        data = tp.pad_firmware(data)
        n = len(data) // 16
        log(f"OTA start: {len(data)} bytes, {n} blocks")
        w = lambda b: self.client.write_gatt_char(tp.UUID_OTA_CHAR, b, response=False)
        await asyncio.sleep(0.5)
        await w(struct.pack("<H", tp.CMD_OTA_FW_VERSION))
        await w(struct.pack("<H", tp.CMD_OTA_START))
        await asyncio.sleep(0.3)
        t0 = time.time()
        for i in range(n):
            await w(tp.ota_block(i, data[i * 16:(i + 1) * 16]))
            if (i + 1) % 8 == 0:
                st = await self.client.read_gatt_char(tp.UUID_OTA_CHAR)
                if st and st[0] != 0:
                    raise RuntimeError(f"OTA error after block {i}: {tp.OTA_ERRORS[st[0]] if st[0] < len(tp.OTA_ERRORS) else st[0]}")
            if i % 256 == 0:
                log(f"block {i}/{n} {100 * i // n}% {time.time() - t0:.1f}s")
        await w(tp.ota_end_packet(n))
        log(f"OTA end sent after {time.time() - t0:.1f}s")


async def main():
    mac, cmd = sys.argv[1], sys.argv[2]
    dev = Dev(mac)
    await dev.connect()
    try:
        info = await dev.identify()
        if cmd == "config-set":
            k, v = sys.argv[3].split("=")
            await dev.set_cfg_field(k, int(v))
        elif cmd == "settime":
            now = int(time.time()) + (time.localtime().tm_gmtoff)
            r = await dev.custom_cmd(tp.encode_time(now))
            log(f"settime -> {tp.parse_time(r) if r else None}")
        elif cmd == "ota":
            await dev.ota(sys.argv[3], yes="--yes" in sys.argv)
    finally:
        await dev.client.disconnect()
        log("disconnected")

asyncio.run(main())
