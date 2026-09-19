#include <Arduino.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include "Config.h"
#include "TlsCertificates.h"
#include "NetworkManager.h"
#include "CameraManager.h"
#include "AudioManager.h"
#include "StreamServer.h"
#include "RelayClient.h"
#include "RtpPusher.h"

static NetworkManager net;
static CameraManager cam;
static AudioManager audio;
static StreamServer streamServer(&cam, &audio, &net);
static RelayClient relay(&cam, &audio, &net);
static RtpPusher rtpPusher(&cam);

static uint32_t g_lastHealthReport = 0;
static uint32_t g_lastHeartbeatMs = 0;
static bool g_lastHeartbeatOk = true;
static uint32_t g_lastGoodMs = 0;

// ------------------------------------------------------------------
// Web server lifecycle: start once Wi-Fi is connected
// ------------------------------------------------------------------
static void startWebServerIfDue() {
    if (streamServer.isStarted() || !net.isConnected()) {
        return;
    }
    streamServer.begin();
    relay.begin();
    relay.setVideoSuppressed(true);
}

// ------------------------------------------------------------------
// Backend heartbeat (Laravel device API)
//
// POST /api/devices/heartbeat with the provisioned bearer token so the
// dashboard can resolve this camera's LAN address for live view.
// ------------------------------------------------------------------
static void maybeSendHeartbeat(uint32_t now) {
    if (strlen(LEAF_BACKEND_URL) == 0 || strlen(LEAF_CAMERA_DEVICE_TOKEN) == 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Serial.println("[BE] Heartbeat disabled (build with LEAF_BACKEND_URL + LEAF_CAMERA_DEVICE_TOKEN)");
        }
        return;
    }
    if (!net.isConnected()) {
        return; // don't consume the interval while offline; fires right after reconnect
    }
    if (g_lastHeartbeatMs != 0 && now - g_lastHeartbeatMs < BACKEND_HEARTBEAT_INTERVAL_MS) {
        return;
    }
    g_lastHeartbeatMs = now;

    // Pause heartbeats while actively streaming to the relay: the blocking
    // HTTPS POST would stall the loopTask (and thus frame capture) for up to
    // ~1.5s, which turns into visible live-view freezes. Presence is still
    // signalled by the live WebSocket itself.
    if (relay.isStreaming()) {
        return;
    }

    HTTPClient http;
    const String url = String(LEAF_BACKEND_URL) + "/api/devices/heartbeat";
    // Verify the backend HTTPS cert with the same ISRG Root X1 trust anchor
    // used by the relay (begin(url, CAcert)). Without it the core silently
    // falls back to setInsecure().
    if (!http.begin(url, LEAF_RELAY_TLS_CA_PEM)) {
        Serial.println("[BE] Heartbeat failed: cannot begin request");
        g_lastHeartbeatOk = false;
        return;
    }
    // Keep the heartbeat cheap: a stalled TLS/HTTP call must never freeze the
    // loopTask for seconds (that delayed Wi-Fi/reconnect handling and made
    // stream drops far worse).
    http.setConnectTimeout(1500);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " LEAF_CAMERA_DEVICE_TOKEN);

    AudioManager::Stats as = audio.stats();
    StreamServer::StreamMetrics sm = streamServer.getMetrics();
    String body = "{\"device_id\":\"" CAMERA_DEVICE_ID "\","
                  "\"type\":\"camera\","
                  "\"name\":\"" CAMERA_DEVICE_NAME "\","
                  "\"firmware_version\":\"" FIRMWARE_VERSION "\","
                  "\"local_ip_address\":\"" + net.ip().toString() + "\","
                  "\"wifi_rssi\":" + String(static_cast<int>(net.rssi())) + ","
                  "\"uptime_seconds\":" + String(millis() / 1000UL) + ","
                  "\"free_heap\":" + String(ESP.getFreeHeap()) + ","
                  "\"free_psram\":" + String(ESP.getFreePsram()) + ","
                  "\"stream_fps\":" + String(sm.streamFps, 1) + ","
                  "\"stream_active\":" + String(sm.isStreaming ? "true" : "false") + ","
                  "\"sound_db_spl\":" + String(as.soundDbSpl, 1) + "}";

    const int code = http.POST(body);
    http.end();

    const bool ok = code == 200;
    if (ok) {
        g_lastGoodMs = millis(); // any successful uplink counts as "healthy"
    }
    if (ok != g_lastHeartbeatOk) {
        Serial.printf("[BE] Heartbeat %s (HTTP %d)\n", ok ? "sent" : "FAILED", code);
    } else if (!ok && code < 0) {
        Serial.printf("[BE] Heartbeat error: %s\n", HTTPClient::errorToString(code).c_str());
    }
    g_lastHeartbeatOk = ok;
}

