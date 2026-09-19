# Project L.E.A.F. Camera Firmware (`proleaf-camera`)

Independent PlatformIO firmware for the **ESP32-S3-CAM N16R8**, responsible for camera,
INMP441 microphone, Wi-Fi, and real-time 1280×720 (HD) @ 30 FPS MJPEG live streaming.

This project is fully separate from the main `proleaf` sensor/automation firmware.
The main ESP32 keeps handling environmental sensors, automation, and MQTT telemetry.

```
Greenhouse 1
├── Main Device:   esp32-001     → proleaf firmware (sensors/automation/MQTT)
└── Camera Device: esp32-cam-001 → proleaf-camera firmware (this repo folder)
```

## Features & Current Scope

- **Sensor & Resolution**: OV3660 @ 1280×720 (FRAMESIZE_HD) with automated sensor PID detection.
- **Framerate Target**: 30.0 FPS with sub-millisecond adaptive pacing; graceful fallback to 20–25 FPS if network bandwidth or hardware throughput dips.
- **PSRAM DMA Buffering**: 3 framebuffers allocated in 8MB Octal (OPI) PSRAM (`CAMERA_FB_IN_PSRAM`, `CAMERA_GRAB_LATEST`) for zero-tearing DMA ping-pong.
- **Image Quality & Greenhouse ISP Profile**: Tuned for plant foliage clarity and crisp detail without compression blockiness (`CAMERA_JPEG_QUALITY = 12`, Auto-Exposure DSP AEC2, Auto-Gain with 2X ceiling, Auto White Balance AWB, Black/White Pixel Cancellation, Lens Shading Correction, and Gamma curve).
- **Asynchronous HTTP Server**: Built on ESP-IDF `esp_http_server` running in a dedicated FreeRTOS background task on Core 0. Never blocks `loopTask`, sensor routines, or MQTT tasks on Core 1.
- **Real-Time FPS Diagnostics**: Precise hardware capture FPS and socket stream FPS measured via microsecond timer (`esp_timer_get_time()`) and logged to the serial monitor every second.
- **INMP441 I2S MEMS Microphone**: Continuous background I2S DMA capture task on Core 0 with RMS/peak audio level telemetry.
- **Laravel Backend Integration**: Periodic non-blocking heartbeat reporting device status, IP, and live stream telemetry.
- **Responsive HTML5 Dashboard**: Dark greenhouse-themed web UI at `http://<device-ip>/` featuring live 720p stream, real-time FPS HUD, telemetry dashboard, snapshot capture, and live quality adjustments.

## Project Layout

```
proleaf-camera/
├── platformio.ini                  # build config; reads LEAF_* env vars
├── boards/
│   └── esp32-s3-cam-n16r8.json     # N16R8 target: QIO flash, OPI PSRAM, 16MB
├── include/
│   ├── Config.h                    # identity, pins, Wi-Fi, audio, camera, timing
│   ├── NetworkManager.h            # non-blocking Wi-Fi state machine
│   ├── CameraManager.h             # OV3660 init/capture/ISP profile/controls
│   ├── AudioManager.h              # I2S init/capture/level stats
│   └── StreamServer.h              # Asynchronous HTTP & 30 FPS MJPEG stream engine
├── src/
│   └── main.cpp                    # setup, health loop, heartbeat
├── platformio_override.example.ini # template for local credentials
└── .gitignore                      # excludes platformio_override.ini
```

## FreeRTOS Task Architecture

| Task | Core | Priority | Stack | Purpose |
|------|------|----------|-------|---------|
| `loopTask` | 1 | 1 | 8 KB | Wi-Fi state machine, health monitoring, backend heartbeat, idle camera self-test |
| `audioTask` | 0 | 5 | 6144 B | Continuous I2S DMA reads (~16 ms chunks), RMS/peak level calculation |
| `httpd` | 0 | 5 | 8192 B | Asynchronous HTTP server, 1280x720 @ 30 FPS chunked MJPEG stream delivery |

No long `delay()` calls; all periodic work is `millis()`-gated so camera, audio,
and Wi-Fi remain responsive and watchdog-safe.

## HTTP Endpoints

| Endpoint | Port | Description |
|----------|------|-------------|
| `GET /` | 80 | HTML5 Greenhouse Live View dashboard with live FPS/bitrate HUD and controls |
| `GET /stream` | 80 & 81 | Real-time MJPEG live video stream (1280×720 @ 30 FPS target) |
| `GET /capture` | 80 | Full-resolution JPEG snapshot (`1280x720`) |
| `GET /status` | 80 | Comprehensive plain-text telemetry (stream FPS, bitrate, memory, audio, Wi-Fi) |
| `GET /control` | 80 | Dynamic parameter control (e.g. `/control?var=quality&val=10`) |

## GPIO Allocation

### Camera Bus (OV3660 / OV2640, Freenove-Compatible Layout)

