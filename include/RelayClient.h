#ifndef RELAY_CLIENT_H
#define RELAY_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_websocket_client.h>
#include "Config.h"
#include "TlsCertificates.h"
#include "CameraManager.h"
#include "AudioManager.h"
#include "NetworkManager.h"

class RelayClient {
public:
    enum class State {
        DISCONNECTED,
        CONNECTING,
        AUTHENTICATING,
        WAITING_START,
        STREAMING,
        RECONNECT_WAIT
    };

    RelayClient(CameraManager* cam, AudioManager* audio, NetworkManager* net)
        : m_cam(cam), m_audio(audio), m_net(net) {}

    ~RelayClient() {
        stop();
    }

    void begin() {
        if (strlen(RELAY_HOST) == 0 || strlen(LEAF_CAMERA_DEVICE_TOKEN) == 0) {
            Serial.println("[RELAY] Disabled (set RELAY_HOST + LEAF_CAMERA_DEVICE_TOKEN)");
            return;
        }
        m_enabled = true;
        Serial.printf("[RELAY] Will connect to wss://%s/camera/relay\n", RELAY_HOST);
    }

    void loop() {
        if (!m_enabled) return;
        if (!m_net->isConnected()) {
            disconnect();
            return;
        }

        switch (m_state) {
        case State::DISCONNECTED:
            maybeConnect();
            break;
        case State::CONNECTING:
            break;
        case State::AUTHENTICATING:
        case State::WAITING_START:
            break;
        case State::STREAMING:
            streamVideoFrame();
            streamAudioChunk();
            break;
        case State::RECONNECT_WAIT:
            if (millis() - m_lastDisconnectMs >= m_reconnectDelayMs) {
                m_state = State::DISCONNECTED;
            }
            break;
        }
    }

    void stop() {
        disconnect();
        m_enabled = false;
    }

    bool isConnected() const { return m_state == State::STREAMING; }
    bool isStreaming() const { return m_streaming && m_state == State::STREAMING; }
    State state() const { return m_state; }
    uint32_t lastConnectedMs() const { return m_connectedAtMs; }

    // When video rolls over a separate low-latency RTP/UDP pipe (RtpPusher),
    // the WebSocket relay must not double-send frames over the lossy TCP
    // uplink. Audio and control keep flowing over the relay regardless.
    void setVideoSuppressed(bool v) { m_videoSuppressed = v; }
    bool videoSuppressed() const { return m_videoSuppressed; }

private:
    void maybeConnect() {
        if (!m_enabled || !m_net->isConnected()) return;
        if (millis() - m_lastDisconnectMs < m_reconnectDelayMs) return;

        Serial.println("[RELAY] Connecting...");
        m_state = State::CONNECTING;

        // RELAY_PORT was previously dead config (the URI hardcoded :443).
        // Omit an explicit port when using the default so the URI stays short.
        char uri[160];
        if (RELAY_PORT == 443) {
            snprintf(uri, sizeof(uri), "wss://%s/camera/relay", RELAY_HOST);
        } else {
            snprintf(uri, sizeof(uri), "wss://%s:%d/camera/relay", RELAY_HOST, (int)RELAY_PORT);
        }
        m_connectStartMs = millis();

        esp_websocket_client_config_t cfg = {};
        cfg.uri = uri;
        cfg.task_stack = RELAY_TASK_STACK_SIZE;
        cfg.task_prio = RELAY_TASK_PRIORITY;
        cfg.disable_auto_reconnect = true;   // we own reconnection via loop()
        cfg.ping_interval_sec = 15;
        cfg.pingpong_timeout_sec = 30;
        cfg.keep_alive_enable = true;
        cfg.keep_alive_idle = 15;
        cfg.keep_alive_interval = 5;
        cfg.keep_alive_count = 3;
#if RELAY_SKIP_TLS_VERIFY
        // Bypass server certificate verification (dev/testing only).
        cfg.skip_cert_common_name_check = true;
#else
        // Verify the relay's wss:// server using the embedded Let's Encrypt
        // root. PEM format must be NUL-terminated; with PEM you must leave
        // cert_len as 0 (passing a length strips the terminator and makes
        // mbedTLS try to parse it as DER, causing X509_CRT_PARSE_FAILED).
        cfg.cert_pem = LEAF_RELAY_TLS_CA_PEM;
        cfg.cert_len = 0;
#endif

        if (m_client) {
            esp_websocket_client_destroy(m_client);
            m_client = nullptr;
        }
        m_client = esp_websocket_client_init(&cfg);
        if (!m_client) {
            Serial.println("[RELAY] Failed to init WebSocket client");
            scheduleReconnect();
            return;
        }

        esp_websocket_register_events(m_client, WEBSOCKET_EVENT_ANY, wsEventHandler, this);

        if (esp_websocket_client_start(m_client) != ESP_OK) {
            Serial.println("[RELAY] Failed to start WebSocket client");
            esp_websocket_client_destroy(m_client);
            m_client = nullptr;
            scheduleReconnect();
        }
    }

