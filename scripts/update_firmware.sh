#!/bin/bash
# Refresh firmware/manifest.json and the bundled ATC image from the pvvx repository.
set -e
cd "$(dirname "$0")/.."
if [ -d reference/ATC_MiThermometer/.git ]; then
  git -C reference/ATC_MiThermometer pull -q --ff-only || true
else
  git clone -q --depth 1 https://github.com/pvvx/ATC_MiThermometer.git reference/ATC_MiThermometer
fi
cp reference/ATC_MiThermometer/firmware.json firmware/manifest.json
ver=$(python3 -c "import json;print(json.load(open('firmware/manifest.json'))['version'])")
files=$(python3 -c "import json;m=json.load(open('firmware/manifest.json'));print(' '.join(sorted({f.split('/')[-1] for f in m['custom'] if f.endswith('.bin') and 'ATC_' in f})))")
rm -f firmware/ATC_v*.bin
for f in $files; do cp "reference/ATC_MiThermometer/bin/$f" firmware/; done
echo "manifest version $ver (0x$(printf %x $ver)); bundled: $files"
