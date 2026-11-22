# Build and flash the firmware (Windows, PowerShell).
#
#   tools\build_upload.cmd               # build + OTA upload over Wi-Fi (default host esp32-cam.local)
#   tools\build_upload.cmd -NoUpload     # build only
#   tools\build_upload.cmd -HostName 192.168.1.50   # OTA to an IP (mDNS lookup on Windows takes ~1 s)
#   tools\build_upload.cmd -Usb          # flash over USB. Put the board into the bootloader FIRST:
#                                        #   hold GIO0 -> press & release REST -> release GIO0
#
# Requirements: arduino-cli (standalone or the one bundled with Arduino IDE 2.x),
# ESP32 Arduino core 3.x, esptool (pip install esptool) for -Usb, curl (built into Windows 10+).
param([switch]$NoUpload, [switch]$Usb, [string]$HostName = "esp32-cam.local", [string]$Port = "")
$ErrorActionPreference = 'Stop'
$root   = Split-Path $PSScriptRoot -Parent
$sketch = Join-Path $root "firmware\esp32_cam_web"
$build  = Join-Path $root "build"
# min_spiffs = 2 x 1.9 MB app slots (needed for OTA); PSRAM must be enabled for the camera.
$fqbn   = "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=min_spiffs,DebugLevel=warn"

# --- locate arduino-cli ---
$cli = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source
if (-not $cli) {
  foreach ($c in @("$env:ProgramFiles\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
                   "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe")) {
    if (Test-Path $c) { $cli = $c; break }
  }
}
if (-not $cli) { Write-Error "arduino-cli not found (install it or Arduino IDE 2.x)" }

if (-not (Test-Path (Join-Path $sketch "secrets.h"))) {
  Write-Error "firmware\esp32_cam_web\secrets.h is missing: copy secrets.example.h to secrets.h and set your Wi-Fi"
}

& $cli compile -b $fqbn --build-path $build $sketch
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($NoUpload) { exit 0 }

$app = Join-Path $build "esp32_cam_web.ino.bin"

if (-not $Usb) {
  Write-Host "OTA -> http://$HostName/update"
  & curl.exe -sS -m 120 --data-binary "@$app" -H "Content-Type: application/octet-stream" "http://$HostName/update"
  exit $LASTEXITCODE
}

# --- USB ---
# The board's CH340X auto-reset only drives EN, never GPIO0, so the board must already
# wait in the ROM bootloader and esptool must not reset it.
if (-not $Port) {
  $dev = Get-PnpDevice -PresentOnly | Where-Object { $_.FriendlyName -match 'CH340.*\((COM\d+)\)' } | Select-Object -First 1
  if (-not $dev) { Write-Error "CH340 serial port not found, pass -Port COMx" }
  $Port = $dev.FriendlyName -replace '.*\((COM\d+)\).*', '$1'
}
Write-Host "USB, port $Port"
# Do not reset on failure: the board stays in the bootloader and we can retry at a lower speed.
foreach ($baud in 460800, 230400, 115200) {
  Write-Host "esptool @ $baud baud"
  & esptool --chip esp32 --port $Port --baud $baud --before no-reset --after no-reset write-flash -z `
    --flash-mode keep --flash-freq keep --flash-size detect `
    0x1000  (Join-Path $build "esp32_cam_web.ino.bootloader.bin") `
    0x8000  (Join-Path $build "esp32_cam_web.ino.partitions.bin") `
    0xe000  (Join-Path $build "boot_app0.bin") `
    0x10000 $app
  if ($LASTEXITCODE -eq 0) {
    # success: pulse EN via RTS to start the new firmware
    python -c "import serial,time,sys;s=serial.Serial();s.port=sys.argv[1];s.dtr=False;s.rts=False;s.open();s.rts=True;time.sleep(0.15);s.rts=False;s.close()" $Port
    exit 0
  }
  Start-Sleep -Milliseconds 500
}
exit 1