    void disconnect() {
        if (m_client) {
            esp_websocket_client_close(m_client, 1000);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_websocket_client_destroy(m_client);
            m_client = nullptr;
        }
        m_state = State::DISCONNECTED;
    }

    void scheduleReconnect() {
        // Once we have ever successfully reached the relay, treat later drops
        // as network blips and retry immediately at the base delay. Exponential
        // backoff is reserved for the "server truly unreachable" case where the
        // WebSocket never even connected.
        if (m_wasConnected) {
            m_reconnectDelayMs = RELAY_RECONNECT_BASE_MS;
        } else {
            m_reconnectDelayMs = min<uint32_t>(RELAY_RECONNECT_MAX_MS,
                m_reconnectDelayMs == 0 ? RELAY_RECONNECT_BASE_MS : m_reconnectDelayMs * 2);
        }
        m_lastDisconnectMs = millis();
        m_state = State::RECONNECT_WAIT;
        Serial.printf("[RELAY] Reconnecting in %lums\n", static_cast<unsigned long>(m_reconnectDelayMs));
    }

    void onConnected() {
        m_wasConnected = true;
        m_connectedAtMs = millis();
        m_reconnectDelayMs = RELAY_RECONNECT_BASE_MS;
        Serial.printf("[RELAY] WebSocket connected in %lums, authenticating...\n",
                      static_cast<unsigned long>(millis() - m_connectStartMs));
        m_state = State::AUTHENTICATING;
        sendAuthMessage();
    }

    void onDisconnected() {
        if (m_state != State::DISCONNECTED && m_state != State::RECONNECT_WAIT) {
            Serial.println("[RELAY] Connection lost");
            m_streaming = false;
            scheduleReconnect();
        }
    }

    void onData(const char* data, int len, int opCode) {
        if (opCode == 0x01 && len > 0) {
            handleTextMessage(data, len);
        }
    }

    void handleTextMessage(const char* data, int len) {
        String msg(data, len);
        if (msg.indexOf("\"start_stream\"") >= 0) {
            if (m_state == State::AUTHENTICATING || m_state == State::WAITING_START) {
                Serial.println("[RELAY] Received start_stream — streaming active");
                m_state = State::STREAMING;
                m_streaming = true;
                m_reconnectDelayMs = RELAY_RECONNECT_BASE_MS;
            }
        } else if (msg.indexOf("\"stop_stream\"") >= 0) {
            Serial.println("[RELAY] Received stop_stream — pausing");
            m_streaming = false;
            m_state = State::WAITING_START;
        }
    }

    void sendAuthMessage() {
        String auth = "{\"token\":\"" LEAF_CAMERA_DEVICE_TOKEN
                      "\",\"device_id\":\"" CAMERA_DEVICE_ID "\"}";
        if (m_client && esp_websocket_client_is_connected(m_client)) {
            esp_websocket_client_send_text(m_client, auth.c_str(), auth.length(), 1000);
        }
        m_state = State::WAITING_START;
    }

    void streamVideoFrame() {
        if (!m_streaming || !m_cam || !m_cam->isReady()) return;
        if (!m_client || !esp_websocket_client_is_connected(m_client)) return;
        if (m_videoSuppressed) return;

        // Pace the uplink to RELAY_TARGET_FPS. Each push only carries the
        // freshest frame (no queue), so over a saturated link we sacrifice
        // frame rate rather than deepening latency — the browser always sees
        // the newest image, never a backlog of stale ones.
        const uint32_t minPeriodMs = 1000UL / RELAY_TARGET_FPS;
        const uint32_t now = millis();
        if (now - m_lastFrameSentMs < minPeriodMs) return;

        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) return;