| Signal | GPIO | Signal | GPIO |
|--------|------|--------|------|
| XCLK   | 15   | D7 (Y9) | 16 |
| SIOD (SCCB SDA) | 4 | D6 (Y8) | 17 |
| SIOC (SCCB SCL) | 5 | D5 (Y7) | 18 |
| VSYNC  | 6    | D4 (Y6) | 12 |
| HREF   | 7    | D3 (Y5) | 10 |
| PCLK   | 13   | D2 (Y4) | 8  |
| PWDN   | – (not wired) | D1 (Y3) | 9 |
| RESET  | – (not wired) | D0 (Y2) | 11 |

### INMP441 I2S Microphone Wiring

| INMP441 pin | ESP32-S3 GPIO | Notes |
|-------------|---------------|-------|
| VDD         | 3V3           | 1.8–3.3 V supply rail |
| GND         | GND           | Common ground |
| SCK (BCLK)  | GPIO 2        | Bit clock |
| WS (LRCLK)  | GPIO 1        | Word select |
| SD (DATA)   | GPIO 14       | Mic data out → MCU data in |
| L/R         | GND           | LEFT slot selection (mono-left config) |

## Building and Flashing

```bash
cd proleaf-camera

# Option A: environment variables
export LEAF_WIFI_SSID="your-ssid"
export LEAF_WIFI_PASSWORD="your-password"
export LEAF_CAMERA_DEVICE_ID="esp32-cam-001"
export LEAF_CAMERA_DEVICE_NAME="Greenhouse 1 Camera"
pio run -t upload && pio device monitor

# Option B: local override file (gitignored)
cp platformio_override.example.ini platformio_override.ini
$EDITOR platformio_override.ini   # fill in real SSID/password
pio run -t upload && pio device monitor
```

Serial monitor: **115200 baud**.

### Expected Boot Output

```
================================================
 Project L.E.A.F. Greenhouse Camera Firmware   
================================================
Firmware: 0.1.0
Board: ESP32-S3-CAM N16R8 (16MB Flash, 8MB PSRAM)
Camera: OV3660 (1280x720 HD @ 30 FPS target)
INMP441: I2S MEMS Microphone (16 kHz, 24-bit)
WiFi: your-ssid
Device: esp32-cam-001 (Greenhouse 1 Camera)
================================================
[BOOT] SDK: v... | CPU freq: 240 MHz
[BOOT] Flash: 16384KB | Heap: 280KB free | PSRAM: 8192KB free
[AUDIO] Initializing INMP441...
[AUDIO] I2S initialized: 16000 Hz, 32-bit slots, mono LEFT, DMA 8x256
[BOOT] Audio subsystem started
[CAM] Initializing camera subsystem...
[CAM] Octal PSRAM detected: 8192 KB total, 8190 KB free
[CAM] Sensor identified: OV3660 (3MP) (PID: 0x3660)
[CAM] Applying OV3660 Greenhouse ISP Tuning Profile...
[CAM] Frame config: 1280x720 (HD) JPEG q=12, 3 PSRAM framebuffer(s)
[CAM] First frame captured: 48512 bytes (31ms)
[CAM] Subsystem ready for 1280x720 live streaming
[NET] Connecting to WiFi (SSID: ..., attempt 1)...
[NET] Connected
[NET] IP: 192.168.1.105
[HTTP] Starting asynchronous HTTP server on port 80 and stream port 81...
[HTTP] Primary HTTP server active on http://192.168.1.105:80/ (/, /capture, /status, /stream, /control)
[BOOT] Boot SUCCESS — camera:ok audio:ok wifi:connected
```

### Expected Stream Diagnostics Output

When a client connects to `http://<device-ip>/stream` or opens the Web UI:

```
[STREAM] Client connected (Active viewers: 1). Streaming 1280x720 @ 30 FPS target...
[STREAM] 1280x720 @ 29.6 FPS (Capture: 30.0 FPS) | 1452.4 KB/s (11.62 Mbps) | Avg: 49.1 KB | Latency: 29ms | q=12 | Viewers: 1
[STREAM] 1280x720 @ 29.8 FPS (Capture: 30.0 FPS) | 1466.8 KB/s (11.73 Mbps) | Avg: 49.2 KB | Latency: 28ms | q=12 | Viewers: 1
[STREAM] 1280x720 @ 30.0 FPS (Capture: 30.0 FPS) | 1475.1 KB/s (11.80 Mbps) | Avg: 49.2 KB | Latency: 28ms | q=12 | Viewers: 1
```

## Performance & Reliability Highlights

- **30 FPS 720p HD with Adaptive Timing**: Sub-millisecond interval pacing eliminates timing jitter while preventing lag buildup.
- **Graceful Fallback**: If network bandwidth is restricted, frame capture and delivery gracefully settle at 20–25 FPS without buffer stalls or tearing.
- **Zero Loop-Task Starvation**: Dedicated FreeRTOS HTTP server keeps Core 1 `loop()` completely free for Wi-Fi management, backend heartbeat, and sensor/MQTT automation.
- **No Dynamic Heap Allocation in Steady State**: All framebuffers reside in Octal PSRAM. Heap usage remains stable throughout long streaming sessions.

