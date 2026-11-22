# Flashing this board

## TL;DR

- **First time / after a partition change:** USB, bootloader entered **by hand**:
  1. hold **GIO0**
  2. press and release **REST**
  3. release **GIO0**
  4. run `tools\build_upload.cmd -Usb`, or upload from Arduino IDE
- **Every other time:** OTA over Wi-Fi with `tools\build_upload.cmd`, or
  `curl --data-binary @esp32_cam_web.ino.bin http://esp32-cam.local/update`.

## Why the automatic upload doesn't work

The listing promises a "CH340X automatic download circuit". In practice esptool's reset sequence
toggles EN, but **GPIO0 never goes low**. We tried esptool's default reset and several custom
`custom_reset_sequence` timings, and every attempt failed with:

```
Wrong boot mode detected (0x13)! The chip needs to be in download mode.
```

Most likely only one CH340X modem line is wired to the reset circuit. The first upload sometimes
works by luck. Don't rely on it.

On top of that, once your firmware has started the camera, the OV5640 keeps driving its data lines
through an EN reset:

- **GPIO2 (D0) high** → download mode refused: `Wrong boot mode detected (0xb)`
- **GPIO12 (D3) high** → the ROM selects **1.8 V** for the flash → `flash read err, 1000` boot loop
  (`boot:0x33`), and esptool fails with *Unable to verify flash chip connection*

Unplugging USB (a full power cycle) resets the camera, and its outputs come up disabled. So:

- if the board is stuck in a `flash read err` loop: **unplug USB, hold GIO0, plug USB back in,
  release GIO0**. You're in the bootloader with a clean camera.
- the firmware also has a serial command, **`flash`**, that stops the camera and holds its RESET
  low before you reset into the bootloader.

## Recommended: burn the flash-voltage eFuse (one-time, irreversible)

This board has a **3.3 V flash chip**, and GPIO12 is used by the camera. Espressif's recommended fix
is to make the chip ignore GPIO12 and always power the flash at 3.3 V:

```
espefuse --port COMx --before no-reset set-flash-voltage 3.3V
```

(Run it with the board in the bootloader. The tool asks you to type `BURN`.)

Effects:
- burns `XPD_SDIO_FORCE`, `XPD_SDIO_REG` and `XPD_SDIO_TIEH`
- VDD_SDIO is always 3.3 V, which is what this board needs anyway. The flash and the ESP-PSRAM64H are both 3.3 V parts.
- GPIO12 is no longer a strapping pin, so the `flash read err` boot loop can't happen
- camera, SD card and everything else are unaffected. **Irreversible**, but the only thing you lose
  is the ability to use a 1.8 V flash chip with this ESP32.

After burning it, the boot log shows `boot:0x13` even with the camera running.

## Upload settings

| Setting | Value |
|---------|-------|
| Board | ESP32 Dev Module (`esp32:esp32:esp32`) |
| PSRAM | Enabled |
| Partition scheme | Minimal SPIFFS (1.9MB APP with OTA): `min_spiffs` |
| Upload speed | 460800 is reliable. 921600 sometimes fails mid-way with *Failed to read target memory* |
| Flash mode | keep the default (DIO in the bootloader header) |

Manual esptool command, with the board already in the bootloader:

```
esptool --chip esp32 --port COMx --baud 460800 --before no-reset --after no-reset write-flash -z \
  0x1000  build/esp32_cam_web.ino.bootloader.bin \
  0x8000  build/esp32_cam_web.ino.partitions.bin \
  0xe000  build/boot_app0.bin \
  0x10000 build/esp32_cam_web.ino.bin
```

Then press **REST** to run it. `boot_app0.bin` is in the ESP32 core
(`tools/partitions/boot_app0.bin`), and `arduino-cli compile --build-path build` also puts it there.

## OTA

`POST /update` with the raw application binary as the body. The firmware stops the camera, writes
the inactive OTA slot, and reboots into it.

```
curl --data-binary @build/esp32_cam_web.ino.bin http://esp32-cam.local/update
# -> OK, rebooting
```

There's no authentication. Only use this on a trusted network, or add a token check in `updateHandler()`.

## Windows notes

- `.ps1` scripts are blocked by the default execution policy. `tools\build_upload.cmd` runs
  `build_upload.ps1` with `-ExecutionPolicy Bypass` for that one invocation only.
- Resolving `esp32-cam.local` costs ~1 s per new connection on Windows. Browsers cache it. For
  scripts, use the IP (`-HostName 192.168.x.y`).