        // IMPORTANT: the relay parses each WebSocket BINARY message as
        //     [type:1][len:4][payload]
        // so the 5-byte header and the JPEG payload MUST be delivered as a
        // single WebSocket message. esp_websocket_client_send() wraps every
        // call as its own frame, so calling it twice splits the header and
        // payload into two messages the relay can't reassemble. Send them
        // together in one buffer.
        uint8_t header[5];
        header[0] = 0x01;
        uint32_t len = fb->len;
        header[1] = (len >> 24) & 0xFF;
        header[2] = (len >> 16) & 0xFF;
        header[3] = (len >> 8) & 0xFF;
        header[4] = len & 0xFF;

        // Build a contiguous [header+payload] frame. Use regular heap
        // (not PSRAM) to avoid the slow OPI PSRAM memcpy on this path —
        // VGA JPEG frames are small enough (~8-10 KB) to fit in IRAM/DRAM.
        size_t total = 5 + fb->len;
        uint8_t* frame = (uint8_t*)malloc(total);
        if (!frame) {
            esp_camera_fb_return(fb);
            return;
        }
        memcpy(frame, header, 5);
        memcpy(frame + 5, fb->buf, fb->len);
        const int sent = esp_websocket_client_send(m_client,
            reinterpret_cast<const char*>(frame), (int)total, RELAY_SEND_TIMEOUT_MS);
        free(frame);
        m_lastFrameSentMs = millis();

        if (sent < 0) {
            // Transport can't keep up; the WS client will raise DISCONNECTED
            // shortly and the loop() will schedule a quick reconnect.
            Serial.println("[RELAY] Video send failed — connection likely stale");
        }

        esp_camera_fb_return(fb);
    }

    void streamAudioChunk() {
        if (!m_streaming || !m_audio || !m_client) return;
        if (!esp_websocket_client_is_connected(m_client)) return;

        int16_t samples[512];
        size_t count = m_audio->readPcmRelay(samples, 512);
        if (count == 0) return;

        size_t payloadBytes = count * sizeof(int16_t);
        uint8_t header[5];
        header[0] = 0x02;
        header[1] = (payloadBytes >> 24) & 0xFF;
        header[2] = (payloadBytes >> 16) & 0xFF;
        header[3] = (payloadBytes >> 8) & 0xFF;
        header[4] = payloadBytes & 0xFF;

        // Send header + PCM payload as ONE WebSocket message (see streamVideoFrame).
        // Use regular heap — audio chunks are tiny (1 KB).
        size_t total = 5 + payloadBytes;
        uint8_t* frame = (uint8_t*)malloc(total);
        if (!frame) return;
        memcpy(frame, header, 5);
        memcpy(frame + 5, samples, payloadBytes);
        esp_websocket_client_send(m_client,
            reinterpret_cast<const char*>(frame), (int)total, 200);
        free(frame);
    }

    static void wsEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
        RelayClient* self = static_cast<RelayClient*>(arg);
        esp_websocket_event_data_t* ev = static_cast<esp_websocket_event_data_t*>(data);

        switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            self->onConnected();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            self->onDisconnected();
            break;
        case WEBSOCKET_EVENT_DATA:
            if (ev->op_code == 0x01 && ev->data_len > 0) {
                self->onData(ev->data_ptr, ev->data_len, ev->op_code);
            }
            break;
        default:
            break;
        }
    }

    CameraManager* m_cam = nullptr;
    AudioManager* m_audio = nullptr;
    NetworkManager* m_net = nullptr;
    esp_websocket_client_handle_t m_client = nullptr;
    bool m_enabled = false;
    bool m_streaming = false;
    bool m_wasConnected = false;
    bool m_videoSuppressed = false;
    State m_state = State::DISCONNECTED;
    uint32_t m_lastDisconnectMs = 0;
    uint32_t m_connectStartMs = 0;
    uint32_t m_lastFrameSentMs = 0;
    uint32_t m_connectedAtMs = 0;
    uint32_t m_reconnectDelayMs = RELAY_RECONNECT_BASE_MS;
};

#endif // RELAY_CLIENT_H
