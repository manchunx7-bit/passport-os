# Safe Flashing Guide

How to pick the right image, keep your data, and flash a FoloToy AI Passport (ESP32-C3, 8 MB Flash) on Windows, macOS, Linux, or WSL. You do not need ESP-IDF to install a prebuilt image.

> **Rule of thumb: the full image goes to `0x0`; an application image goes only to `0x10000`.**
> Writing an application image at `0x0` overwrites the second-stage bootloader and partition table, and the device will reboot in a loop.

## Supported hardware

- FoloToy AI Passport
- ESP32-C3 with 8 MB Flash
- Display, audio codec, and buttons matching the released `partitions.csv`

Similar-looking boards with a different flash size, display, pins, audio chip, or partition layout cannot use these images.

## Choose the right image

| Situation | Image | Address | Data impact |
| --- | --- | --- | --- |
| First installation, full reset, or changed partition layout | `FoloToy-AI-Passport-vX.Y.Z-full.bin` | `0x0` | Overwrites everything in range; provisioning is required afterwards |
| Existing Passport OS, firmware update only | `passport-os.bin` (published as `*-app.bin`) | `0x10000` | Keeps Wi-Fi, portrait, cards, reminders, lists, and settings |

Partition layout (`partitions.csv`):

| Region | Offset | Size | Contents |
| --- | --- | --- | --- |
| bootloader | `0x0` | — | Second-stage bootloader |
| partition table | `0x8000` | `0x1000` | Partition definitions |
| NVS | `0x9000` | `0x6000` | Wi-Fi, settings, app data |
| phy_init | `0xF000` | `0x1000` | RF calibration |
| factory app | `0x10000` | `0x400000` | Application firmware |
| cardid | `0x410000` | — | Device identity |
| storage | `0x420000` | — | Portraits, cards, files |

Verify the device uses this layout before an application-only update. Devices from older releases or with custom partition tables need a full image.

## Preparation

1. Install Python 3.11 or newer.
2. Install esptool:

```bash
python3 -m pip install --user esptool
```

On Windows use `py -m pip install esptool` and replace `python3` with `py`.

3. Connect the device with a USB data cable and find the serial port:

| OS | Typical port | How to list |
| --- | --- | --- |
| Windows | `COM3`, `COM5` | Device Manager → Ports |
| macOS | `/dev/cu.usbmodem101` | `ls /dev/cu.usbmodem*` |
| Linux | `/dev/ttyUSB0`, `/dev/ttyACM0` | `ls /dev/ttyUSB* /dev/ttyACM*` |

On Linux, if access is denied, add your user to `dialout` and log in again:

```bash
sudo usermod -aG dialout "$USER"
```

4. Optionally verify the downloaded file:

```bash
# macOS / Linux
shasum -a 256 FoloToy-AI-Passport-vX.Y.Z-full.bin
# Windows PowerShell
Get-FileHash .\FoloToy-AI-Passport-vX.Y.Z-full.bin -Algorithm SHA256
```

## Normal update: application partition only (keeps data)

This is the recommended way to update an existing device. The example uses `build-public/passport-os.bin` from a release build.

### With ESP-IDF (recommended)

From a terminal with ESP-IDF activated, `app-flash` reads the correct offset from `flash_args`:

```bash
idf.py -B build-public -p /dev/cu.usbmodem101 app-flash
```

On Windows: `idf.py -B build-public -p COM3 app-flash`.

### With esptool directly

```bash
# macOS / Linux
python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodem101 --baud 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x10000 build-public/passport-os.bin

# Windows PowerShell
py -m esptool --chip esp32c3 --port COM3 --baud 460800 write-flash `
  --flash-mode dio --flash-size 8MB --flash-freq 80m `
  0x10000 .\passport-os.bin
```

> An application image belongs at `0x10000`. At `0x0` the ROM treats it as the second-stage bootloader and prints
> `rst:0x7 (TG0WDT_SYS_RST)` and `Invalid image block, can't boot` in a reboot loop. A multi-megabyte write from
> `0x0` also overwrites the partition table and NVS.

## First installation or full reset: full image

The full image contains the bootloader, partition table, and application, and is written at `0x0`. It overwrites configuration in range and requires provisioning afterwards. To guarantee a clean start, erase the whole flash first:

```powershell
# Windows PowerShell
py -m esptool --chip esp32c3 --port COM3 --baud 460800 erase-flash
py -m esptool --chip esp32c3 --port COM3 --baud 460800 write-flash `
  --flash-mode dio --flash-size 8MB --flash-freq 80m `
  0x0 .\FoloToy-AI-Passport-vX.Y.Z-full.bin
```

```bash
# macOS / Linux
python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodem101 --baud 460800 erase-flash
python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodem101 --baud 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 FoloToy-AI-Passport-vX.Y.Z-full.bin
```

Writing the full image without erasing keeps existing NVS configuration. Use `erase-flash` only when a clean first-run experience is required.

**Do not run `erase-flash` or write a full image over an existing device without a backup and explicit approval.**

## After flashing

1. Keep the USB cable connected until writing finishes. The device resets automatically.
2. Watch the serial log (`Ctrl+]` to exit):

```bash
idf.py -B build-public -p /dev/cu.usbmodem101 monitor
```

A normal boot includes:

- `boot: Loaded app from partition at offset 0x10000`
- `App version: ...`
- `Passport OS Boot Complete`

3. Type `v` over the serial port; the log reports `heap_ok=1`.
4. After an application-only update, confirm Wi-Fi is still connected and portraits/cards are intact. After a full image, confirm the device starts the `Passport-WiFi-xxxx` setup hotspot.

## Recovering from a wrong offset

If an application image was written at `0x0`, restore the full layout. NVS configuration is lost and the device returns to provisioning; `cardid` and `storage` remain if they were not overwritten.

```bash
python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodem101 \
  --before default_reset --after no_reset erase_region 0x9000 0x7000
python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodem101 \
  --before no_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 build-public/bootloader/bootloader.bin \
  0x8000 build-public/partition_table/partition-table.bin \
  0x10000 build-public/passport-os.bin
```

`0x9000/0x7000` covers both NVS and phy_init; they must be cleared after contamination and are reinitialized by the firmware. Flashing the matching full image is an equivalent recovery.

## Troubleshooting

**The old UI is still shown**: make sure you wrote the newly built `passport-os.bin` at `0x10000`. The application image never updates the bootloader or partition table.

**The device reboots forever after writing at `0x0`**: restore the full layout as described above; do not keep rewriting the application image.

**No serial port**: use a USB cable that supports data; install CP210x/CH34x drivers on Windows; check `dialout` on Linux; on WSL bind the device with `usbipd` or flash from Windows directly.

**Wi-Fi and personal data are gone**: a full image or `erase-flash` clears configuration. Use the application image for routine updates and back up before large changes.

**Flashing was interrupted**: reconnect and repeat the same command. A failed write normally does not brick the device; a full image restores it if needed.
