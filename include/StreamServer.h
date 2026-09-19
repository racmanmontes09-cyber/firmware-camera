#ifndef STREAM_SERVER_H
#define STREAM_SERVER_H

#include <Arduino.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "Config.h"
#include "CameraManager.h"
#include "AudioManager.h"
#include "NetworkManager.h"

// High-performance, non-blocking HTTP and MJPEG streaming server for ESP32-S3-CAM N16R8.
//
// Architecture:
//  - Built on ESP-IDF's esp_http_server running in a dedicated FreeRTOS background task.
//  - Port 80 (camera_httpd): Web UI (/), snapshot (/capture), telemetry (/status), control (/control), and stream (/stream).
//  - Port 81 (stream_httpd): Dedicated high-throughput MJPEG streaming endpoint (/stream).
//  - Pacing: Sub-millisecond adaptive pacing targeting 30.0 FPS at 1280x720 HD with graceful fallback to 20-25 FPS.
//  - Zero Loop-Task Contention: NetworkManager, AudioManager, heartbeat, and sensor tasks run 100% unblocked.
//  - Diagnostic Reporting: Real-time measured capture/stream FPS, bandwidth, and frame sizes logged every second.
class StreamServer {
public:
    struct StreamMetrics {
        bool isStreaming = false;
        uint8_t activeClients = 0;
        float streamFps = 0.0f;
        float captureFps = 0.0f;
        float bitrateKbps = 0.0f;
        float avgFrameKb = 0.0f;
        uint32_t captureLatencyMs = 0;
        uint32_t totalFrames = 0;
        uint64_t totalBytes = 0;
        uint32_t droppedFrames = 0;
    };

    StreamServer(CameraManager* cam, AudioManager* audio, NetworkManager* net)
        : m_cam(cam), m_audio(audio), m_net(net) {}

    bool begin() {
        if (m_started) {
            return true;
        }

        Serial.printf("[HTTP] Starting asynchronous HTTP server on port %d and stream port %d...\n",
                      CAMERA_HTTP_PORT, CAMERA_STREAM_PORT);

        // 1. Primary HTTP Server (Port 80): Web UI, Capture, Status, Control, and Stream
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = CAMERA_HTTP_PORT;
        config.ctrl_port = 32768;
        config.max_open_sockets = 5;
        config.max_uri_handlers = 10;
        config.task_priority = HTTPD_TASK_PRIORITY;
        config.stack_size = HTTPD_TASK_STACK_SIZE;
        config.core_id = HTTPD_TASK_CORE;
        config.lru_purge_enable = true;
        config.recv_wait_timeout = 5;
        config.send_wait_timeout = 1;

        if (httpd_start(&m_cameraHttpd, &config) != ESP_OK) {
            Serial.println("[HTTP] ERROR: Failed to start primary HTTP server on port 80");
            return false;
        }

        // Register URI handlers on Port 80
        httpd_uri_t indexUri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = handleIndex,
            .user_ctx = this
        };
        httpd_register_uri_handler(m_cameraHttpd, &indexUri);

        httpd_uri_t captureUri = {
            .uri = "/capture",
            .method = HTTP_GET,
            .handler = handleCapture,
            .user_ctx = this
        };
        httpd_register_uri_handler(m_cameraHttpd, &captureUri);

        httpd_uri_t statusUri = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = handleStatus,
            .user_ctx = this
        };
        httpd_register_uri_handler(m_cameraHttpd, &statusUri);

        httpd_uri_t controlUri = {
            .uri = "/control",
            .method = HTTP_GET,
            .handler = handleControl,
            .user_ctx = this
        };
        httpd_register_uri_handler(m_cameraHttpd, &controlUri);

        httpd_uri_t streamUriPort80 = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = handleStream,
            .user_ctx = this
        };
        httpd_register_uri_handler(m_cameraHttpd, &streamUriPort80);

        // 2. Dedicated Stream Server (Port 81): Isolated high-bandwidth streaming
        config.server_port = CAMERA_STREAM_PORT;
        config.ctrl_port = 32769;
        config.max_open_sockets = 4;
        config.max_uri_handlers = 5;

        if (httpd_start(&m_streamHttpd, &config) == ESP_OK) {
            httpd_uri_t streamUriPort81 = {
                .uri = "/stream",
                .method = HTTP_GET,
                .handler = handleStream,
                .user_ctx = this
            };
            httpd_register_uri_handler(m_streamHttpd, &streamUriPort81);

            httpd_uri_t indexUriPort81 = {
                .uri = "/",
                .method = HTTP_GET,
                .handler = handleIndex,
                .user_ctx = this
            };
            httpd_register_uri_handler(m_streamHttpd, &indexUriPort81);

            Serial.printf("[HTTP] Dedicated MJPEG stream listening on http://%s:%d/stream\n",
                          m_net->ip().toString().c_str(), CAMERA_STREAM_PORT);
        } else {
            Serial.println("[HTTP] WARNING: Could not start port 81 stream server; streaming available on port 80");
        }

        // 3. Dedicated PCM audio server: wrapped in #if 0 to comply with RA 10173 data minimization
