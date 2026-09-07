#!/bin/bash
# End-to-end API driver used for the test report.  Usage: api_test.sh <host> <cmd> [args]
#   status | devices | device MAC | identify MAC | config MAC | settime MAC | flash MAC FWID | log [since] | fw | wait [secs]
H="${1:-xiaomi-esp-flasher.local}"; shift
CMD="${1:-status}"; shift
J() { python3 -m json.tool 2>/dev/null || cat; }
case "$CMD" in
  status)   curl -s "http://$H/api/status" | J ;;
  devices)  curl -s "http://$H/api/devices" | J ;;
  device)   curl -s "http://$H/api/device/$1" | J ;;
  fw)       curl -s "http://$H/api/firmware" | J ;;
  log)      curl -s "http://$H/api/log?since=${1:-0}" | python3 -c "import sys,json,datetime;d=json.load(sys.stdin);[print(datetime.datetime.fromtimestamp(l['t']).strftime('%H:%M:%S') if l['t'] else '--:--:--', l['msg']) for l in d['lines']]" ;;
  identify) curl -s -H "Content-Length: 0" -X POST "http://$H/api/device/$1/identify" | J ;;
  settime)  curl -s -H "Content-Length: 0" -X POST "http://$H/api/device/$1/settime" | J ;;
  reboot)   curl -s -H "Content-Length: 0" -X POST "http://$H/api/device/$1/reboot" | J ;;
  scan)     curl -s -H "Content-Length: 0" -X POST "http://$H/api/scan" | J ;;
  config)   curl -s -X POST -H 'Content-Type: application/json' -d "$2" "http://$H/api/device/$1/config" | J ;;
  flash)    curl -s -X POST -H 'Content-Type: application/json' -d "{\"firmware\":\"$2\",\"confirm\":true}" "http://$H/api/device/$1/flash" | J ;;
  ota)      curl -s "http://$H/api/ota/status" | J ;;
  wait)     # wait until the session is idle/success/error, printing new log lines
            python3 - "$H" "${1:-120}" <<'PY'
import sys, json, time, urllib.request
h, secs = sys.argv[1], int(sys.argv[2])
get = lambda p: json.load(urllib.request.urlopen(f"http://{h}/api{p}", timeout=10))
since = get("/log")["seq"]
for i in range(secs // 2):
    time.sleep(2)
    d = get(f"/log?since={since}")
    for l in d["lines"]:
        print("  ", time.strftime("%H:%M:%S", time.localtime(l["t"])) if l["t"] else "--:--:--", l["msg"])
    since = d["seq"]
    s = get("/status")["session"]
    if i > 1 and s in ("IDLE", "SUCCESS", "ERROR"):
        print("session:", s); break
PY
            ;;
  *) echo "unknown cmd"; exit 1 ;;
esac
