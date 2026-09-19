#ifndef CONFIG_H
#define CONFIG_H

#include <driver/gpio.h>

// ============================================================
// Project L.E.A.F. Camera Firmware — Board Configuration
// Target: ESP32-S3-CAM N16R8 (ESP32-S3-WROOM-1, 16MB flash,
//         8MB octal PSRAM, OV3660 camera on DVP bus)
// Target Stream: 1280x720 (HD) @ 30 FPS MJPEG
// ============================================================

// ------------------------------------------------------------
// Device identity (independent from the main sensor device)
//   Greenhouse 1
//   ├─ Main Device:   esp32-001     (proleaf firmware)
//   └─ Camera Device: esp32-cam-001 (this firmware)
// ------------------------------------------------------------
#ifndef CAMERA_DEVICE_ID
#define CAMERA_DEVICE_ID "esp32-cam-001"
#endif

#ifndef CAMERA_DEVICE_NAME
#define CAMERA_DEVICE_NAME "Greenhouse 1 Camera"
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.1.0"
#endif

#ifndef SERIAL_BAUD
#define SERIAL_BAUD 115200
#endif

// ------------------------------------------------------------
// Wi-Fi (credentials injected at build time; never in source)
// ------------------------------------------------------------
#ifndef WIFI_SSID
#define WIFI_SSID "abc"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "kerraker"
#endif

#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 15000UL
#endif

#ifndef WIFI_STATUS_LOG_MS
#define WIFI_STATUS_LOG_MS 3000UL
#endif

#ifndef WIFI_RETRY_BASE_MS
#define WIFI_RETRY_BASE_MS 2000UL
#endif

#ifndef WIFI_RETRY_MAX_MS
#define WIFI_RETRY_MAX_MS 30000UL
#endif

#ifndef WIFI_RADIO_RESET_AFTER_FAILS
#define WIFI_RADIO_RESET_AFTER_FAILS 4
#endif

// ------------------------------------------------------------
// Camera pin map — ESP32-S3-CAM N16R8 (Freenove-compatible layout)
//
// IMPORTANT: third-party "ESP32-S3-CAM" boards are not fully
// standardized. This mapping is the de-facto standard used by the
// Freenove ESP32-S3 WROOM CAM design and most of its clones
// (verified against esp32-camera's BOARD_ESP32S3_WROOM default and
// multiple published N16R8 CAM board references).
// If your exact board does not match, override any pin below via
// build flags instead of editing source.
//
// Occupied by the camera bus:      GPIO 4-13, 15-18
// Occupied by OPI PSRAM (module):  GPIO 33-37 (do not use)
// SD card slot (if populated):     GPIO 38 (CMD), 39 (CLK), 40 (D0)
// USB D-/D+:                       GPIO 19, 20
// UART0 TX/RX:                     GPIO 43, 44
// Strapping pins (avoid):          GPIO 0, 3, 45, 46
// ------------------------------------------------------------
#ifndef CAM_PIN_PWDN
#define CAM_PIN_PWDN -1                 // not wired on this board
#endif
#ifndef CAM_PIN_RESET
#define CAM_PIN_RESET -1                // not wired on this board
#endif
#ifndef CAM_PIN_XCLK
#define CAM_PIN_XCLK 15
#endif
#ifndef CAM_PIN_SIOD
#define CAM_PIN_SIOD 4                  // SCCB SDA
#endif
#ifndef CAM_PIN_SIOC
#define CAM_PIN_SIOC 5                  // SCCB SCL
#endif
#ifndef CAM_PIN_D7
#define CAM_PIN_D7 16                   // Y9
#endif
#ifndef CAM_PIN_D6
#define CAM_PIN_D6 17                   // Y8
#endif
#ifndef CAM_PIN_D5
#define CAM_PIN_D5 18                   // Y7
#endif
#ifndef CAM_PIN_D4
#define CAM_PIN_D4 12                   // Y6
#endif
#ifndef CAM_PIN_D3
#define CAM_PIN_D3 10                   // Y5
#endif
#ifndef CAM_PIN_D2
#define CAM_PIN_D2 8                    // Y4
#endif
#ifndef CAM_PIN_D1
#define CAM_PIN_D1 9                    // Y3
#endif
#ifndef CAM_PIN_D0
#define CAM_PIN_D0 11                   // Y2
#endif
#ifndef CAM_PIN_VSYNC
#define CAM_PIN_VSYNC 6
#endif
#ifndef CAM_PIN_HREF
#define CAM_PIN_HREF 7
#endif
#ifndef CAM_PIN_PCLK
#define CAM_PIN_PCLK 13
#endif

