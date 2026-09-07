#!/usr/bin/env python3
"""Headless-Chromium test of the browser-side firmware path: Firmware page -> refresh manifest from GitHub ->
download ATC image -> stored on the ESP32.  Usage: gui_download_test.py <esp-ip> [image-name]"""
import json, subprocess, sys, time, urllib.request, os
import websocket
host = sys.argv[1]; name = sys.argv[2] if len(sys.argv) > 2 else "ATC_v59.bin"
chrome = [c for c in ("/usr/bin/google-chrome", "/usr/bin/chromium-browser", "/usr/bin/chromium") if os.path.exists(c)][0]
proc = subprocess.Popen([chrome, "--headless=new", "--disable-gpu", "--no-sandbox", "--remote-debugging-port=9224", "--remote-allow-origins=*",
                         "--window-size=1300,1000", "--user-data-dir=/tmp/xf-chrome-profile2", "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(40):
        try: tabs = json.load(urllib.request.urlopen("http://127.0.0.1:9224/json")); break
        except Exception: time.sleep(0.25)
    ws = websocket.create_connection([t for t in tabs if t["type"] == "page"][0]["webSocketDebuggerUrl"])
    mid = 0
    def cmd(method, **params):
        global mid
        mid += 1; ws.send(json.dumps({"id": mid, "method": method, "params": params}))
        while True:
            m = json.loads(ws.recv())
            if m.get("id") == mid: return m.get("result", {})
    def js(expr):
        return cmd("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=True).get("result", {}).get("value")
    cmd("Page.enable"); cmd("Runtime.enable")
    cmd("Page.navigate", url=f"http://{host}/#/firmware")
    for _ in range(60):
        time.sleep(0.5)
        if js("!!document.querySelector('#fw-refresh-manifest')"): break
    js("document.querySelector('#fw-refresh-manifest').click()")
    for _ in range(30):
        time.sleep(1)
        if js("document.querySelectorAll('#fw-remote-list button[data-dl]').length > 0"): break
    print("manifest:", js("document.querySelector('#fw-manifest-info').textContent"))
    idx = js(f"Array.from(document.querySelectorAll('#fw-remote-list tr')).findIndex(tr => tr.textContent.includes('{name}')) - 1")
    print("row for", name, "->", idx)
    js(f"document.querySelector('#fw-remote-list button[data-dl=\"{idx}\"]').click()")
    for _ in range(90):
        time.sleep(1)
        if js(f"document.querySelector('#log').textContent.includes('Stored {name}') || document.querySelector('#toast').textContent.includes('failed')"): break
    print("log tail:", js("document.querySelector('#log').textContent.split('\\n').slice(-4).join(' | ')"))
    print("toast:", js("document.querySelector('#toast').textContent"))
finally:
    proc.terminate()
