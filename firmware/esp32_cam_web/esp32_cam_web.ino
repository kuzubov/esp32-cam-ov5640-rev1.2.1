// ESP32-CAM "OV5640 REV1.2.1" (CH340X + IP5306 + TF slot) — MJPEG streaming web camera.
//
//   Web UI:    http://esp32-cam.local/
//   Stream:    http://esp32-cam.local:81/stream
//   Snapshot:  http://esp32-cam.local/capture
//
// See README.md and docs/ for the board pinout and its quirks.

#include <WiFi.h>
#include <ESPmDNS.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Update.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy secrets.example.h to secrets.h and put your Wi-Fi credentials there."
#endif
#include "board_pins.h"
#include "index_html.h"

static httpd_handle_t web_httpd = nullptr;
static httpd_handle_t stream_httpd = nullptr;
static volatile float stream_fps = 0;
static volatile int stream_clients = 0;
static bool led_on = false;

// Guards the camera against grabbing a frame while it is being re-initialized.
static SemaphoreHandle_t cam_mutex = xSemaphoreCreateMutex();
struct CamLock {
  CamLock() { xSemaphoreTake(cam_mutex, portMAX_DELAY); }
  ~CamLock() { xSemaphoreGive(cam_mutex); }
};

// ============================================================== camera
struct CamParams {
  int xclk_mhz = CAM_XCLK_MHZ;
  bool swap_sync = false;                 // swap VSYNC/HREF (diagnostics only)
  framesize_t init_fs = FRAMESIZE_QSXGA;  // frame size at init: buffers are sized for it
  framesize_t run_fs = FRAMESIZE_VGA;     // frame size actually used
  pixformat_t fmt = PIXFORMAT_JPEG;
  int fb_count = 2;
};
static CamParams cam_params;
static String cam_last_error;

static bool initCamera(const CamParams &P = cam_params) {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = P.swap_sync ? HREF_GPIO_NUM : VSYNC_GPIO_NUM;
  c.pin_href = P.swap_sync ? VSYNC_GPIO_NUM : HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = P.xclk_mhz * 1000000;
  c.pixel_format = P.fmt;
  c.grab_mode = CAMERA_GRAB_LATEST;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  // Frame buffers are allocated for the init frame size, so start with the
  // largest one and switch to the working size afterwards.
  c.frame_size = P.init_fs;
  c.jpeg_quality = 10;
  c.fb_count = P.fb_count;

  if (!psramFound()) {
    Serial.println("[CAM] PSRAM not found! Falling back to a reduced mode.");
    c.frame_size = FRAMESIZE_SVGA;
    c.fb_location = CAMERA_FB_IN_DRAM;
    c.fb_count = 1;
  }

  Serial.printf("[CAM] init: xclk=%d MHz, vsync=%d href=%d, init_fs=%d run_fs=%d fmt=%d fb=%d\n",
                P.xclk_mhz, c.pin_vsync, c.pin_href, P.init_fs, P.run_fs, P.fmt, P.fb_count);
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    cam_last_error = String("esp_camera_init 0x") + String(err, HEX) + " " + esp_err_to_name(err);
    Serial.printf("[CAM] esp_camera_init FAILED 0x%x (%s)\n", err, esp_err_to_name(err));
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("[CAM] sensor PID=0x%04x (OV5640=0x5640)\n", s->id.PID);
  if (P.run_fs != P.init_fs) s->set_framesize(s, P.run_fs);
  s->set_quality(s, 12);
  cam_last_error = "";
  return true;
}