// Camera Clock Frequency: 20MHz generates stable 30 FPS for 720p HD on OV3660
#ifndef CAMERA_XCLK_FREQ_HZ
#define CAMERA_XCLK_FREQ_HZ 20000000
#endif

// Resolution: 1280x720 (FRAMESIZE_HD) — OV3660 native HD, 16:9 for the
// live dashboard player. The earlier VGA downgrade was a TCP-window
// workaround for the WebSocket relay; video now rides UDP/RTP.
#ifndef CAMERA_FRAME_SIZE
#define CAMERA_FRAME_SIZE FRAMESIZE_HD
#endif

// PSRAM frame buffers. Two avoided tearing/ViS and left headroom; with 8MB PSRAM
// available we run THREE so the ISP/DMA can always be writing the next frame while
// the caller still holds the freshest one. Combined with CAMERA_GRAB_LATEST this
// guarantees esp_camera_fb_get() never stalls waiting for a free buffer — a
// proven 30-60ms latency sink in high-throughput streaming.
#ifndef CAMERA_FB_COUNT
#define CAMERA_FB_COUNT 3
#endif

// JPEG Quality: 0-63, lower = better (more bytes).
//
// q40 keeps a 1280x720 frame small enough (~12-16 KB) to fit the real
// uplink budget. With frame pacing (see RtpPusher.h) the ESP32 can push
// ~225 datagrams/s (~2.5 Mbps); going larger than that (q25 HD was ~37 KB
// per frame -> 6 Mbps) collapses the stream with lwIP ENOMEM + MediaMTX
// "RTP packets lost". Video rides UDP/RTP, so the old q40 VGA TCP-window
// compromise is obsolete — but quality still has to respect the real
// serial link.
#ifndef CAMERA_JPEG_QUALITY
#define CAMERA_JPEG_QUALITY 40
#endif

#ifndef CAMERA_TEST_INTERVAL_MS
#define CAMERA_TEST_INTERVAL_MS 10000UL
#endif

// Warm-up retries for the first frame after init; sensors freshly powered
// (OV3660 in particular) can take a moment before the first JPEG arrives.
#ifndef CAMERA_FIRST_FRAME_ATTEMPTS
#define CAMERA_FIRST_FRAME_ATTEMPTS 12
#endif

#ifndef CAMERA_FIRST_FRAME_RETRY_DELAY_MS
#define CAMERA_FIRST_FRAME_RETRY_DELAY_MS 200
#endif

// How often the self-test retries recovery when the camera never came up.
#ifndef CAMERA_RECOVER_INTERVAL_MS
#define CAMERA_RECOVER_INTERVAL_MS 5000UL
#endif

// Image orientation (0 = normal, 1 = inverted/flipped)
#ifndef CAMERA_VFLIP
#define CAMERA_VFLIP 0
#endif

#ifndef CAMERA_HMIRROR
#define CAMERA_HMIRROR 0
#endif

// ------------------------------------------------------------
// OV3660 Advanced Image Quality Settings (Greenhouse Optimization)
// ------------------------------------------------------------
#ifndef OV3660_BRIGHTNESS
#define OV3660_BRIGHTNESS 1             // -2 to 2: +1 gives clear ambient illumination
#endif

