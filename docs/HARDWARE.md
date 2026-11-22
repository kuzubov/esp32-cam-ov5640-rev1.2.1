# Hardware details

Everything here was checked on a real board (silkscreen `OV5640 REV1.2.1`) in November 2026.

## Components

| Part | Marking / value | Notes |
|------|-----------------|-------|
| MCU | ESP32-D0WDQ6, rev v1.1, 40 MHz crystal | read by `esptool flash-id` |
| Flash | BoyaMicro 25Q32, **4 MB** (manufacturer 0x68, device 0x4016) | listings don't mention the size |
| PSRAM | ESP-PSRAM64H, 8 MB | the classic ESP32 maps 4 MB; uses GPIO16/17 |
| USB-UART | CH340X (USB VID:PID 1A86:7523) | shows up as "USB-SERIAL CH340" |
| Charger / power path | IP5306, **I2C version** (answers at 0x75) | LiPo pads `+ −` near USB-C |
| Camera | OV5640 on a 24-pin FPC, module marked `FD5640 500W-V1` | PID 0x5640 |
| Storage | micro-SD (TF) slot under the camera | SPI mode |
| Display | large FPC connector under the camera | for a TFT, not tested |
| Antenna | PCB antenna + IPEX connector | |

Per the listing, with a battery attached the IP5306 turns the board off on a REST double-click and back on with a single click. Not verified.

## Header pins

```
 left header            right header
 21                     GND
 RXD (GPIO3)            3.3
 TXD (GPIO1)            5V
 19   ← SD CS           37  ← TFT (input only)
 17   ← PSRAM!          38  ← TFT (input only)
 16   ← PSRAM!          32  ← TFT
 4    ← SD SCK / I2C    25  ← LED / TFT
 5V                     EN
 3.3                    13  ← SD MISO / I2C
 GND                    0   ← GIO0 button
```

None of the camera pins are on the headers. That check alone rules out most "ESP32-CAM" pin maps
you'll find online.

## Camera pin map and how it was verified

| Signal | GPIO |
|--------|-----:|
| XCLK | 15 |
| SIOD / SIOC | 22 / 23 |
| VSYNC / HREF / PCLK | 18 / 36 / 26 |
| RESET | 5 |
| PWDN | not connected |
| D0…D7 | 2, 14, 35, 12, 27, 33, 34, 39 |

How each line was checked:
1. The seller's table and the community Tasmota template `OV5640_V1_2`
   (<https://templates.blakadder.com/OV5640_V1_2.html>) give the same camera map.
2. **SCCB:** the sensor answers on 22/23 with PID `0x5640`.
3. **Signal activity** (`/diag` endpoint): XCLK (15) and PCLK (26) toggle constantly, and VSYNC,
   HREF and all eight data lines show activity.
4. **Image:** RGB565 and grayscale 320x240 frames arrive every ~126 ms at XCLK 20 MHz with
   VSYNC=18 and HREF=36. With them swapped, nothing arrives. JPEG frames decode into correct
   pictures (see [CAMERA.md](CAMERA.md) for the XCLK story).

> Note on naming: the seller's table labels the data lines **D2…D9** (OV5640 10-bit numbering).
> In `esp32-camera` terms that's Y2…Y9 = `pin_d0`…`pin_d7`.

## TF card

The seller's table says `CLK=4, CMD=19, DAT0=13, DAT3=21`. The classic ESP32's SDMMC peripheral has
fixed pins (14/15/2/…) that are taken by the camera, so the card can only run in **SPI mode**. With
the obvious SPI mapping (`SCK=CLK, MOSI=CMD, MISO=DAT0, CS=DAT3`) the card never answers
`GO_IDLE_STATE`.

Trying every SPI assignment of the four pins (`/sdprobe`) finds the real wiring:

```
FOUND: SCK=4 MISO=13 MOSI=21 CS=19
type SDHC, card 3840 MB, FS 3824 MB, used 0 MB
write/read test: OK
```

So **CMD and DAT3 are swapped in the seller's table**: MOSI is 21 and CS is 19.

## IP5306

The seller's table puts the IP5306 on GPIO13 and GPIO4, the same pins as the SD card. An I2C scan
before mounting the card shows:

```
[I2C] SDA=13 SCL=4: 0x75
```

`0x75` is the address of the I2C variant of the IP5306, so battery level and charging state can be
read in software (registers 0x70/0x71/0x78 in the usual IP5306 libraries). This firmware doesn't
do it yet. Since the bus is shared with the SD card, you'd have to `SD.end()`, read over `Wire`,
then remount.

The listing also mentions a "HY2.0 connector on the internal I2C bus". We didn't find one on this
board: there's only an unpopulated 2-pin footprint next to the GIO0 button, and it wasn't traced.

## Strapping pins: the reason flashing is painful

| GPIO | Strapping role | On this board |
|------|----------------|---------------|
| 0 | boot mode (low = download) | GIO0 button. CH340X auto-reset does **not** drive it |
| 2 | must be low/floating for download mode | **camera D0** |
| 12 (MTDI) | flash voltage: high = 1.8 V | **camera D3** |
| 15 (MTDO) | boot log / timing | **camera XCLK** |
| 5 | SDIO timing | **camera RESET** |

Once the firmware has started the camera, the OV5640 keeps driving its data lines through an ESP32
reset, because EN only resets the ESP32. The observed boot strapping values (`boot:0x..` in the ROM
log) were:

- `boot:0x13`: normal flash boot (GPIO0=1, GPIO12=0)
- `boot:0x0b`: GPIO0=0 but **GPIO2=1** → esptool: *Wrong boot mode detected (0xb)*
- `boot:0x33`: **GPIO12=1** → VDD_SDIO = 1.8 V → endless `flash read err, 1000` boot loop

Fixes and the flashing procedure are in [FLASHING.md](FLASHING.md).

## Seller table errata (summary)

| Seller says | Reality |
|-------------|---------|
| TF CMD = GPIO19, DAT3 = GPIO21 | SPI MOSI = **21**, CS = **19** |
| "CH340X automatic download circuit" | auto-reset drives EN only; GPIO0 must be pressed by hand |
| camera data labelled D2…D9 | = Y2…Y9 / `pin_d0`…`pin_d7` (not wrong, just confusing) |
| "up to 30 FPS in VGA" | ~16 fps measured (JPEG, XCLK 16 MHz, Wi-Fi stream) |
