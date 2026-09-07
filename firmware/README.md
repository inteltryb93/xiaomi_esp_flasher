# Bundled firmware

Files in this directory are compiled into the ESP32 application and served as the **offline** firmware source
(`LocalFirmwareProvider`).  Nothing here is hand-written:

* `manifest.json` – verbatim copy of `firmware.json` from https://github.com/pvvx/ATC_MiThermometer
  (arrays indexed by pvvx hardware id, see `bin/README.md` there).  The `version` field (decimal of the BCD
  version byte, 89 = 0x59 = "5.9") is what the ESP32 compares against the installed firmware.
* `*.bin` – optional: Telink OTA images to compile into the ESP32 image (`bundle_firmware: true`). By default no
  image is bundled: the browser downloads the image from GitHub and uploads it into the `fwstore` partition.


Refresh with `scripts/update_firmware.sh` (pulls the pvvx repo and copies the current manifest + images).
Additional images can be uploaded at runtime through the web GUI (stored in the `fwstore` flash partition)
or downloaded from GitHub with "Check online" when `remote_manifest` is configured.