#if 0
        config.server_port = CAMERA_AUDIO_PORT;
        config.ctrl_port = 32770;
        config.max_open_sockets = 2;
        config.max_uri_handlers = 2;
        if (httpd_start(&m_audioHttpd, &config) == ESP_OK) {
            httpd_uri_t audioUri = {
                .uri = "/audio",
                .method = HTTP_GET,
                .handler = handleAudio,
                .user_ctx = this
            };
            httpd_register_uri_handler(m_audioHttpd, &audioUri);
            Serial.printf("[HTTP] Dedicated PCM audio listening on http://%s:%d/audio\n",
                          m_net->ip().toString().c_str(), CAMERA_AUDIO_PORT);
        } else {
            Serial.println("[HTTP] WARNING: Could not start dedicated PCM audio server");
        }
#endif

        m_started = true;
        Serial.printf("[HTTP] Primary HTTP server active on http://%s:%d/ (/, /capture, /status, /stream, /control)\n",
                      m_net->ip().toString().c_str(), CAMERA_HTTP_PORT);
        return true;
    }

    void stop() {
        if (m_cameraHttpd) {
            httpd_stop(m_cameraHttpd);
            m_cameraHttpd = nullptr;
        }
        if (m_streamHttpd) {
            httpd_stop(m_streamHttpd);
            m_streamHttpd = nullptr;
        }
        if (m_audioHttpd) {
            httpd_stop(m_audioHttpd);
            m_audioHttpd = nullptr;
        }
        m_started = false;
        Serial.println("[HTTP] Server stopped");
    }

    bool isStarted() const { return m_started; }

    StreamMetrics getMetrics() const {
        StreamMetrics out;
        portENTER_CRITICAL(&m_mux);
        out = m_metrics;
        portEXIT_CRITICAL(&m_mux);
        return out;
    }

    bool isStreamingActive() const {
        return m_metrics.isStreaming && (m_metrics.activeClients > 0);
    }