// Camera self-test.
// On this board every camera init randomly lands either in a "good" state
// (VGA frames steadily every ~63 ms) or in a "bad" one (truncated JPEGs,
// ~1 frame/s, dropped frames). It looks like a random PCLK phase relative to
// the I2S sampling. The state persists until the next init, so we measure it
// and re-initialize until we get a good one (usually 1-3 attempts).
static bool camSelfTest(String *log = nullptr) {
  for (int i = 0; i < 2; i++) {  // warm-up
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }
  const int N = 8;
  size_t mn = SIZE_MAX, mx = 0;
  int64_t t0 = esp_timer_get_time();
  for (int i = 0; i < N; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      if (log) *log += "self-test: frame timeout\n";
      Serial.println("[CAM] self-test: frame timeout");
      return false;
    }
    mn = min(mn, fb->len);
    mx = max(mx, fb->len);
    esp_camera_fb_return(fb);
  }
  int avg_ms = (esp_timer_get_time() - t0) / 1000 / N;
  bool ok = avg_ms < 200 && mn * 3 > mx;  // fast and no truncated frames
  char b[128];
  snprintf(b, sizeof(b), "self-test: %d ms/frame, size %u..%u -> %s\n", avg_ms, (unsigned)mn, (unsigned)mx,
           ok ? "OK" : "BAD");
  Serial.print("[CAM] ");
  Serial.print(b);
  if (log) *log += b;
  return ok;
}

// Init with self-test and retries.
static bool startCamera(String *log = nullptr) {
  CamLock lock;
  for (int attempt = 1; attempt <= 8; attempt++) {
    esp_camera_deinit();
    delay(100);
    if (!initCamera()) continue;
    // self-test always runs in VGA, then the selected frame size is restored
    sensor_t *s = esp_camera_sensor_get();
    framesize_t fs = cam_params.run_fs;
    if (fs != FRAMESIZE_VGA) s->set_framesize(s, FRAMESIZE_VGA);
    bool ok = camSelfTest(log);
    if (fs != FRAMESIZE_VGA) s->set_framesize(s, fs);
    if (ok) {
      Serial.printf("[CAM] camera ready (attempt %d)\n", attempt);
      if (log) *log += "camera ready, attempt " + String(attempt) + "\n";
      return true;
    }
  }
  Serial.println("[CAM] self-test failed 8 times, running as is");
  if (log) *log += "self-test failed 8 times\n";
  return false;
}

// Diagnostics: re-init the camera with custom parameters and try to grab frames.
// /camtest?xclk=16&swap=0&ifs=23&fs=10&fmt=4&fb=2&keep=0
//   fmt: 4=JPEG, 0=RGB565, 1=YUV422, 3=GRAYSCALE; keep=1 keeps the params if frames arrived.
static esp_err_t camtestHandler(httpd_req_t *req) {
  char q[128] = "", v[16];
  httpd_req_get_url_query_str(req, q, sizeof(q));
  CamParams P = cam_params;
  if (httpd_query_key_value(q, "xclk", v, sizeof(v)) == ESP_OK) P.xclk_mhz = atoi(v);
  if (httpd_query_key_value(q, "swap", v, sizeof(v)) == ESP_OK) P.swap_sync = atoi(v);
  if (httpd_query_key_value(q, "ifs", v, sizeof(v)) == ESP_OK) P.init_fs = (framesize_t)atoi(v);
  if (httpd_query_key_value(q, "fs", v, sizeof(v)) == ESP_OK) P.run_fs = (framesize_t)atoi(v);
  if (httpd_query_key_value(q, "fmt", v, sizeof(v)) == ESP_OK) P.fmt = (pixformat_t)atoi(v);
  if (httpd_query_key_value(q, "fb", v, sizeof(v)) == ESP_OK) P.fb_count = atoi(v);
  bool keep = httpd_query_key_value(q, "keep", v, sizeof(v)) == ESP_OK && atoi(v);
  CamLock lock;

  esp_camera_deinit();
  delay(100);
  String out;
  bool ok = initCamera(P);
  out += ok ? "init OK\n" : "init FAIL: " + cam_last_error + "\n";
  int good = 0;
  if (ok) {
    for (int i = 0; i < 4; i++) {
      int64_t t0 = esp_timer_get_time();
      camera_fb_t *fb = esp_camera_fb_get();
      int ms = (esp_timer_get_time() - t0) / 1000;
      if (!fb) {
        out += "frame " + String(i) + ": NONE (" + String(ms) + " ms)\n";
        continue;
      }
      good++;
      char b[160];
      snprintf(b, sizeof(b), "frame %d: %ux%u fmt=%d len=%u (%d ms) bytes: %02X %02X %02X %02X\n", i,
               fb->width, fb->height, fb->format, fb->len, ms, fb->buf[0], fb->buf[1], fb->buf[2], fb->buf[3]);
      out += b;
      esp_camera_fb_return(fb);
    }
  }
  if (keep && good) {
    cam_params = P;
    out += "parameters kept as working ones\n";
  } else {
    esp_camera_deinit();
    delay(100);
    initCamera(cam_params);
    out += "working parameters restored\n";
  }
  Serial.print(out);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_send(req, out.c_str(), out.length());
}