#ifndef OV3660_CONTRAST
#define OV3660_CONTRAST 1               // -2 to 2: +1 enhances foliage edge definition
#endif

#ifndef OV3660_SATURATION
#define OV3660_SATURATION 0             // -2 to 2: 0 for true-to-life leaf colors
#endif

#ifndef OV3660_SHARPNESS
#define OV3660_SHARPNESS 0              // -2 to 2
#endif

#ifndef OV3660_DENOISE
#define OV3660_DENOISE 1                // 0 or 1: filter sensor noise
#endif

// ------------------------------------------------------------
// INMP441 I2S MEMS microphone
//
// Selected GPIOs are free under BOTH known ESP32-S3-CAM vendor
// layouts (camera-on-GPIO4..18 and camera-on-GPIO40..48 variants),
// so the mic wiring stays valid even if the camera pinout differs:
//   I2S WS   -> GPIO 1
//   I2S BCLK -> GPIO 2
//   I2S SD   -> GPIO 14
// L/R pin tied to GND selects the LEFT slot (see AudioManager).
// ------------------------------------------------------------
#ifndef INMP441_WS_PIN
#define INMP441_WS_PIN 1
#endif
#ifndef INMP441_SCK_PIN
#define INMP441_SCK_PIN 2
#endif
#ifndef INMP441_SD_PIN
#define INMP441_SD_PIN 14
#endif

#ifndef AUDIO_I2S_PORT
#define AUDIO_I2S_PORT I2S_NUM_0
#endif

#ifndef AUDIO_SAMPLE_RATE_HZ
#define AUDIO_SAMPLE_RATE_HZ 16000
#endif

#ifndef AUDIO_BITS_PER_SAMPLE
#define AUDIO_BITS_PER_SAMPLE 32        // INMP441 delivers 24-bit data in a 32-bit slot
#endif

#ifndef AUDIO_DMA_BUF_COUNT
#define AUDIO_DMA_BUF_COUNT 8
#endif

#ifndef AUDIO_DMA_BUF_LEN_SAMPLES
#define AUDIO_DMA_BUF_LEN_SAMPLES 256   // frames per DMA buffer
#endif

#ifndef AUDIO_READ_SAMPLES
#define AUDIO_READ_SAMPLES 256          // samples per task read (~16ms @16kHz)
#endif

#ifndef AUDIO_RING_BUFFER_SAMPLES
#define AUDIO_RING_BUFFER_SAMPLES 8192  // 512ms mono PCM playback buffer
#endif

#ifndef AUDIO_REPORT_INTERVAL_MS
#define AUDIO_REPORT_INTERVAL_MS 500UL
#endif

// RMS below this (24-bit scale) is treated as silence for the
// "valid audio" check. Relative amplitude only — NOT calibrated dB SPL.
#ifndef AUDIO_SILENCE_RMS_THRESHOLD
#define AUDIO_SILENCE_RMS_THRESHOLD 50
#endif

// ------------------------------------------------------------
// HTTP & MJPEG Stream Configuration
// ------------------------------------------------------------
#ifndef CAMERA_HTTP_PORT
#define CAMERA_HTTP_PORT 80             // Web UI, capture, status, control
#endif

#ifndef CAMERA_STREAM_PORT
#define CAMERA_STREAM_PORT 81           // Dedicated MJPEG stream port
#endif

#ifndef CAMERA_AUDIO_PORT
#define CAMERA_AUDIO_PORT 82            // Dedicated PCM audio stream port
#endif

// Target 30 FPS live streaming with adaptive interval pacing
#ifndef CAMERA_STREAM_TARGET_FPS
#define CAMERA_STREAM_TARGET_FPS 30
#endif

// Graceful fallback floor if Wi-Fi throughput or client lags
#ifndef CAMERA_STREAM_MIN_FPS
#define CAMERA_STREAM_MIN_FPS 20
#endif