private:
    static const char* PART_BOUNDARY() {
        return "123456789000000000000987654321";
    }

    // ------------------------------------------------------------------
    // MJPEG Stream Handler (Asynchronous, adaptive pacing, 30 FPS target)
    // ------------------------------------------------------------------
    static esp_err_t handleStream(httpd_req_t* req) {
        StreamServer* self = static_cast<StreamServer*>(req->user_ctx);
        if (!self || !self->m_cam || !self->m_cam->isReady()) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        char contentTypeHeader[128];
        snprintf(contentTypeHeader, sizeof(contentTypeHeader),
                 "multipart/x-mixed-replace;boundary=%s", PART_BOUNDARY());

        esp_err_t res = httpd_resp_set_type(req, contentTypeHeader);
        if (res != ESP_OK) return res;

        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_set_hdr(req, "Expires", "0");
        httpd_resp_set_hdr(req, "X-Framerate", "30");

        portENTER_CRITICAL(&self->m_mux);
        self->m_metrics.activeClients++;
        self->m_metrics.isStreaming = true;
        portEXIT_CRITICAL(&self->m_mux);

        Serial.printf("[STREAM] Client connected (Active viewers: %u). Streaming %s @ %d FPS target...\n",
                  self->m_metrics.activeClients,
                  self->m_cam->resolutionName(), CAMERA_STREAM_TARGET_FPS);

        // Frame timing constants
        const uint32_t targetIntervalUs = 1000000UL / CAMERA_STREAM_TARGET_FPS; // 33,333 us for 30 FPS
        const int64_t sessionStartUs = esp_timer_get_time();
        int64_t lastReportUs = sessionStartUs;

        uint32_t sessionFrames = 0;
        uint64_t sessionBytes = 0;
        uint32_t windowFrames = 0;
        uint32_t windowCaptures = 0;
        uint64_t windowBytes = 0;
        uint8_t consecutiveFails = 0;

        char partHeader[160];

        while (true) {
            const int64_t frameStartUs = esp_timer_get_time();

            // 1. Hardware frame capture from PSRAM
            camera_fb_t* fb = esp_camera_fb_get();
            const int64_t captureDoneUs = esp_timer_get_time();
            const uint32_t captureDurationMs = static_cast<uint32_t>((captureDoneUs - frameStartUs) / 1000);

            if (!fb) {
                if (++consecutiveFails >= 15) {
                    Serial.println("[STREAM] Aborting stream — repeated camera capture failures");
                    res = ESP_FAIL;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            consecutiveFails = 0;
            windowCaptures++;

            // 2. Transmit frame chunks over TCP socket
            const size_t hlen = snprintf(partHeader, sizeof(partHeader),
                                         "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %ld.%06ld\r\n\r\n",
                                         PART_BOUNDARY(),
                                         fb->len,
                                         static_cast<long>(fb->timestamp.tv_sec),
                                         static_cast<long>(fb->timestamp.tv_usec));
            res = httpd_resp_send_chunk(req, partHeader, hlen);
            if (res == ESP_OK) {
                constexpr size_t streamChunkSize = 4096;
                size_t offset = 0;
                while (offset < fb->len && res == ESP_OK) {
                    const size_t chunkSize = min(streamChunkSize, fb->len - offset);
                    res = httpd_resp_send_chunk(req,
                                                reinterpret_cast<const char*>(fb->buf + offset),
                                                chunkSize);
                    offset += chunkSize;
                    taskYIELD();
                }
            }

            const size_t frameBytes = fb->len;
            esp_camera_fb_return(fb);

            if (res != ESP_OK) {
                // Client disconnected or network write error
                portENTER_CRITICAL(&self->m_mux);
                self->m_metrics.droppedFrames++;
                portEXIT_CRITICAL(&self->m_mux);
                break;
            }

            sessionFrames++;
            sessionBytes += frameBytes;
            windowFrames++;
            windowBytes += frameBytes;

            // 3. Real-time FPS & Throughput Measurement (Every 1.0s window)
            const int64_t nowUs = esp_timer_get_time();
            const int64_t elapsedWindowUs = nowUs - lastReportUs;
            if (elapsedWindowUs >= (CAMERA_STREAM_REPORT_INTERVAL_MS * 1000UL)) {
                const double elapsedSec = elapsedWindowUs / 1000000.0;
                const float streamFps = static_cast<float>(windowFrames / elapsedSec);
                const float captureFps = static_cast<float>(windowCaptures / elapsedSec);
                const float bitrateKbps = static_cast<float>((windowBytes * 8.0) / (elapsedSec * 1000.0));
                const float kbytesPerSec = static_cast<float>((windowBytes / 1024.0) / elapsedSec);
                const float avgFrameKb = windowFrames > 0 ? static_cast<float>((windowBytes / 1024.0) / windowFrames) : 0.0f;

                portENTER_CRITICAL(&self->m_mux);
                self->m_metrics.streamFps = streamFps;
                self->m_metrics.captureFps = captureFps;
                self->m_metrics.bitrateKbps = bitrateKbps;
                self->m_metrics.avgFrameKb = avgFrameKb;
                self->m_metrics.captureLatencyMs = captureDurationMs;
                self->m_metrics.totalFrames = sessionFrames;
                self->m_metrics.totalBytes = sessionBytes;
                portEXIT_CRITICAL(&self->m_mux);

                Serial.printf("[STREAM] %s @ %.1f FPS (Capture: %.1f FPS) | %.1f KB/s (%.2f Mbps) | Avg: %.1f KB | Latency: %ums | q=%d | Viewers: %u\n",
                              self->m_cam->resolutionName(), streamFps, captureFps, kbytesPerSec, bitrateKbps / 1000.0f,
                              avgFrameKb, captureDurationMs, self->m_cam->getQuality(),
                              self->m_metrics.activeClients);

                windowFrames = 0;
                windowCaptures = 0;
                windowBytes = 0;
                lastReportUs = nowUs;
            }

            // 4. Adaptive Sub-Millisecond Pacing
            // If frame work took less than target interval (33.3ms for 30 FPS), delay only the difference.
            // If network or capture took longer, don't delay (gracefully fall back to sustained rate).
            const int64_t frameWorkUs = esp_timer_get_time() - frameStartUs;
            if (frameWorkUs < targetIntervalUs) {
                const uint32_t delayMs = static_cast<uint32_t>((targetIntervalUs - frameWorkUs) / 1000);
                if (delayMs > 0) {
                    vTaskDelay(pdMS_TO_TICKS(delayMs));
                }
            } else {
                taskYIELD(); // yield without delay to keep audio & wifi responsive
            }

            // Enforce max session safety limit
            if ((esp_timer_get_time() - sessionStartUs) > (static_cast<int64_t>(CAMERA_STREAM_MAX_SESSION_MS) * 1000LL)) {
                Serial.println("[STREAM] Stream reached maximum session duration limit");
                break;
            }
        }

        // Close chunk stream
        httpd_resp_send_chunk(req, nullptr, 0);

        const double totalSec = (esp_timer_get_time() - sessionStartUs) / 1000000.0;
        const float avgSessionFps = totalSec > 0.1 ? static_cast<float>(sessionFrames / totalSec) : 0.0f;
        const float totalMb = sessionBytes / (1024.0f * 1024.0f);

        portENTER_CRITICAL(&self->m_mux);
        if (self->m_metrics.activeClients > 0) {
            self->m_metrics.activeClients--;
        }
        if (self->m_metrics.activeClients == 0) {
            self->m_metrics.isStreaming = false;
            self->m_metrics.streamFps = 0.0f;
            self->m_metrics.captureFps = 0.0f;
        }
        portEXIT_CRITICAL(&self->m_mux);

        Serial.printf("[STREAM] Client disconnected — session ended: %u frames (%.1fs, avg %.1f FPS, total %.2f MB)\n",
                      sessionFrames, totalSec, avgSessionFps, totalMb);

        return ESP_OK;
    }

    static esp_err_t handleAudio(httpd_req_t* req) {
        StreamServer* self = static_cast<StreamServer*>(req->user_ctx);
        if (!self || !self->m_audio) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");

        int16_t samples[512];
        while (true) {
            const size_t count = self->m_audio->readPcm(samples, 512);
            if (count == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            const esp_err_t res = httpd_resp_send_chunk(req,
                                                        reinterpret_cast<const char*>(samples),
                                                        count * sizeof(int16_t));
            if (res != ESP_OK) return res;
        }
    }

    // ------------------------------------------------------------------
    // Single Snapshot Capture Handler (/capture)
    // ------------------------------------------------------------------
    static esp_err_t handleCapture(httpd_req_t* req) {
        StreamServer* self = static_cast<StreamServer*>(req->user_ctx);
        if (!self || !self->m_cam || !self->m_cam->isReady()) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        camera_fb_t* fb = self->m_cam->captureFrame();
        if (!fb) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture_1280x720.jpg");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

        char ts[32];
        snprintf(ts, sizeof(ts), "%ld.%06ld", static_cast<long>(fb->timestamp.tv_sec), static_cast<long>(fb->timestamp.tv_usec));
        httpd_resp_set_hdr(req, "X-Timestamp", ts);

        esp_err_t res = httpd_resp_send(req, reinterpret_cast<const char*>(fb->buf), fb->len);
        self->m_cam->returnFrame(fb);
        return res;
    }

    // ------------------------------------------------------------------
    // Full Telemetry & Diagnostics Handler (/status)
    // ------------------------------------------------------------------
    static esp_err_t handleStatus(httpd_req_t* req) {
        StreamServer* self = static_cast<StreamServer*>(req->user_ctx);
        if (!self) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        AudioManager::Stats a = self->m_audio ? self->m_audio->stats() : AudioManager::Stats{};
        StreamMetrics sm = self->getMetrics();

        String s;
        s.reserve(1024);
        s += "device_id: " CAMERA_DEVICE_ID "\n";
        s += "device_name: " CAMERA_DEVICE_NAME "\n";
        s += "firmware_version: " FIRMWARE_VERSION "\n";
        s += "uptime_seconds: " + String(millis() / 1000) + "\n";
        s += "heap_free_bytes: " + String(ESP.getFreeHeap()) + "\n";
        s += "heap_min_free_bytes: " + String(ESP.getMinFreeHeap()) + "\n";
        s += "psram_size_bytes: " + String(ESP.getPsramSize()) + "\n";
        s += "psram_free_bytes: " + String(ESP.getFreePsram()) + "\n";
        s += "psram_status: " + String(self->m_cam->psramReady() ? "OK" : "FAIL") + "\n";
        s += "wifi_connected: " + String(self->m_net->isConnected() ? "true" : "false") + "\n";
        s += "wifi_ip: " + (self->m_net->isConnected() ? self->m_net->ip().toString() : String("-")) + "\n";
        s += "wifi_rssi_dbm: " + String(self->m_net->isConnected() ? self->m_net->rssi() : 0) + "\n";
        s += "camera_sensor: " + String(self->m_cam->sensorName()) + "\n";
        s += "camera_sensor_pid: 0x" + String(self->m_cam->sensorPid(), HEX) + "\n";
        s += "camera_resolution: " + String(self->m_cam->frameWidth()) + "x" + String(self->m_cam->frameHeight()) + "\n";
        s += "camera_jpeg_quality: " + String(self->m_cam->getQuality()) + "\n";
        s += "camera_fb_count: " + String(CAMERA_FB_COUNT) + " (PSRAM)\n";
        s += "camera_ready: " + String(self->m_cam->isReady() ? "true" : "false") + "\n";
        s += "camera_captures_ok: " + String(self->m_cam->captureCount()) + "\n";
        s += "camera_captures_fail: " + String(self->m_cam->failCount()) + "\n";
        s += "stream_target_fps: " + String(CAMERA_STREAM_TARGET_FPS) + "\n";
        s += "stream_active: " + String(sm.isStreaming ? "true" : "false") + "\n";
        s += "stream_clients: " + String(sm.activeClients) + "\n";
        s += "stream_fps_measured: " + String(sm.streamFps, 1) + "\n";
        s += "stream_capture_fps: " + String(sm.captureFps, 1) + "\n";
        s += "stream_bitrate_kbps: " + String(sm.bitrateKbps, 1) + "\n";
        s += "stream_avg_frame_kb: " + String(sm.avgFrameKb, 1) + "\n";
        s += "stream_capture_latency_ms: " + String(sm.captureLatencyMs) + "\n";
        s += "stream_total_frames: " + String(sm.totalFrames) + "\n";
        s += "stream_dropped_frames: " + String(sm.droppedFrames) + "\n";
        s += "audio_initialized: " + String(a.initialized ? "true" : "false") + "\n";
        s += "audio_receiving: " + String(a.receiving ? "true" : "false") + "\n";
        s += "audio_level_rms: " + String(a.rmsLevel) + "\n";
        s += "audio_level_peak: " + String(a.peakLevel) + "\n";
        s += "audio_noise_floor: " + String(a.noiseFloor) + "\n";
        s += "audio_dc_offset: " + String(a.dcOffset) + "\n";
        s += "audio_clipped_samples: " + String(a.clippedSamples) + "\n";
        s += "audio_nonzero_samples: " + String(a.nonZeroSamples) + "\n";
        s += "audio_raw_peak: " + String(a.rawPeak) + "\n";
        s += "audio_active_channel: " + String(a.activeChannel) + "\n";
        s += "audio_total_samples: " + String(a.totalSamples) + "\n";

        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, s.c_str(), s.length());
    }

    // ------------------------------------------------------------------
    // Live Camera Controls (/control?var=quality&val=12)
    // ------------------------------------------------------------------
    static esp_err_t handleControl(httpd_req_t* req) {
        StreamServer* self = static_cast<StreamServer*>(req->user_ctx);
        if (!self || !self->m_cam) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        size_t queryLen = httpd_req_get_url_query_len(req) + 1;
        if (queryLen <= 1) {
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }

        char* queryBuf = static_cast<char*>(malloc(queryLen));
        if (!queryBuf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        if (httpd_req_get_url_query_str(req, queryBuf, queryLen) != ESP_OK) {
            free(queryBuf);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }

        char var[32] = {0};
        char val[32] = {0};
        httpd_query_key_value(queryBuf, "var", var, sizeof(var));
        httpd_query_key_value(queryBuf, "val", val, sizeof(val));
        free(queryBuf);

        int value = atoi(val);
        bool ok = false;

        if (strcmp(var, "quality") == 0) {
            ok = self->m_cam->setQuality(value);
            Serial.printf("[CAM] Set JPEG Quality: %d (%s)\n", value, ok ? "OK" : "FAIL");
        } else if (strcmp(var, "brightness") == 0) {
            ok = self->m_cam->setBrightness(value);
            Serial.printf("[CAM] Set Brightness: %d (%s)\n", value, ok ? "OK" : "FAIL");
        } else if (strcmp(var, "contrast") == 0) {
            ok = self->m_cam->setContrast(value);
            Serial.printf("[CAM] Set Contrast: %d (%s)\n", value, ok ? "OK" : "FAIL");
        } else if (strcmp(var, "saturation") == 0) {
            ok = self->m_cam->setSaturation(value);
            Serial.printf("[CAM] Set Saturation: %d (%s)\n", value, ok ? "OK" : "FAIL");
        } else {
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        const char* resp = ok ? "OK" : "FAIL";
        return httpd_resp_send(req, resp, strlen(resp));
    }

    // ------------------------------------------------------------------
    // Responsive HTML5 Greenhouse Live View UI (/)
    // ------------------------------------------------------------------
    static esp_err_t handleIndex(httpd_req_t* req) {
        StreamServer* self = static_cast<StreamServer*>(req->user_ctx);
        if (!self) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        String ip = self->m_net->isConnected() ? self->m_net->ip().toString() : "localhost";
        String streamUrl = "http://" + ip + ":" + String(CAMERA_STREAM_PORT) + "/stream";
        String audioUrl = "http://" + ip + ":" + String(CAMERA_AUDIO_PORT) + "/audio";
        String html = "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
                      "<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
                      "<title>" CAMERA_DEVICE_NAME " - Live View</title>"
                      "<style>"
                      ":root{--bg:#0f1412;--card:#18221e;--accent:#22c55e;--border:#263c32;--text:#e2e8f0;--muted:#94a3b8}"
                      "*{box-sizing:border-box;margin:0;padding:0}body{font-family:system-ui,-apple-system,sans-serif;"
                      "background:var(--bg);color:var(--text);padding:1.5rem;display:flex;flex-direction:column;align-items:center}"
                      ".container{max-width:1100px;width:100%}"
                      "header{display:flex;justify-content:space-between;align-items:center;margin-bottom:1.5rem;padding-bottom:1rem;border-bottom:1px solid var(--border)}"
                      "h1{font-size:1.5rem;font-weight:700;color:var(--accent);display:flex;align-items:center;gap:8px}"
                      ".badge{background:#052e16;color:#4ade80;border:1px solid #166534;font-size:0.75rem;padding:3px 8px;border-radius:999px;font-weight:600}"
                      ".video-card{background:var(--card);border:1px solid var(--border);border-radius:12px;overflow:hidden;margin-bottom:1.5rem;box-shadow:0 10px 25px rgba(0,0,0,0.5)}"
                      ".stream-wrapper{position:relative;background:#000;display:flex;justify-content:center;align-items:center;min-height:360px;cursor:pointer}"
                      ".stream-wrapper img{width:100%;height:auto;max-height:72vh;object-fit:contain;display:block}"
                      ".overlay{position:absolute;top:12px;left:12px;display:flex;gap:8px;z-index:10}"
                      ".hud-tag{background:rgba(0,0,0,0.75);backdrop-filter:blur(4px);border:1px solid rgba(255,255,255,0.1);padding:4px 10px;border-radius:6px;font-size:0.75rem;font-weight:600;font-family:monospace;color:#fff}"
                      ".hud-tag span{color:var(--accent)}"
                      ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:1rem;margin-bottom:1.5rem}"
                      ".metric-card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:1rem}"
                      ".metric-title{font-size:0.8rem;color:var(--muted);text-transform:uppercase;letter-spacing:0.05em;margin-bottom:4px}"
                      ".metric-val{font-size:1.4rem;font-weight:700;font-family:monospace;color:#fff}"
                      ".metric-val small{font-size:0.8rem;color:var(--muted);font-weight:normal}"
                      ".controls{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:1.5rem}"
                      "button,a.btn{background:#1e293b;color:#fff;border:1px solid #334155;padding:8px 16px;border-radius:6px;font-weight:600;font-size:0.85rem;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;gap:6px;transition:all 0.2s}"
                      "button:hover,a.btn:hover{background:var(--accent);color:#000;border-color:var(--accent)}"
                      "footer{text-align:center;color:var(--muted);font-size:0.8rem;margin-top:2rem}"
                      "</style></head><body>"
                      "<div class='container'>"
                      "<header><div><h1>🌿 " CAMERA_DEVICE_NAME "</h1><p style='color:var(--muted);font-size:0.85rem;margin-top:4px'>Device: " CAMERA_DEVICE_ID " | Sensor: " + String(self->m_cam->sensorName()) + "</p></div>"
                      "<div><span class='badge'>" + String(self->m_cam->resolutionName()) + "</span> <span class='badge'>Target: " + String(CAMERA_STREAM_TARGET_FPS) + " FPS</span></div></header>"
                      "<div class='video-card'><div class='stream-wrapper'>"
                      "<div class='overlay'>"
                      "<div class='hud-tag'>STATUS: <span>LIVE</span></div>"
                      "<div class='hud-tag'>RES: <span>" + String(self->m_cam->frameWidth()) + "x" + String(self->m_cam->frameHeight()) + "</span></div>"
                      "<div class='hud-tag'>STREAM: <span id='hud-fps'>--</span> FPS</div>"
                      "<div class='hud-tag'>BITRATE: <span id='hud-bitrate'>--</span> Mbps</div>"
                      "</div>"
                      "<img src='" + streamUrl + "' id='mjpeg-stream' alt='Live Video Stream' onclick='startAudio()' onload='videoStarted()' onerror='videoEnded()'>"
                      "</div></div>"
                      "<div class='grid'>"
                      "<div class='metric-card'><div class='metric-title'>Measured Stream FPS</div><div class='metric-val' id='val-fps'>-- <small>FPS</small></div></div>"
                      "<div class='metric-card'><div class='metric-title'>Hardware Capture FPS</div><div class='metric-val' id='val-cap-fps'>-- <small>FPS</small></div></div>"
                      "<div class='metric-card'><div class='metric-title'>Stream Bandwidth</div><div class='metric-val' id='val-mbps'>-- <small>Mbps</small></div></div>"
                      "<div class='metric-card'><div class='metric-title'>Audio Mic Level (INMP441)</div><div class='metric-val' id='val-audio'>-- <small>RMS</small></div></div>"
                      "</div>"
                      "<div class='controls'>"
                      "<a href='/capture' target='_blank' class='btn'>📸 Single Snapshot</a>"
                      "<a href='/status' target='_blank' class='btn'>📊 Raw Telemetry</a>"
                      "<button onclick='setQuality(10)'>High Quality (q10)</button>"
                      "<button onclick='setQuality(20)'>Default (q20)</button>"
                      "<button onclick='setQuality(25)'>Balanced (q25)</button>"
                      "<button id='audio-button' onclick='toggleAudio()'>Start Audio</button>"
                      "<button onclick='reloadStream()'>🔄 Reconnect Stream</button>"
                      "</div>"
                      "<footer>Project L.E.A.F. Greenhouse Environmental Automation System &bull; Firmware v" FIRMWARE_VERSION "</footer>"
                      "</div>"
                      "<script>"
                      "function videoStarted(){if(!audioReader)startAudio();}"
                      "function videoEnded(){stopAudio();}"
                      "function reloadStream(){videoEnded();const img=document.getElementById('mjpeg-stream');img.src='" + streamUrl + "?t='+Date.now();}"
                      "function setQuality(q){fetch('/control?var=quality&val='+q).then(()=>console.log('Quality set to '+q));}"
                      "let audioContext=null,audioReader=null,audioNextTime=0,audioRun=0,audioPending=new Uint8Array(0);"
                      "function stopAudio(){audioRun++;if(audioReader){audioReader.cancel().catch(()=>{});audioReader=null;}"
                      "if(audioContext){audioContext.close().catch(()=>{});audioContext=null;}audioPending=new Uint8Array(0);"
                      "const b=document.getElementById('audio-button');if(b){b.innerText='Start Audio';b.disabled=false;}}"
                      "function toggleAudio(){if(audioReader){stopAudio();}else{startAudio();}}"
                      "async function startAudio(){const b=document.getElementById('audio-button'),run=++audioRun;"
                      "try{audioContext=audioContext||new AudioContext({sampleRate:16000});await audioContext.resume();"
                      "if(audioReader){await audioReader.cancel();} const r=await fetch('" + audioUrl + "');"
                      "if(!r.ok||!r.body)throw new Error('Audio endpoint unavailable');audioReader=r.body.getReader();"
                      "audioNextTime=audioContext.currentTime+0.1;b.innerText='Stop Audio';b.disabled=false;"
                      "while(run===audioRun){const x=await audioReader.read();if(x.done)break;let d=x.value;"
                      "if(audioPending.length){const z=new Uint8Array(audioPending.length+d.length);z.set(audioPending);z.set(d,audioPending.length);d=z;}"
                      "if(d.length%2){audioPending=d.slice(d.length-1);d=d.slice(0,d.length-1);}else audioPending=new Uint8Array(0);"
                      "if(!d.length)continue;const a=new Int16Array(d.length/2);new Uint8Array(a.buffer).set(d);const f=audioContext.createBuffer(1,a.length,16000),c=f.getChannelData(0);"
                      "for(let i=0;i<a.length;i++)c[i]=a[i]/32768;const s=audioContext.createBufferSource();s.buffer=f;s.connect(audioContext.destination);"
                      "audioNextTime=Math.max(audioNextTime,audioContext.currentTime+0.02);s.start(audioNextTime);audioNextTime+=f.duration;}}"
                      "catch(e){if(run===audioRun){b.disabled=false;b.innerText='Start Audio';}console.error(e);}}"
                      "document.addEventListener('visibilitychange',()=>{if(document.hidden)stopAudio();});"
                      "window.addEventListener('pagehide',stopAudio);"
                      "function updateHud(){fetch('/status').then(r=>r.text()).then(t=>{"
                      "const parse=(k)=>{const m=t.match(new RegExp(k+':\\\\s*([^\\\\n]+)'));return m?m[1].trim():'--';};"
                      "const fps=parse('stream_fps_measured');"
                      "const capFps=parse('stream_capture_fps');"
                      "const kbps=parseFloat(parse('stream_bitrate_kbps'))||0;"
                      "const mbps=(kbps/1000).toFixed(2);"
                      "const audio=parse('audio_level_rms');"
                      "document.getElementById('hud-fps').innerText=fps!=='0.0'&&fps!=='--'?fps:'--';"
                      "document.getElementById('hud-bitrate').innerText=mbps!=='0.00'?mbps:'--';"
                      "document.getElementById('val-fps').innerHTML=(fps!=='0.0'&&fps!=='--'?fps:'--')+' <small>FPS</small>';"
                      "document.getElementById('val-cap-fps').innerHTML=(capFps!=='0.0'&&capFps!=='--'?capFps:'--')+' <small>FPS</small>';"
                      "document.getElementById('val-mbps').innerHTML=(mbps!=='0.00'?mbps:'--')+' <small>Mbps</small>';"
                      "document.getElementById('val-audio').innerHTML=audio+' <small>RMS</small>';"
                      "}).catch(e=>console.error(e));}"
                      "setInterval(updateHud, 1000);updateHud();"
                      "</script></body></html>";

        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, html.c_str(), html.length());
    }

    CameraManager* m_cam = nullptr;
    AudioManager* m_audio = nullptr;
    NetworkManager* m_net = nullptr;
    httpd_handle_t m_cameraHttpd = nullptr;
    httpd_handle_t m_streamHttpd = nullptr;
    httpd_handle_t m_audioHttpd = nullptr;
    bool m_started = false;
    StreamMetrics m_metrics;
    mutable portMUX_TYPE m_mux = portMUX_INITIALIZER_UNLOCKED;
};

#endif // STREAM_SERVER_H