// ============================================================== I2C (IP5306)
static String i2c_report;
static void scanI2C(int sda, int scl) {
  Wire.begin(sda, scl, 100000);
  String found;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) found += String(found.length() ? ", 0x" : "0x") + String(a, HEX);
  }
  Wire.end();
  String line = "SDA=" + String(sda) + " SCL=" + String(scl) + ": " + (found.length() ? found : "no devices");
  Serial.println("[I2C] " + line);
  i2c_report += line + "\n";
}

// ============================================================== SD card (SPI mode)
static SPIClass sdSPI(HSPI);
static bool sd_ok = false;
static String sd_report;
static int sd_pins[4] = {SD_SCK_GPIO_NUM, SD_MISO_GPIO_NUM, SD_MOSI_GPIO_NUM, SD_CS_GPIO_NUM};

static bool mountSD(int sck, int miso, int mosi, int cs, uint32_t hz) {
  SD.end();
  sdSPI.end();
  sdSPI.begin(sck, miso, mosi, cs);
  return SD.begin(cs, sdSPI, hz);
}

static bool initSD() {
  sd_report = "";
  if (!mountSD(sd_pins[0], sd_pins[1], sd_pins[2], sd_pins[3], 4000000)) {
    sd_report = "SD: no card / mount failed (SCK=" + String(sd_pins[0]) + " MISO=" + String(sd_pins[1]) +
                " MOSI=" + String(sd_pins[2]) + " CS=" + String(sd_pins[3]) + ")\n";
    Serial.print("[SD] " + sd_report);
    return false;
  }
  const char *types[] = {"none", "MMC", "SD", "SDHC", "?"};
  uint8_t t = SD.cardType();
  char buf[160];
  snprintf(buf, sizeof(buf), "type %s, card %llu MB, FS %llu MB, used %llu MB",
           types[t > 4 ? 4 : t], SD.cardSize() >> 20, SD.totalBytes() >> 20, SD.usedBytes() >> 20);
  sd_report = String(buf) + "\n";
  Serial.printf("[SD] %s\n", buf);

  // write / read-back test
  const char *path = "/esp32cam_test.txt";
  String payload = "ESP32-CAM SD test " + String((uint32_t)esp_random(), HEX);
  bool ok = false;
  File f = SD.open(path, FILE_WRITE);
  if (f) {
    f.print(payload);
    f.close();
    f = SD.open(path, FILE_READ);
    if (f) {
      ok = f.readString() == payload;
      f.close();
    }
    SD.remove(path);
  }
  sd_report += String("write/read test: ") + (ok ? "OK" : "FAILED") + "\n";
  Serial.printf("[SD] write/read test: %s\n", ok ? "OK" : "FAILED");

  File root = SD.open("/");
  int n = 0;
  sd_report += "root:\n";
  for (File e = root.openNextFile(); e && n < 30; e = root.openNextFile(), n++) {
    sd_report += String("  ") + (e.isDirectory() ? "[DIR] " : "") + e.name() +
                 (e.isDirectory() ? "" : "  (" + String((uint32_t)e.size()) + " B)") + "\n";
  }
  return ok;
}

// Prepare for USB flashing (serial command "flash").
// Camera data lines D0=GPIO2 and D3=GPIO12 are ESP32 strapping pins: if the
// running camera holds them high at reset, the ROM bootloader won't start.
// Stop the camera and keep it in RESET: after reset OV5640 outputs are disabled.
static void prepareForFlash() {
  esp_camera_deinit();
  pinMode(RESET_GPIO_NUM, OUTPUT);
  digitalWrite(RESET_GPIO_NUM, LOW);
  Serial.println("READY_FOR_FLASH");
}

