#!/usr/bin/env python3
"""Headless-Chromium GUI test (Chrome DevTools Protocol, no puppeteer needed).
Loads the ESP32 GUI (bootstrap -> CDN assets), opens the device page, the Configure tab, presses Connect,
waits for the pvvx 'Get Config' menu, sends 55 and checks that CustomConfig() rendered; takes screenshots.
Usage: gui_test.py <esp-ip> <mac> [outdir]"""
import json, subprocess, sys, time, urllib.request, base64, os
import websocket

host, mac = sys.argv[1], sys.argv[2].upper()
out = sys.argv[3] if len(sys.argv) > 3 else "logs"
chrome = None
for c in ("/usr/bin/google-chrome", "/usr/bin/chromium-browser", "/usr/bin/chromium"):
    if os.path.exists(c): chrome = c; break
proc = subprocess.Popen([chrome, "--headless=new", "--disable-gpu", "--no-sandbox", "--remote-debugging-port=9223", "--remote-allow-origins=*",
                         "--window-size=1300,1000", "--user-data-dir=/tmp/xf-chrome-profile", "about:blank"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(40):
        try:
            tabs = json.load(urllib.request.urlopen("http://127.0.0.1:9223/json")); break
        except Exception: time.sleep(0.25)
    ws = websocket.create_connection([t for t in tabs if t["type"] == "page"][0]["webSocketDebuggerUrl"])
    mid = 0
    def cmd(method, **params):
        global mid
        mid += 1
        ws.send(json.dumps({"id": mid, "method": method, "params": params}))
        while True:
            m = json.loads(ws.recv())
            if m.get("id") == mid: return m.get("result", {})
    def js(expr):
        r = cmd("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=True)
        return r.get("result", {}).get("value")
    def shot(name):
        d = cmd("Page.captureScreenshot", format="png")
        open(f"{out}/{name}.png", "wb").write(base64.b64decode(d["data"]))
        print("screenshot", f"{out}/{name}.png")
    cmd("Page.enable"); cmd("Runtime.enable")
    cmd("Page.navigate", url=f"http://{host}/#/")
    for _ in range(60):
        time.sleep(0.5)
        if js("!!document.querySelector('#dev-rows tr')"): break
    print("devices rows:", js("document.querySelectorAll('#dev-rows tr').length"), "assets:", js("document.scripts.length"))
    shot("gui_devices")
    js(f"location.hash='#/device/{mac}'")
    time.sleep(2)
    js("document.querySelector('#d-tabs button[data-tab=config]').click()")
    time.sleep(0.5)
    shot("gui_config_before")
    js("document.querySelector('#cfg-connect').click()")
    for _ in range(90):
        time.sleep(1)
        if js("!!document.querySelector('#custcfg button')"): break
    print("status:", js("document.querySelector('#percent').textContent"))
    js("sendCustomSetting('55')")
    for _ in range(20):
        time.sleep(1)
        if js("!!document.querySelector('#cfg_adv_int')"): break
    print("CustomConfig rendered:", js("!!document.querySelector('#cfg_adv_int')"),
          "| version:", js("cfg.ver ? (cfg.ver>>4)+'.'+(cfg.ver&15) : null"),
          "| adv int:", js("document.querySelector('#cfg_adv_int') && document.querySelector('#cfg_adv_int').value"),
          "| smiley:", js("document.querySelector('#cfg_smiley') && document.querySelector('#cfg_smiley').value"),
          "| comfort:", js("document.querySelector('#cmf_tmp_hi') && document.querySelector('#cmf_tmp_hi').value"),
          "| name:", js("document.querySelector('#dev_name') && document.querySelector('#dev_name').value"))
    js("sendCustomSetting('20'); sendCustomSetting('44'); sendCustomSetting('01')")
    time.sleep(4)
    print("after 20/44/01:", "comfort:", js("document.querySelector('#cmf_tmp_hi').value"), "trg:", js("document.querySelector('#trg_tmp_hst').value"), "name:", js("document.querySelector('#dev_name').value"))
    js("window.scrollTo(0,0)")
    shot("gui_config_connected")
    js("window.scrollTo(0,900)"); time.sleep(0.5); shot("gui_config_connected_2")
    print("log tail:", js("document.querySelector('#log').textContent.split('\\n').slice(-6).join(' | ')"))
    js("document.querySelector('#cfg-disconnect').click()")
    time.sleep(3)
    print("after disconnect:", js("document.querySelector('#percent').textContent"))
finally:
    proc.terminate()