// Real-time FPS serial log interval (1 second)
#ifndef CAMERA_STREAM_REPORT_INTERVAL_MS
#define CAMERA_STREAM_REPORT_INTERVAL_MS 1000UL
#endif

// Hard cap per stream session. Prevents orphaned connections from leaking.
#ifndef CAMERA_STREAM_MAX_SESSION_MS
#define CAMERA_STREAM_MAX_SESSION_MS (30UL * 60UL * 1000UL)
#endif

// ------------------------------------------------------------
// Backend integration (Laravel device API)
//
// Set LEAF_BACKEND_URL (e.g. http://192.168.1.10:8000) and
// LEAF_CAMERA_DEVICE_TOKEN (issued by `php artisan leaf:provision-camera`)
// at build time. Empty values disable the heartbeat; the camera keeps
// serving /stream on the LAN regardless.
// ------------------------------------------------------------
#ifndef LEAF_BACKEND_URL
#define LEAF_BACKEND_URL ""
#endif

#ifndef LEAF_CAMERA_DEVICE_TOKEN
#define LEAF_CAMERA_DEVICE_TOKEN ""
#endif

#ifndef BACKEND_HEARTBEAT_INTERVAL_MS
#define BACKEND_HEARTBEAT_INTERVAL_MS 15000UL
#endif

// ------------------------------------------------------------
// Cloud relay (outbound WSS to VPS for remote live view)
//
// RELAY_HOST is the VPS hostname the camera connects to via
// WebSocket. Empty string disables the relay entirely; the
// camera keeps serving /stream on the LAN regardless.
// ------------------------------------------------------------
#ifndef RELAY_HOST
#define RELAY_HOST ""
#endif

#ifndef RELAY_PORT
#define RELAY_PORT 443
#endif

// Reconnect delay after a WebSocket drop. Fast enough that the live view
// resumes within ~1.5s of a blip, slow enough not to hammer the relay while
// the (lossy) uplink is down.
#ifndef RELAY_RECONNECT_BASE_MS
#define RELAY_RECONNECT_BASE_MS 1000UL
#endif

#ifndef RELAY_RECONNECT_MAX_MS
#define RELAY_RECONNECT_MAX_MS 30000UL
#endif

// Maximum FPS the ESP32 pushes to the relay. Kept at the camera's native
// rate for smooth realtime live view: frames are already small (VGA JPEG),
// and relay streaming only happens while a viewer is attached, so the
// uplink stays comfortably within capacity (RSSI is typically excellent).
#ifndef RELAY_TARGET_FPS
#define RELAY_TARGET_FPS 30
#endif

// Blocking timeout per WebSocket send. Kept short so a stalled uplink can
// never freeze the loopTask (Wi-Fi manager + heartbeat + reconnect handling).
// Max time a WS send() may block the loopTask. Deliberately short: when the
// uplink is momentarily saturated, we would rather DROP a frame (send() fails,
// next loop picks up a fresh one) than stall the loopTask and risk the task
// watchdog or a latency pile-up. Real-time view = newest frames, not queued ones.
#ifndef RELAY_SEND_TIMEOUT_MS
#define RELAY_SEND_TIMEOUT_MS 300UL
#endif

// Set to 1 to disable TLS certificate verification (not recommended
// for production, but useful when the VPS uses self-signed certs).
#ifndef RELAY_SKIP_TLS_VERIFY
#define RELAY_SKIP_TLS_VERIFY 0
#endif

// FreeRTOS task for relay audio reads (runs on Core 0 with HTTPD)
#ifndef RELAY_TASK_STACK_SIZE
#define RELAY_TASK_STACK_SIZE 8192
#endif

#ifndef RELAY_TASK_PRIORITY
#define RELAY_TASK_PRIORITY 3
#endif

#ifndef RELAY_TASK_CORE
#define RELAY_TASK_CORE 0
#endif