// ============================================================== HTTP handlers
static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captureHandler(httpd_req_t *req) {
  CamLock lock;
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t streamHandler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "60");

  stream_clients++;
  Serial.printf("[HTTP] stream: client connected (%d total)\n", stream_clients);
  char part[64];
  int64_t last = esp_timer_get_time();
  int frames = 0;
  int misses = 0;

  while (true) {
    xSemaphoreTake(cam_mutex, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      xSemaphoreGive(cam_mutex);
      Serial.println("[CAM] frame grab failed");
      if (++misses >= 3) {
        res = ESP_FAIL;
        break;
      }
      continue;
    }
    misses = 0;
    size_t hlen = snprintf(part, sizeof(part), STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    xSemaphoreGive(cam_mutex);
    if (res != ESP_OK) break;

    frames++;
    int64_t now = esp_timer_get_time();
    if (now - last >= 1000000) {
      stream_fps = frames * 1000000.0f / (now - last);
      frames = 0;
      last = now;
    }
  }
  stream_clients--;
  if (stream_clients == 0) stream_fps = 0;
  Serial.printf("[HTTP] stream: client disconnected (%d left)\n", stream_clients);
  return res;
}

static esp_err_t statusHandler(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();
  char json[1400];
  char *p = json;
  p += sprintf(p, "{");
  if (s) {
    p += sprintf(p, "\"pid\":%u,", s->id.PID);
    p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
    p += sprintf(p, "\"quality\":%u,", s->status.quality);
    p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
    p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
    p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
    p += sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
    p += sprintf(p, "\"denoise\":%u,", s->status.denoise);
    p += sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
    p += sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
    p += sprintf(p, "\"awb\":%u,", s->status.awb);
    p += sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
    p += sprintf(p, "\"aec\":%u,", s->status.aec);
    p += sprintf(p, "\"aec2\":%u,", s->status.aec2);
    p += sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
    p += sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
    p += sprintf(p, "\"agc\":%u,", s->status.agc);
    p += sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
    p += sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
    p += sprintf(p, "\"bpc\":%u,", s->status.bpc);
    p += sprintf(p, "\"wpc\":%u,", s->status.wpc);
    p += sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
    p += sprintf(p, "\"lenc\":%u,", s->status.lenc);
    p += sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
    p += sprintf(p, "\"vflip\":%u,", s->status.vflip);
    p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
    p += sprintf(p, "\"colorbar\":%u,", s->status.colorbar);
  }
  p += sprintf(p, "\"led\":%u,", led_on ? 1 : 0);
  p += sprintf(p, "\"sd\":%d,", sd_ok ? 1 : 0);
  p += sprintf(p, "\"sd_total\":%u,", sd_ok ? (unsigned)(SD.totalBytes() >> 20) : 0);
  p += sprintf(p, "\"sd_used\":%u,", sd_ok ? (unsigned)(SD.usedBytes() >> 20) : 0);
  p += sprintf(p, "\"fps\":%.1f,", stream_fps);
  p += sprintf(p, "\"clients\":%d,", stream_clients);
  p += sprintf(p, "\"rssi\":%d,", WiFi.RSSI());
  p += sprintf(p, "\"ip\":\"%s\",", WiFi.localIP().toString().c_str());
  p += sprintf(p, "\"host\":\"%s.local\",", MDNS_HOSTNAME);
  p += sprintf(p, "\"uptime\":%lu,", (unsigned long)(millis() / 1000));
  p += sprintf(p, "\"heap\":%u,", (unsigned)ESP.getFreeHeap());
  p += sprintf(p, "\"psram\":%u,", (unsigned)ESP.getPsramSize());
  p += sprintf(p, "\"psram_free\":%u", (unsigned)ESP.getFreePsram());
  p += sprintf(p, "}");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, p - json);
}

