#!/usr/bin/env python3
"""Press "Update all" in the GUI (headless Chromium, DevTools protocol), confirm the dialog and follow the
ESP32 log until the update queue has finished.  Usage: gui_update_all.py <esp-ip> [max-minutes]"""
import json, subprocess, sys, time, urllib.request, os
import websocket
host = sys.argv[1]; maxmin = int(sys.argv[2]) if len(sys.argv) > 2 else 25
# optional: which toolbar button to press and which dialog button confirms it (default: Update all)
btn_id = sys.argv[3] if len(sys.argv) > 3 else "btn-update-all"
confirm = sys.argv[4] if len(sys.argv) > 4 else "Start updates"
chrome = [c for c in ("/usr/bin/google-chrome", "/usr/bin/chromium-browser", "/usr/bin/chromium") if os.path.exists(c)][0]
proc = subprocess.Popen([chrome, "--headless=new", "--disable-gpu", "--no-sandbox", "--remote-debugging-port=9225", "--remote-allow-origins=*",
                         "--window-size=1300,1000", "--user-data-dir=/tmp/xf-chrome-profile3", "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
get = lambda p: json.load(urllib.request.urlopen(f"http://{host}/api{p}", timeout=15))
try:
    for _ in range(40):
        try: tabs = json.load(urllib.request.urlopen("http://127.0.0.1:9225/json")); break
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
    cmd("Page.navigate", url=f"http://{host}/#/")
    for _ in range(60):
        time.sleep(0.5)
        if js(f"!!document.querySelector('#dev-rows tr') && !!document.querySelector('#{btn_id}')"): break
    since = get("/log")["seq"]
    js(f"document.querySelector('#{btn_id}').click()")
    time.sleep(1)
    print("dialog:", js("(document.querySelector('#modal-box') || {}).innerText") )
    js(f"Array.from(document.querySelectorAll('#modal-box .modal-btns button')).find(b => b.textContent === '{confirm}').click()")
    time.sleep(2)
    print("toast:", js("document.querySelector('#toast').textContent"))
    t0 = time.time(); idle = 0; last_badge = None; last_gui_log = None; opened = None; progress_samples = []
    while time.time() - t0 < maxmin * 60:
        time.sleep(3)
        # --- what the GUI itself shows (status badge + its own OTA Log panel fed by SSE) ---
        badge = js("document.querySelector('#session-badge').textContent")
        gui_log = js("document.querySelector('#log').textContent.trim().split('\\n').slice(-1)[0]")
        if badge != last_badge:
            print("   [GUI status]", badge, flush=True); last_badge = badge
        if gui_log != last_gui_log:
            print("   [GUI log]", gui_log, flush=True); last_gui_log = gui_log
        # open the device page of the device being flashed once, to sample the progress bar
        st = get("/status")
        if st.get("job") == "flash" and st.get("session_device") and opened != st["session_device"]:
            opened = st["session_device"]
            js(f"location.hash='#/device/{opened}'"); time.sleep(2)
        if opened and js("!!document.querySelector('#progress-box')"):
            pb = js("document.querySelector('#progress-box .pstage').textContent + ' | ' + document.querySelector('#progress-box .pinfo').textContent + ' | bar ' + document.querySelector('#progress-box .pbar i').style.width")
            if pb and (not progress_samples or progress_samples[-1] != pb):
                progress_samples.append(pb); print("   [GUI progress]", pb, flush=True)
        # --- ESP32 log ring (ground truth) ---
        d = get(f"/log?since={since}")
        for l in d["lines"]:
            print("  ", time.strftime("%H:%M:%S", time.localtime(l["t"])) if l["t"] else "--:--:--", l["msg"], flush=True)
        since = d["seq"]
        if st["session"] in ("IDLE", "SUCCESS", "ERROR") and st["queue"] == 0:
            idle += 1
            if idle >= 4: break
        else:
            idle = 0
    st = get("/status")
    print("final:", {k: st.get(k) for k in ("session", "queue", "last_ota_result", "last_error", "uptime_s", "reset_reason", "min_free_heap")})
finally:
    proc.terminate()
