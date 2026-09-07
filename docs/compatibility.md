# Hardware / firmware compatibility model

Implemented in `components/xiaomi_esp_flasher/compatibility.{h,cpp}` — the **only** place where
compatibility rules live. GUI, OTA state machine and the Home Assistant update entity all call
`is_firmware_compatible()` / `check_compatibility()`.

## 1. Hardware identity

A device is described by `HardwareInfo`:

| field | source |
|---|---|
| `model` (`LYWSD03MMC`, `MHO-C401`, `CGG1`, …) | DIS `0x2A24` model string, BLE name, or pvvx `hw_id` table |
| `hw_id` (pvvx numeric id, `bin/README.md`) | custom fw: `CMD_ID_CFG` byte 11 / `CMD_ID_DEV_ID`; stock fw: derived from name + DIS `0x2A27` |
| `hw_string` (`B1.4` … `B2.0`, `B1.1`) | DIS `0x2A27` (both stock and custom firmware expose it) |
| `firmware_kind` | `CUSTOM_PVVX` (service 0x1F10 present), `STOCK_XIAOMI` (service 0xFE95 + ebe0ccb0), `ATC1441` (0x1F10 but no 0x181A), `UNKNOWN` |
| `fw_version` | custom: BCD `ver` byte → `"4.7"`; stock: DIS `0x2A26` (`1.0.0_0130`) |
| `i2c_sensor`, `i2c_lcd` | custom fw `CMD_ID_GDEVS` (cross-check of the HW class) |

### 1.1 LYWSD03MMC hw_id table (pvvx `bin/README.md`, `app.c set_hw_version()`)

| hw_id | DIS string | LCD | sensor | custom image |
|---|---|---|---|---|
| 0 | B1.4 | I2C 0x3C | SHTC3 (0x70) | ATC |
| 10 | B1.5 | UART | SHTC3 | ATC |
| 4 | B1.6 | UART | SHT4x (0x44) | ATC |
| 5 | B1.7 / B2.0 | I2C 0x3C | SHT4x | ATC |
| 3 | B1.9 | I2C 0x3E | SHT4x | ATC |
| 14 | B1.1 (PCB mark B1.6/B1.5, 2025) | SPI | SHT4x/SHTC3 clones | ATC |

All LYWSD03MMC revisions share one custom image (`ATC_vXX.bin`) which autodetects the display/sensor at
boot (`set_hw_version()`), so *custom → newer custom* is allowed for every known id above.
The *original* images differ per revision: `Original_OTA_Xiaomi_LYWSD03MMC_v1.0.0_0130.bin` is listed for
ids 0,3,4,5,10 (README: "HW: B1.4..B2.0"), `v1.0.0_0109` only B1.4, and id 14 gets `v2.1.1_0159_B1.6.bin`
which afterwards **requires Mi-Home registration** to be flashed again (issue #602). Going back to original
on id 14 is therefore blocked by default.

## 2. Rules (`check_compatibility(hw, fw)`)

1. `fw.hw_ids` must contain `hw.hw_id`. Images are tagged with the set of ids from `firmware.json`
   (`custom[]`, `original[]`, `signed[]` indexes). Unknown/`"?"` entries never match.
2. `hw.hw_id` must be known (not `HW_UNKNOWN`). If the device is stock Xiaomi and the DIS string is not in the
   LYWSD03MMC table (or the model name is unknown) → `HW_UNKNOWN` → blocked
   ("Unknown / unsupported hardware revision. Firmware update blocked for safety.").
3. Custom-firmware devices: if `CMD_ID_GDEVS` is available, the LCD/sensor pair must be consistent with a
   LYWSD03MMC class (any of the rows above). Inconsistent → blocked. A mismatch between the pvvx `hw_id`
   and the DIS string (test device: hw_id 4 "B1.6" but DIS "B1.7", I2C says 0x3C+SHT4x = B1.7 class) is
   reported as a warning, not a block, because both ids map to the same image and the image re-detects the HW.
4. Image must pass `validate_telink_image()` (header, size pointer, CRC32) and fit the device's OTA limit
   (128 KiB, or 208 KiB when `big_ota` is supported: custom `ver > 0x45`).
5. Stock firmware `2.1.1_0159` / `MJWSD06MMC 0009` require Mi cloud token → blocked unless token+bindkey provided.
6. `original` images are only offered when `fw.kind == ORIGINAL` and the user explicitly selects "back to original";
   never as an automatic update target.
7. Version comparison for "update available" is numeric: custom `ver` byte (`0x47 < 0x59`), never string compare.
   Stock firmware never shows "update available" for the custom image automatically; it is offered as a
   *conversion*, flagged `requires_activation`.

Any rule failing yields a `CompatResult{ok=false, code, message}` with codes
`DEVICE_UNSUPPORTED`, `HW_UNKNOWN`, `FW_UNKNOWN`, `INCOMPATIBLE_FIRMWARE`, `NOT_ENOUGH_STORAGE`,
`ACTIVATION_REQUIRED`.