// Diagnostics: how many times each GPIO toggled during N register reads.
// Handy to see which pins actually carry camera signals.
static esp_err_t diagHandler(httpd_req_t *req) {
  static const int pins[] = {0, 2, 4, 5, 12, 13, 14, 15, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33, 34, 35, 36, 37, 38, 39};
  const int N = sizeof(pins) / sizeof(pins[0]);
  uint32_t cnt[40] = {0};
  uint32_t p0 = REG_READ(GPIO_IN_REG), p1 = REG_READ(GPIO_IN1_REG);
  uint32_t first0 = p0, first1 = p1;
  const int SAMPLES = 300000;
  int64_t t0 = esp_timer_get_time();
  for (int i = 0; i < SAMPLES; i++) {
    uint32_t a = REG_READ(GPIO_IN_REG), b = REG_READ(GPIO_IN1_REG);
    uint32_t d0 = a ^ p0, d1 = b ^ p1;
    if (d0 | d1) {
      for (int k = 0; k < N; k++) {
        int g = pins[k];
        if (g < 32 ? (d0 >> g) & 1 : (d1 >> (g - 32)) & 1) cnt[g]++;
      }
    }
    p0 = a;
    p1 = b;
  }
  int64_t dt = esp_timer_get_time() - t0;
  char out[900];
  char *p = out;
  p += sprintf(p, "%d samples in %lld us\nGPIO  toggles  level\n", SAMPLES, dt);
  for (int k = 0; k < N; k++) {
    int g = pins[k];
    int lvl = g < 32 ? (first0 >> g) & 1 : (first1 >> (g - 32)) & 1;
    p += sprintf(p, "%4d  %7u  %d\n", g, (unsigned)cnt[g], lvl);
  }
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, out, p - out);
}

// Diagnostics: brute-force SPI pin assignment until the SD card mounts.
// /sdprobe?pins=4,13,19,21   (default: these four)
static esp_err_t sdprobeHandler(httpd_req_t *req) {
  int cand[8] = {4, 13, 19, 21};
  int n = 4;
  char q[96] = "", v[64];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "pins", v, sizeof(v)) == ESP_OK) {
    n = 0;
    for (char *t = strtok(v, ","); t && n < 8; t = strtok(nullptr, ",")) cand[n++] = atoi(t);
  }
  String out;
  bool found = false;
  int tries = 0;
  for (int a = 0; a < n && !found; a++)
    for (int b = 0; b < n && !found; b++)
      for (int c = 0; c < n && !found; c++)
        for (int d = 0; d < n && !found; d++) {
          int p[4] = {cand[a], cand[b], cand[c], cand[d]};
          if (a == b || a == c || a == d || b == c || b == d || c == d) continue;
          if (p[0] >= 34 || p[2] >= 34 || p[3] >= 34) continue;  // SCK/MOSI/CS must be outputs
          tries++;
          if (mountSD(p[0], p[1], p[2], p[3], 400000)) {
            memcpy(sd_pins, p, sizeof(p));
            out += "FOUND: SCK=" + String(p[0]) + " MISO=" + String(p[1]) + " MOSI=" + String(p[2]) +
                   " CS=" + String(p[3]) + "\n";
            found = true;
          }
        }
  out += "combinations tried: " + String(tries) + "\n";
  if (found) {
    sd_ok = initSD();
    out += sd_report;
  } else {
    SD.end();
    sdSPI.end();
    out += "card did not respond to any combination\n";
  }
  Serial.print("[SD] " + out);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_send(req, out.c_str(), out.length());
}

// OTA: curl --data-binary @esp32_cam_web.ino.bin http://esp32-cam.local/update
static esp_err_t updateHandler(httpd_req_t *req) {
  size_t total = req->content_len;
  Serial.printf("[OTA] receiving firmware, %u bytes\n", (unsigned)total);
  xSemaphoreTake(cam_mutex, portMAX_DELAY);  // stops the stream; never released — we reboot afterwards
  esp_camera_deinit();                       // free memory and the bus
  if (total == 0 || !Update.begin(total)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "Update.begin failed, rebooting\n");
    delay(300);
    ESP.restart();
    return ESP_FAIL;
  }
  static uint8_t buf[4096];
  size_t got = 0;
  int last_pct = -1;
  while (got < total) {
    int r = httpd_req_recv(req, (char *)buf, sizeof(buf));
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) break;
    if (Update.write(buf, r) != (size_t)r) break;
    got += r;
    int pct = got * 100 / total;
    if (pct / 10 != last_pct / 10) {
      Serial.printf("[OTA] %d%%\n", pct);
      last_pct = pct;
    }
  }
  bool ok = got == total && Update.end(true);
  String msg = ok ? "OK, rebooting\n" : String("FAILED: ") + Update.errorString() + "\n";
  Serial.print("[OTA] " + msg);
  if (!ok) httpd_resp_set_status(req, "500 Internal Server Error");
  httpd_resp_send(req, msg.c_str(), msg.length());
  delay(300);
  ESP.restart();
  return ESP_OK;
}