// Audio chunk size sent over relay (512 samples = 32 ms at 16 kHz)
#ifndef RELAY_AUDIO_CHUNK_SAMPLES
#define RELAY_AUDIO_CHUNK_SAMPLES 512
#endif

// ------------------------------------------------------------
// RTP/JPEG cloud push (low-latency UDP to VPS MediaMTX ingest)
//
// The camera pushes RFC2435 MJPEG over UDP straight to the VPS, where
// MediaMTX (`udp+rtp://0.0.0.0:8000`) ingests it and re-exposes it over
// WebRTC for the dashboard. Unlike the WebSocket relay (which must fight
// the LwIP 5760-byte TCP send window over a ~900ms RTT link), RTP/UDP is
// connectionless: frames are fire-and-forget, newest-wins, and bypass the
// TCP congestion window entirely. Empty RTP_RELAY_HOST disables the pusher.
// ------------------------------------------------------------
#ifndef RTP_RELAY_HOST
#define RTP_RELAY_HOST "187.127.213.71"
#endif

#ifndef RTP_RELAY_PORT
#define RTP_RELAY_PORT 8000
#endif

// Nominal video FPS for the RTP push. The pusher captures at most this
// often and always sends the freshest frame (no queue), mirroring the
// relay's drop-on-backlog philosophy. 15 FPS matches the measured liftable
// uplink budget (~2.5 Mbps => ~35 KB of JPEG per frame at HD, before TCP
// send-buffer/driver retries eat the rest).
#ifndef RTP_TARGET_FPS
#define RTP_TARGET_FPS 15
#endif

// Cap of JPEG payload per RTP packet. Matches MediaMTX/gortsplib's default
// payload max (udpMaxPayloadSize: 1440) so packets arrive without server-side
// remuxing, and stays below the 1472-byte unfragmented-UDP limit.
#ifndef RTP_PAYLOAD_MAX
#define RTP_PAYLOAD_MAX 1440
#endif

// ------------------------------------------------------------
// Health monitoring
// ------------------------------------------------------------
#ifndef HEALTH_REPORT_INTERVAL_MS
#define HEALTH_REPORT_INTERVAL_MS 15000UL
#endif

// If the ESP32 has WiFi but neither the relay WebSocket nor the backend
// heartbeat has succeeded for this long, reboot to recover a wedged
// network stack (mbedTLS/LWIP). The task watchdog catches hard hangs;
// this catches the quieter "connected but dead" case.
#ifndef RECOVERY_REBOOT_AFTER_MS
#define RECOVERY_REBOOT_AFTER_MS (15UL * 60UL * 1000UL)
#endif

// ------------------------------------------------------------
// FreeRTOS tasks (documented per reliability requirements)
//
// | Task       | Core | Priority | Stack | Purpose                          |
// |------------|------|----------|-------|----------------------------------|
// | loopTask   | 1    | 1        | 8KB   | Wi-Fi state machine, health,     |
// |            |      |          |       | backend heartbeat, idle self-test|
// | audioTask  | 0    | 5        | 6144B | Continuous non-blocking I2S DMA  |
// |            |      |          |       | reads, level calculation         |
// | httpd      | 0    | 5        | 8192B | Non-blocking HTTP & MJPEG stream |
// ------------------------------------------------------------
#ifndef AUDIO_TASK_STACK_SIZE
#define AUDIO_TASK_STACK_SIZE 6144
#endif

#ifndef AUDIO_TASK_PRIORITY
#define AUDIO_TASK_PRIORITY 5
#endif

#ifndef AUDIO_TASK_CORE
#define AUDIO_TASK_CORE 1
#endif

#ifndef HTTPD_TASK_STACK_SIZE
#define HTTPD_TASK_STACK_SIZE 8192
#endif

#ifndef HTTPD_TASK_PRIORITY
#define HTTPD_TASK_PRIORITY 5
#endif

#ifndef HTTPD_TASK_CORE
#define HTTPD_TASK_CORE 0
#endif

#endif // CONFIG_H
