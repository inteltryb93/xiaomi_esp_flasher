# Bundled firmware

Files in this directory are compiled into the ESP32 application and served as the **offline** firmware source
(`LocalFirmwareProvider`).  Nothing here is hand-written:

* `manifest.json` – verbatim copy of `firmware.json` from https://github.com/pvvx/ATC_MiThermometer
  (arrays indexed by pvvx hardware id, see `bin/README.md` there).  The `version` field (decimal of the BCD
  version byte, 89 = 0x59 = "5.9") is what the ESP32 compares against the installed firmware.
* `*.bin` – the Telink OTA images referenced by the manifest that should be available offline.
  Only images whose file name appears in the manifest are offered.  By default only `ATC_v59.bin`
  (all LYWSD03MMC hardware revisions) is bundled to keep the ESP32 image small.

Refresh with `scripts/update_firmware.sh` (pulls the pvvx repo and copies the current manifest + images).
Additional images can be uploaded at runtime through the web GUI (stored in the `fwstore` flash partition)
or downloaded from GitHub with "Check online" when `remote_manifest` is configured.