static esp_err_t sdHandler(httpd_req_t *req) {
  String out = sd_report + "\nI2C scan (IP5306 = 0x75):\n" + i2c_report;
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_send(req, out.c_str(), out.length());
}

static esp_err_t controlHandler(httpd_req_t *req) {
  char query[96], var[32], val[16];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "var", var, sizeof(var)) != ESP_OK ||
      httpd_query_key_value(query, "val", val, sizeof(val)) != ESP_OK) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  int v = atoi(val);
  int r = 0;

  if (!strcmp(var, "reinit")) {
    String log;
    startCamera(&log);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, log.c_str(), log.length());
  }
  if (!strcmp(var, "reboot")) {
    httpd_resp_send(req, "rebooting", HTTPD_RESP_USE_STRLEN);
    delay(200);
    ESP.restart();
  }
  if (!strcmp(var, "led")) {
    led_on = v != 0;
    digitalWrite(LED_GPIO_NUM, led_on ? HIGH : LOW);
    return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
  }

  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "camera not initialized", HTTPD_RESP_USE_STRLEN);
  }
  if (!strcmp(var, "framesize")) {
    r = s->set_framesize(s, (framesize_t)v);
    if (r == 0) cam_params.run_fs = (framesize_t)v;  // survive camera re-init
  }
  else if (!strcmp(var, "quality")) r = s->set_quality(s, v);
  else if (!strcmp(var, "brightness")) r = s->set_brightness(s, v);
  else if (!strcmp(var, "contrast")) r = s->set_contrast(s, v);
  else if (!strcmp(var, "saturation")) r = s->set_saturation(s, v);
  else if (!strcmp(var, "sharpness")) r = s->set_sharpness(s, v);
  else if (!strcmp(var, "denoise")) r = s->set_denoise(s, v);
  else if (!strcmp(var, "special_effect")) r = s->set_special_effect(s, v);
  else if (!strcmp(var, "wb_mode")) r = s->set_wb_mode(s, v);
  else if (!strcmp(var, "awb")) r = s->set_whitebal(s, v);
  else if (!strcmp(var, "awb_gain")) r = s->set_awb_gain(s, v);
  else if (!strcmp(var, "aec")) r = s->set_exposure_ctrl(s, v);
  else if (!strcmp(var, "aec2")) r = s->set_aec2(s, v);
  else if (!strcmp(var, "ae_level")) r = s->set_ae_level(s, v);
  else if (!strcmp(var, "aec_value")) r = s->set_aec_value(s, v);
  else if (!strcmp(var, "agc")) r = s->set_gain_ctrl(s, v);
  else if (!strcmp(var, "agc_gain")) r = s->set_agc_gain(s, v);
  else if (!strcmp(var, "gainceiling")) r = s->set_gainceiling(s, (gainceiling_t)v);
  else if (!strcmp(var, "bpc")) r = s->set_bpc(s, v);
  else if (!strcmp(var, "wpc")) r = s->set_wpc(s, v);
  else if (!strcmp(var, "raw_gma")) r = s->set_raw_gma(s, v);
  else if (!strcmp(var, "lenc")) r = s->set_lenc(s, v);
  else if (!strcmp(var, "hmirror")) r = s->set_hmirror(s, v);
  else if (!strcmp(var, "vflip")) r = s->set_vflip(s, v);
  else if (!strcmp(var, "dcw")) r = s->set_dcw(s, v);
  else if (!strcmp(var, "colorbar")) r = s->set_colorbar(s, v);
  else r = -1;

  Serial.printf("[CTRL] %s = %d -> %s\n", var, v, r == 0 ? "ok" : "error / not supported");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  if (r != 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "unsupported", HTTPD_RESP_USE_STRLEN);
  }
  return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