// ------------------------------------------------------------------
// Runtime health report
// ------------------------------------------------------------------
static void reportHealth(uint32_t now) {
    if (now - g_lastHealthReport < HEALTH_REPORT_INTERVAL_MS) {
        return;
    }
    g_lastHealthReport = now;
    AudioManager::Stats a = audio.stats();
    StreamServer::StreamMetrics sm = streamServer.getMetrics();

    if (sm.isStreaming) {
        Serial.printf("[SYS] Health: uptime=%lus heap=%uKB(min %uKB) psram=%uKB "
                      "rssi=%ddBm cam[ok=%u fail=%u] stream[%.1f FPS | %.2f Mbps] audio[%s lvl=%u spl=%.1fdB]\n",
                      static_cast<unsigned long>(now / 1000),
                      static_cast<unsigned>(ESP.getFreeHeap() / 1024),
                      static_cast<unsigned>(ESP.getMinFreeHeap() / 1024),
                      static_cast<unsigned>(ESP.getFreePsram() / 1024),
                      net.isConnected() ? static_cast<int>(net.rssi()) : 0,
                      cam.captureCount(), cam.failCount(),
                      sm.streamFps, sm.bitrateKbps / 1000.0f,
                      a.receiving ? "OK" : "SILENT",
                      a.rmsLevel,
                      a.soundDbSpl);
    } else {
        Serial.printf("[SYS] Health: uptime=%lus heap=%uKB(min %uKB) psram=%uKB "
                      "rssi=%ddBm cam[ok=%u fail=%u] stream[idle] audio[%s lvl=%u spl=%.1fdB]\n",
                      static_cast<unsigned long>(now / 1000),
                      static_cast<unsigned>(ESP.getFreeHeap() / 1024),
                      static_cast<unsigned>(ESP.getMinFreeHeap() / 1024),
                      static_cast<unsigned>(ESP.getFreePsram() / 1024),
                      net.isConnected() ? static_cast<int>(net.rssi()) : 0,
                      cam.captureCount(), cam.failCount(),
                      a.receiving ? "OK" : "SILENT",
                      a.rmsLevel,
                      a.soundDbSpl);
    }
}

// ------------------------------------------------------------------
void printBanner() {
    Serial.println();
    Serial.println("================================================");
    Serial.println(" Project L.E.A.F. Greenhouse Camera Firmware   ");
    Serial.println("================================================");
    Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);
    Serial.println("Board: ESP32-S3-CAM N16R8 (16MB Flash, 8MB PSRAM)");
    Serial.println("Camera: OV3660 (1280x720 HD JPEG @ q15, 3x PSRAM FB, GRAB_LATEST)");
    Serial.println("INMP441: I2S MEMS Microphone (16 kHz, 24-bit)");
    Serial.printf("RTP:    %s:%d @ %u FPS %s\n",
                  (strlen(RTP_RELAY_HOST) > 0) ? RTP_RELAY_HOST : "(disabled)",
                  (int)RTP_RELAY_PORT, (unsigned)RTP_TARGET_FPS,
                  (strlen(RTP_RELAY_HOST) > 0) ? "(RFC2435 MJPEG over UDP)" : "");
    Serial.printf("WiFi: %s\n",
                  strlen(WIFI_SSID) > 0 ? WIFI_SSID : "(not configured)");
    Serial.printf("Device: %s (%s)\n", CAMERA_DEVICE_ID, CAMERA_DEVICE_NAME);
    Serial.printf("Backend: %s\n",
                  (strlen(LEAF_BACKEND_URL) > 0 && strlen(LEAF_CAMERA_DEVICE_TOKEN) > 0)
                      ? LEAF_BACKEND_URL
                      : "(heartbeat disabled)");
    Serial.println("================================================");
}

