#pragma once
// Pin map of the ESP32-CAM "OV5640 REV1.2.1" board (CH340X + IP5306 + TF slot).
// Verified on real hardware — see docs/HARDWARE.md.
// NOTE: camera wiring is DIFFERENT from the classic AI-Thinker ESP32-CAM!

// ---- OV5640 camera (DVP) ----
#define PWDN_GPIO_NUM   -1   // not connected
#define RESET_GPIO_NUM   5
#define XCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM   22   // SCCB SDA
#define SIOC_GPIO_NUM   23   // SCCB SCL

#define Y2_GPIO_NUM      2   // D0  (strapping pin!)
#define Y3_GPIO_NUM     14   // D1
#define Y4_GPIO_NUM     35   // D2
#define Y5_GPIO_NUM     12   // D3  (strapping pin MTDI!)
#define Y6_GPIO_NUM     27   // D4
#define Y7_GPIO_NUM     33   // D5
#define Y8_GPIO_NUM     34   // D6
#define Y9_GPIO_NUM     39   // D7

#define VSYNC_GPIO_NUM  18
#define HREF_GPIO_NUM   36
#define PCLK_GPIO_NUM   26

// XCLK frequency. On this board JPEG is only stable at 16 MHz:
// 20/24 MHz -> no JPEG frames at all, 5-12 MHz -> frames with gaps.
#define CAM_XCLK_MHZ    16

// ---- TF card, SPI mode ----
// The seller's table swaps CMD(19) and DAT3(21) — the real wiring is MOSI=21, CS=19.
#define SD_SCK_GPIO_NUM   4
#define SD_MISO_GPIO_NUM 13
#define SD_MOSI_GPIO_NUM 21
#define SD_CS_GPIO_NUM   19

// ---- IP5306 power management (I2C, address 0x75) — shares pins with the SD card! ----
#define IP5306_SDA_GPIO_NUM 13
#define IP5306_SCL_GPIO_NUM  4

// ---- misc ----
#define LED_GPIO_NUM    25   // also routed to the TFT connector
// TFT FPC connector: GPIO 25, 32, 37, 38 (not tested)