static void startServers() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_uri_handlers = 12;
  cfg.stack_size = 8192;
  cfg.recv_wait_timeout = 30;
  cfg.send_wait_timeout = 30;

  httpd_uri_t index_uri = {"/", HTTP_GET, indexHandler, nullptr};
  httpd_uri_t status_uri = {"/status", HTTP_GET, statusHandler, nullptr};
  httpd_uri_t control_uri = {"/control", HTTP_GET, controlHandler, nullptr};
  httpd_uri_t capture_uri = {"/capture", HTTP_GET, captureHandler, nullptr};
  httpd_uri_t stream_uri = {"/stream", HTTP_GET, streamHandler, nullptr};
  httpd_uri_t diag_uri = {"/diag", HTTP_GET, diagHandler, nullptr};
  httpd_uri_t sd_uri = {"/sd", HTTP_GET, sdHandler, nullptr};
  httpd_uri_t sdprobe_uri = {"/sdprobe", HTTP_GET, sdprobeHandler, nullptr};
  httpd_uri_t camtest_uri = {"/camtest", HTTP_GET, camtestHandler, nullptr};
  httpd_uri_t update_uri = {"/update", HTTP_POST, updateHandler, nullptr};

  if (httpd_start(&web_httpd, &cfg) == ESP_OK) {
    httpd_register_uri_handler(web_httpd, &index_uri);
    httpd_register_uri_handler(web_httpd, &status_uri);
    httpd_register_uri_handler(web_httpd, &control_uri);
    httpd_register_uri_handler(web_httpd, &capture_uri);
    httpd_register_uri_handler(web_httpd, &diag_uri);
    httpd_register_uri_handler(web_httpd, &sd_uri);
    httpd_register_uri_handler(web_httpd, &sdprobe_uri);
    httpd_register_uri_handler(web_httpd, &camtest_uri);
    httpd_register_uri_handler(web_httpd, &update_uri);
    Serial.printf("[HTTP] web server on port %d\n", cfg.server_port);
  } else {
    Serial.println("[HTTP] web server FAILED to start");
  }

  // The stream runs on its own server so it doesn't block the controls.
  cfg.server_port = 81;
  cfg.ctrl_port += 1;
  if (httpd_start(&stream_httpd, &cfg) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    Serial.printf("[HTTP] stream server on port %d\n", cfg.server_port);
  } else {
    Serial.println("[HTTP] stream server FAILED to start");
  }
}

// ============================================================== Wi-Fi
static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_HOSTNAME);
  WiFi.setSleep(false);  // no modem sleep: noticeably smoother stream
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WiFi] connecting to \"%s\"", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
    if (millis() - t0 > 30000) {
      Serial.printf("\n[WiFi] no connection in 30 s (status %d), rebooting\n", WiFi.status());
      ESP.restart();
    }
  }
  Serial.printf("\n[WiFi] connected, IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] http://%s.local\n", MDNS_HOSTNAME);
  } else {
    Serial.println("[mDNS] FAILED to start");
  }
}

// ============================================================== main
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n=== ESP32-CAM OV5640 REV1.2.1 web camera ===");
  Serial.printf("Chip: %s rev %d, %d MHz, flash %u MB\n", ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getCpuFreqMHz(), ESP.getFlashChipSize() >> 20);
  Serial.printf("PSRAM: %s, %u bytes\n", psramFound() ? "yes" : "NO", (unsigned)ESP.getPsramSize());

  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);

  // IP5306 shares GPIO13/GPIO4 with the SD card, so scan I2C before mounting SD.
  scanI2C(IP5306_SDA_GPIO_NUM, IP5306_SCL_GPIO_NUM);
  sd_ok = initSD();

  connectWiFi();
  startCamera();
  startServers();
  Serial.printf("\nReady! Open http://%s.local  (or http://%s)\n\n", MDNS_HOSTNAME,
                WiFi.localIP().toString().c_str());
}

void loop() {
  // serial commands: "flash" — release the camera lines before USB flashing
  static String cmd;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      cmd.trim();
      if (cmd == "flash") prepareForFlash();
      cmd = "";
    } else if (cmd.length() < 32) {
      cmd += ch;
    }
  }

  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 10000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] connection lost, reconnecting...");
      WiFi.reconnect();
    }
  }
  delay(20);
}