// Rebooting after a long silent stretch (WiFi up but nothing reaching the
// VPS) clears wedged mbedTLS/LWIP state a plain reconnect can't. Gated on
// BOTH the relay and the heartbeat failing so a server-side outage doesn't
// make the camera thrash — it only self-heals a genuinely stuck network stack.
static void maybeRecoveryReboot(uint32_t now) {
    if (!net.isConnected()) return;
    const uint32_t lastGood = max(g_lastGoodMs, relay.lastConnectedMs());
    if (lastGood == 0 || now - lastGood < RECOVERY_REBOOT_AFTER_MS) return;
    Serial.printf("[SYS] No uplink success for %lus — rebooting to recover network stack\n",
                  static_cast<unsigned long>((now - lastGood) / 1000));
    esp_restart();
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    const uint32_t serialStart = millis();
    while (!Serial && millis() - serialStart < 2000) {
        delay(10); // short bounded wait so the banner is visible over USB CDC
    }
    printBanner();

    // Task watchdog: if loopTask ever blocks (stalled camera DMA, hung TLS
    // handshake, dead WebSocket send) the SoC reboots instead of going dark
    // until a manual power cycle. This is what kept the camera offline for
    // ~19h on Sep 13 — no recovery mechanism existed.
    esp_err_t wdtErr = esp_task_wdt_init(15, true);
    if (wdtErr == ESP_OK || wdtErr == ESP_ERR_INVALID_STATE) {
        if (esp_task_wdt_add(NULL) == ESP_OK) {
            Serial.println("[BOOT] Task watchdog armed (15s)");
        } else {
            Serial.println("[BOOT] Task watchdog: unable to subscribe loop task");
        }
    } else {
        Serial.printf("[BOOT] Task watchdog failed: %s\n", esp_err_to_name(wdtErr));
    }

    Serial.printf("[BOOT] SDK: %s | CPU freq: %u MHz\n",
                  ESP.getSdkVersion(), ESP.getCpuFreqMHz());
    Serial.printf("[BOOT] Flash: %uKB | Heap: %uKB free | PSRAM: %uKB free\n",
                  static_cast<unsigned>(ESP.getFlashChipSize() / 1024),
                  static_cast<unsigned>(ESP.getFreeHeap() / 1024),
                  static_cast<unsigned>(ESP.getFreePsram() / 1024));

    // INMP441 first: its task runs continuously on Core 0
    if (audio.begin()) {
        Serial.println("[BOOT] Audio subsystem started");
    } else {
        Serial.println("[BOOT] Audio subsystem FAILED — continuing without it");
    }

    // Camera subsystem: 1280x720 HD JPEG, 3x PSRAM framebuffers, OV3660 ISP profile
    if (!cam.begin()) {
        Serial.println("[BOOT] Camera init FAILED — continuing without camera");
    }

    // Wi-Fi subsystem: non-blocking state machine
    net.begin();

    // Low-latency RTP/JPEG push to the VPS (MediaMTX ingest)
    rtpPusher.begin();

    bool wifiUp = net.isConnected();
    bool bootOk = cam.isReady() && audio.stats().initialized;
    Serial.printf("[BOOT] Boot %s — camera:%s audio:%s wifi:%s rtp:%s\n",
                  bootOk ? "SUCCESS" : "DEGRADED",
                  cam.isReady() ? "ok" : "FAIL",
                  audio.stats().initialized ? "ok" : "FAIL",
                  wifiUp ? "connected" : (net.hasCredentials() ? "connecting" : "no-credentials"),
                  rtpPusher.isActive() ? "enabled" : "disabled");
}

void loop() {
    uint32_t now = millis();
    esp_task_wdt_reset(); // feed the watchdog every iteration

    net.loop();
    startWebServerIfDue();

    // Low-latency RTP/JPEG push (always on when configured)
    rtpPusher.loop();

    // Run idle camera self-test only when no active stream is running —
    // neither LAN, relay, nor the RTP push — avoiding pipeline contention
    // during live realtime streaming (a competing esp_camera_fb_get adds
    // frame jitter). RTP captures continuously, so the self-test stays off
    // while the pusher is armed.
    if (!streamServer.isStreamingActive() && !relay.isStreaming() && !rtpPusher.isActive()) {
        cam.periodicSelfTest(now);
    }

    relay.loop();

    // Refresh now after the loops above — they take time and may call
    // millis() internally, setting timestamps (lastSendMs, lastConnectedMs)
    // that are slightly *ahead* of the `now` captured at loop entry.
    // Using the stale `now` makes (now - lastGood) wrap to 0xFFFFFFFF and
    // triggers the recovery reboot within one iteration.
    now = millis();

    // A live relay WebSocket or a successful RTP send means the uplink is
    // healthy; carry that forward so a long stream never triggers the
    // recovery reboot or a stale-heartbeat stall.
    if (relay.isStreaming()) {
        g_lastGoodMs = now;
    }
    const uint32_t rtpLast = rtpPusher.lastSendMs();
    if (rtpLast != 0 && rtpLast > g_lastGoodMs) {
        g_lastGoodMs = rtpLast;
    }

    maybeRecoveryReboot(now);
    audio.reportIfDue(now);
    maybeSendHeartbeat(now);
    reportHealth(now);

    // No delay(): the loop yields to FreeRTOS scheduler each iteration.
    // Streaming runs in its own FreeRTOS task, completely isolating loopTask.
}
