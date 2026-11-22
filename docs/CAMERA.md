# OV5640 camera notes

Driver: `esp32-camera` as bundled with ESP32 Arduino core 3.3.12. Frame buffers are in PSRAM,
`fb_count = 2`, `CAMERA_GRAB_LATEST`. The camera is initialized at QSXGA, so the buffers are big
enough, and then switched to the working frame size.

## Symptom with default settings

With the usual `xclk_freq_hz = 20000000` and JPEG:

- `esp_camera_init()` succeeds and the sensor answers (PID 0x5640)
- `esp_camera_fb_get()` **times out every time** (no frames, no driver errors in the log)

## XCLK sweep (JPEG, VGA)

Four `esp_camera_fb_get()` calls right after init, for two init frame sizes (VGA and QSXGA):

| XCLK | Result |
|------|--------|
| 5 MHz | frames arrive, 180–780 ms, sizes jump between 7 KB and 22 KB |
| 8 MHz | some frames, 0.4–3 s, occasional timeouts |
| 10 MHz | frames every 0.45–4 s, some truncated (~1.1 KB) |
| 12 MHz | mostly timeouts or truncated frames |
| **16 MHz** | **steady: every 63 ms, consistent size** |
| 20 MHz | no frames at all |
| 24 MHz | no frames at all |

Non-JPEG formats at 20 MHz work fine:

| Format @ 20 MHz, QVGA | Result |
|-----------------------|--------|
| RGB565 | ✅ 153600 B every 126 ms |
| GRAYSCALE | ✅ 76800 B every 126 ms |
| RGB565 with VSYNC/HREF swapped | ❌ no frames (confirms 18 = VSYNC, 36 = HREF) |

The results are non-monotonic (10 MHz is worse than 5 MHz, 16 MHz is perfect, 20 MHz is dead). That
points to a timing interaction between the OV5640's JPEG PCLK and the classic ESP32's I2S camera
sampling, not to a wiring fault. **This firmware uses 16 MHz** (`CAM_XCLK_MHZ` in `board_pins.h`).

## Frame sizes at 16 MHz (JPEG)

| Frame size | | Notes |
|------------|---|-------|
| 96X96, QQVGA, QCIF, HQVGA, 240X240, QVGA, CIF, HVGA | ✅ | |
| VGA 640x480 | ✅ | 63 ms/frame; 157–158 frames per 10 s over Wi-Fi (~16 fps) |
| **SVGA 800x600** | ❌ | no frames |
| XGA 1024x768 | ✅ | |
| HD 1280x720 | ✅ | 61 ms/frame |
| SXGA 1280x1024 | ✅ | |
| UXGA 1600x1200 | ✅ | 349 ms/frame |
| FHD 1920x1080 | ✅ | |
| **P_HD 720x1280** | ❌ | no frames |
| **P_3MP 864x1536** | ❌ | no frames |
| **QXGA 2048x1536** | ❌ | no frames |
| QHD 2560x1440 | ✅ | |
| WQXGA 2560x1600 | ✅ | |
| P_FHD 1080x1920 | ✅ | |
| QSXGA 2560x1920 | ✅ | 349 ms/frame, ~110 KB |
| **5MP 2592x1944** | ❌ | no frames |

The web UI only offers the working sizes.

## The init lottery

Even at 16 MHz, **each `esp_camera_init()` randomly lands in one of two states**, and stays there
until the next init:

- **good:** VGA frames every 63 ms, sizes consistent (e.g. 10.4–10.5 KB for a static scene)
- **bad:** frames every ~1 s, mostly **truncated**: ~1.1 KB, only the top few lines of the image,
  the rest gray. Sometimes there are 4 s timeouts.

This isn't auto-exposure: fixing `aec_value` at 200, 400 or 885 doesn't change anything. It isn't
the CPU core the driver interrupt is registered on either: we tested init on core 0, on core 1 and
from the httpd task. Both states show up regardless.

A typical log of five consecutive re-inits with the self-test:

```
self-test: frame timeout
self-test: 1330 ms/frame, size 2396..10738 -> BAD
self-test: 63 ms/frame, size 8011..10823 -> OK
camera ready, attempt 3
...
self-test: 63 ms/frame, size 8014..10880 -> OK
camera ready, attempt 1
```

**Workaround used in the firmware** (`camSelfTest()` / `startCamera()`):

1. init the camera
2. grab 2 warm-up frames, then time 8 frames in VGA
3. the state counts as good if the average is under 200 ms and `min_size * 3 > max_size`
4. otherwise deinit and retry, up to 8 attempts

The same routine runs from the UI's **Restart camera** button (`/control?var=reinit&val=1`).

## Other observations

- The OV5640 driver reports `gainceiling` as a raw register value (e.g. 248), not as the 0–6 enum.
  The UI shows it as `(248)`.
- There's a pink/magenta tint in the center of the frame, typical for OV5640 modules without lens
  shading calibration. `lenc` helps a bit.
- Autofocus isn't initialized: the AF firmware isn't uploaded to the sensor, so the lens stays at its
  default position. A room at 1–3 m looked acceptable, but focus wasn't evaluated in detail.
