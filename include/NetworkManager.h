#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "Config.h"

// Non-blocking Wi-Fi state machine. Never delays the loop; all
// transitions are driven by millis() and WiFi.status() polling.
class NetworkManager {
public:
    enum class State {
        NO_CREDENTIALS,
        CONNECTING,
        CONNECTED,
        RETRY_WAIT
    };

    void begin() {
        if (strlen(WIFI_SSID) == 0) {
            m_state = State::NO_CREDENTIALS;
            Serial.println("[NET] No WiFi credentials configured (set LEAF_WIFI_SSID/LEAF_WIFI_PASSWORD)");
            return;
        }
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(CAMERA_DEVICE_ID);
        WiFi.setAutoReconnect(false);
        startAttempt(millis());
    }

    void loop() {
        const uint32_t now = millis();
        switch (m_state) {
        case State::CONNECTING:
            handleConnecting(now);
            break;
        case State::CONNECTED:
            handleConnected(now);
            break;
        case State::RETRY_WAIT:
            if (now - m_lastEvent >= m_retryDelayMs) {
                startAttempt(now);
            }
            break;
        case State::NO_CREDENTIALS:
        default:
            break;
        }
    }

    bool isConnected() const { return m_state == State::CONNECTED; }
    bool hasCredentials() const { return strlen(WIFI_SSID) > 0; }
    State state() const { return m_state; }

    IPAddress ip() const { return WiFi.localIP(); }
    int32_t rssi() const { return WiFi.RSSI(); }
    uint32_t attemptCount() const { return m_attempts; }

private:
    void startAttempt(uint32_t now) {
        Serial.printf("[NET] Connecting to WiFi (SSID: %s, attempt %u)...\n", WIFI_SSID, ++m_attempts);
        WiFi.disconnect();
        delay(1); // yield so the radio can process the disconnect command
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        m_state = State::CONNECTING;
        m_connectStart = now;
        m_lastStatusLog = now;
    }

    void handleConnecting(uint32_t now) {
        const wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED) {
            m_state = State::CONNECTED;
            m_failStreak = 0;
            Serial.println("[NET] Connected");
            disableWifiPowerSave();
            Serial.print("[NET] IP: ");
            Serial.println(WiFi.localIP());
            Serial.printf("[NET] RSSI: %d dBm\n", WiFi.RSSI());
            return;
        }
        if (now - m_lastStatusLog >= WIFI_STATUS_LOG_MS) {
            m_lastStatusLog = now;
            Serial.printf("[NET] Connecting... status=%s (%lus elapsed)\n",
                          statusName(status),
                          static_cast<unsigned long>((now - m_connectStart) / 1000));
        }
        if (now - m_connectStart >= WIFI_CONNECT_TIMEOUT_MS) {
            m_failStreak++;
            Serial.printf("[NET] Connection timed out (%u consecutive failures)\n", m_failStreak);
            scheduleRetry(now);
        }
    }

    void handleConnected(uint32_t now) {
        (void)now;
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[NET] Connection lost");
            m_state = State::RETRY_WAIT;
            m_lastEvent = millis();
            m_retryDelayMs = WIFI_RETRY_BASE_MS;
        }
    }

    void scheduleRetry(uint32_t now) {
        WiFi.disconnect();
        m_retryDelayMs = min<uint32_t>(WIFI_RETRY_MAX_MS, WIFI_RETRY_BASE_MS << (m_failStreak - 1));
        if (m_failStreak % WIFI_RADIO_RESET_AFTER_FAILS == 0) {
            Serial.println("[NET] Repeated failures — resetting Wi-Fi radio");
            WiFi.mode(WIFI_OFF);
            delay(10); // brief settle before re-enabling the radio
            WiFi.mode(WIFI_STA);
        }
        m_state = State::RETRY_WAIT;
        m_lastEvent = now;
        Serial.printf("[NET] Retrying in %lums\n", static_cast<unsigned long>(m_retryDelayMs));
    }

    static const char* statusName(wl_status_t status) {
        switch (status) {
        case WL_IDLE_STATUS: return "idle";
        case WL_NO_SSID_AVAIL: return "no-ssid";
        case WL_SCAN_COMPLETED: return "scan-done";
        case WL_CONNECTED: return "connected";
        case WL_CONNECT_FAILED: return "connect-failed";
        case WL_CONNECTION_LOST: return "connection-lost";
        case WL_DISCONNECTED: return "disconnected";
        default: return "unknown";
        }
    }

    void disableWifiPowerSave() {
        // WIFI_PS_MIN_MODEM (the default) powers down the radio hundreds of ms
        // at a time, adding 200-500ms jitter to every TCP ACK round-trip and
        // increasing the chance of mbedTLS retransmit timeouts during a WS/HTTPS
        // frame push. Disable it once per association for sub-500ms live latency.
        esp_wifi_set_ps(WIFI_PS_NONE);
        Serial.println("[NET] WiFi power-save OFF (WIFI_PS_NONE)");
    }

    State m_state = State::NO_CREDENTIALS;
    uint32_t m_connectStart = 0;
    uint32_t m_lastStatusLog = 0;
    uint32_t m_lastEvent = 0;
    uint32_t m_retryDelayMs = WIFI_RETRY_BASE_MS;
    uint32_t m_attempts = 0;
    uint32_t m_failStreak = 0;
};

#endif // NETWORK_MANAGER_H
