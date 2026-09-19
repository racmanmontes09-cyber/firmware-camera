#ifndef RTP_PUSHER_H
#define RTP_PUSHER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "Config.h"
#include "CameraManager.h"

// ------------------------------------------------------------------
// RFC 2435 (RTP/M-JPEG) packetizer + fire-and-forget UDP push to the
// VPS MediaMTX ingest (`udp+rtp://0.0.0.0:RTP_RELAY_PORT`).
//
// The wire format below mirrors MediaMTX's own codec chain
// (gortsplib pkg/format/rtpmjpeg* + mediacommon pkg/codecs/jpeg) byte
// for byte, so the camera stream decodes cleanly on the server:
//
//   * RTP header: PT 26 (JPEG/90000), SSRC fixed per session, Marker bit
//     set on the final packet of each frame, seq++ per packet (continuous
//     across frames), timestamp = 90000/RTP_TARGET_FPS per frame.
//   * 8-byte main header:
//         TypeSpecific(1)=0 | FragmentOffset(3, BE, entropy bytes sent) |
//         Type(1, from SOF sampling) | Quantization(1)=255 |
//         Width/8(1) | Height/8(1)
//   * Only the FIRST packet of a frame carries the quantization-table
//     header: MBZ(1)=0 | Precision(1)=0 | Length(2, BE)=128 | then the
//     two 64-byte tables sorted by table ID (luma, chroma for esp32-camera).
//   * The entropy-coded data starts immediately after the SOS segment and
//     includes the trailing EOI marker (MediaMTX only appends EOI if absent).
//   * No DRI/restart markers: esp32-camera emits baseline YUV422 JPEG
//     (SOF Type 0, two DQT tables) — same assumption gortsplib makes.
//
// This is the connectionless low-latency path: no TLS, no TCP window, no
// backlog. A dropped UDP packet costs at most a glitch in the current
// frame; the next frame is always the newest capture.
// ------------------------------------------------------------------
class RtpPusher {
public:
    RtpPusher(CameraManager* cam) : m_cam(cam) {}

    void begin() {
        if (strlen(RTP_RELAY_HOST) == 0) {
            Serial.println("[RTP] Disabled (RTP_RELAY_HOST empty)");
            return;
        }
        if (!m_cam || !m_cam->isReady()) {
            Serial.println("[RTP] Disabled (camera not ready)");
            return;
        }

        IPAddress ip;
        if (!ip.fromString(RTP_RELAY_HOST)) {
            if (!WiFi.hostByName(RTP_RELAY_HOST, ip)) {
                Serial.printf("[RTP] Cannot resolve %s — pusher disabled\n", RTP_RELAY_HOST);
                return;
            }
        }
        m_dstIp = ip;
        m_enabled = true;
        m_ssrc = esp_random();
        m_lastSendMs = 0;
        Serial.printf("[RTP] Pushing RFC2435 MJPEG to %s:%d @ %u FPS (payload<=%uB)\n",
                      ip.toString().c_str(), (int)RTP_RELAY_PORT,
                      (unsigned)RTP_TARGET_FPS, (unsigned)RTP_PAYLOAD_MAX);
    }

    // Paced capture + send, called from loopTask every iteration.
    void loop() {
        if (!m_enabled) return;

        const uint32_t now = millis();
        if (!WiFi.isConnected()) {
            m_lastSendMs = 0; // resume immediately on reconnect
            return;
        }
        const uint32_t minPeriodMs = 1000UL / RTP_TARGET_FPS;
        if (now - m_lastSendMs < minPeriodMs) return;

        camera_fb_t* fb = m_cam->captureFrame();
        if (!fb) return;

        FrameInfo fi;
        if (!parseJpeg(fb->buf, fb->len, fi)) {
            Serial.printf("[RTP] Frame parse FAILED (%u bytes)\n", (unsigned)fb->len);
            m_cam->returnFrame(fb);
            m_dropCount++;
            m_lastSendMs = now;
            return;
        }

        // Push the frame, then adapt the inter-datagram gap to the link:
        // a frame that lost any datagram means the uplink is momentarily
        // congested, so widen the pace for the next frame; a stretch of
        // clean frames earns a step back toward the minimum.
        bool failed = sendFrame(fb->buf + fi.entropyOff, fi.entropyLen, fi);
        m_cam->returnFrame(fb);

        if (!failed) {
            m_lastSendMs = millis(); // uplink proven healthy -> feeds recovery gate
            if (++m_cleanFrames >= 20 && m_paceMs > RTP_PACING_MIN_MS) {
                m_cleanFrames = 0;
                m_paceMs--;
            }
        } else {
            m_cleanFrames = 0;
            m_dropCount++;
            if (m_paceMs < RTP_PACING_MAX_MS) {
                m_paceMs++;
                Serial.printf("[RTP] Uplink congested — pacing now %ums/datagram\n",
                              (unsigned)m_paceMs);
            }
        }
    }

    uint32_t paceMs() const { return m_paceMs; }

    bool isActive() const { return m_enabled; }
    uint32_t lastSendMs() const { return m_enabled ? m_lastSendMs : 0; }
    uint32_t framesSent() const { return m_framesSent; }
    uint32_t packetsSent() const { return m_packetsSent; }
    uint32_t dropCount() const { return m_dropCount; }

private:
    static constexpr size_t JPEG_HEADER_LEN = 8;
    static constexpr size_t QUANT_HEADER_LEN = 4;
    static constexpr size_t QUANT_TABLE_LEN = 128; // 2 x 64
    static constexpr uint32_t RTP_PACING_MIN_MS = 0;
    static constexpr uint32_t RTP_PACING_MAX_MS = 12;

    struct FrameInfo {
        size_t entropyOff = 0;
        size_t entropyLen = 0;
        uint8_t type = 0;      // SOF sampling -> RFC2435 type
        uint8_t width8 = 0;    // width / 8
        uint8_t height8 = 0;   // height / 8
        uint8_t dqt[QUANT_TABLE_LEN] = {0};
        bool dqtValid = false;
    };

    // Parse the JPEG header chain (SOI..SOS) and fill FrameInfo. On success
    // entropyOff points at the first entropy-coded byte (after the SOS segment).
    static bool parseJpeg(const uint8_t* buf, size_t len, FrameInfo& fi) {
        if (len < 4 || buf[0] != 0xFF || buf[1] != 0xD8) {
            return false;
        }

        // DQT tables keyed by table ID (4 slots are ample for esp32-camera).
        uint8_t tables[4][64] = {{0}};
        bool tablePresent[4] = {false, false, false, false};

        size_t pos = 2; // skip SOI
        while (pos + 4 <= len) {
            if (buf[pos] != 0xFF) {
                return false;
            }
            if (buf[pos + 1] == 0xFF) {
                pos++; // fill byte; stay on the same marker run
                continue;
            }
            const uint8_t marker = buf[pos + 1];
            if (marker == 0xD9) { // EOI before SOS — malformed
                return false;
            }
            pos += 2; // now pos points at the segment length field
            if (pos + 2 > len) {
                return false;
            }
            const uint16_t segLen = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
            // segLen counts from (and includes) the length field itself.
            if (pos + segLen > len) {
                return false;
            }
            // Value bytes of the segment (payload = segLen - 2).
            const uint8_t* seg = buf + pos + 2;
            const size_t segPayload = segLen - 2;

            switch (marker) {
            case 0xDB: { // DQT
                size_t q = 0;
                while (q + 1 + 64 <= segPayload) {
                    const uint8_t info = seg[q];
                    const uint8_t precision = (info >> 4) & 0x0F;
                    const uint8_t id = info & 0x0F;
                    if (id > 3) {
                        break;
                    }
                    if (precision != 0) {
                        q += 1 + 128; // 16-bit entries unsupported; skip table
                        continue;
                    }
                    memcpy(tables[id], seg + q + 1, 64);
                    tablePresent[id] = true;
                    q += 1 + 64;
                }
                if (tablePresent[0] && tablePresent[1]) {
                    memcpy(fi.dqt, tables[0], 64);
                    memcpy(fi.dqt + 64, tables[1], 64);
                    fi.dqtValid = true;
                }
                break;
            }
            case 0xC0:   // SOF0 baseline
            case 0xC1: { // SOF1 extended
                if (segPayload < 6) {
                    return false;
                }
                const uint16_t height = ((uint16_t)seg[1] << 8) | seg[2];
                const uint16_t width = ((uint16_t)seg[3] << 8) | seg[4];
                const uint8_t nf = seg[5];
                fi.width8 = uint8_t(width / 8);
                fi.height8 = uint8_t(height / 8);

                if (nf == 1) {
                    const uint8_t sampling = (segPayload >= 7) ? seg[7] : 0x11;
                    const uint8_t h = (sampling >> 4) & 0x0F;
                    const uint8_t v = sampling & 0x0F;
                    if (h == 1 && v == 1) fi.type = 6;
                    else if (h == 1 && v == 2) fi.type = 7;
                    else if (h == 2 && v == 1) fi.type = 8;
                    else fi.type = 9;
                } else if (nf == 3 && segPayload >= 7) {
                    // MediaMTX/mediacommon infers the RFC2435 Type from the
                    // FIRST component's sampling factor only (all others must
                    // be 1x1): comp0 2x1 => Type 0 (4:2:2), 2x2 => Type 1
                    // (4:2:0). The esp32-camera YUV422 JPEG is Type 0.
                    const uint8_t s0 = seg[7];
                    const uint8_t h0 = (s0 >> 4) & 0x0F;
                    const uint8_t v0 = s0 & 0x0F;
                    if (h0 == 2 && v0 == 1) {
                        fi.type = 0; // 4:2:2
                    } else if (h0 == 2 && v0 == 2) {
                        fi.type = 1; // 4:2:0
                    } else if (h0 == 1 && v0 == 1 && segPayload >= 9) {
                        const uint8_t h1 = (seg[10] >> 4) & 0x0F;
                        const uint8_t v1 = seg[10] & 0x0F;
                        const uint8_t h2 = (seg[13] >> 4) & 0x0F;
                        const uint8_t v2 = seg[13] & 0x0F;
                        if (h1 == 1 && v1 == 2 && h2 == 1 && v2 == 2) fi.type = 2;  // 4:4:0
                        else if (h1 == 2 && v1 == 1 && h2 == 2 && v2 == 1) fi.type = 1; // 4:2:0
                        else if (h1 == 2 && v1 == 2 && h2 == 2 && v2 == 2) fi.type = 3; // 4:1:1
                        else fi.type = 0;                                              // 4:2:2
                    } else {
                        fi.type = 0;
                    }
                }
                break;
            }
            case 0xDA: { // SOS — end of header chain; entropy data follows
                const size_t entropyOff = pos + segLen;
                if (entropyOff >= len) {
                    return false;
                }
                fi.entropyOff = entropyOff;
                fi.entropyLen = len - entropyOff;
                return true;
            }
            default:
                break; // APPx, DHT, COM, DRI(...) all skipped by segment length
            }
            pos += segLen;
        }

        // No SOS found; either the frame is truncated or not a JPEG at all.
        if (!tablePresent[0] || !tablePresent[1]) {
            fi.dqtValid = false;
        }
        return false;
    }

    bool sendFrame(const uint8_t* entropy, size_t entropyLen, const FrameInfo& fi) {
        // First packet payload carries header + quant table, so its entropy
        // capacity is smaller. Guests the exact gortsplib split.
        const size_t firstCapacity = RTP_PAYLOAD_MAX - (JPEG_HEADER_LEN + QUANT_HEADER_LEN + QUANT_TABLE_LEN);
        const size_t packetCapacity = RTP_PAYLOAD_MAX - JPEG_HEADER_LEN;

size_t off = 0;
            uint32_t fragmentOffset = 0;
            bool first = true;
            bool error = false;
            uint16_t seq = m_seq;

        while (true) {
            size_t cap = first ? firstCapacity : packetCapacity;
            size_t chunk = entropyLen - off;
            if (chunk > cap) chunk = cap;
            const bool last = (off + chunk >= entropyLen);

            uint8_t pkt[12 + RTP_PAYLOAD_MAX];
            size_t n = 0;
            pkt[n++] = 0x80;                                      // V=2, no P/X/CC
            pkt[n++] = last ? (0x80 | 26) : 26;                   // M bit + PT 26
            pkt[n++] = (seq >> 8) & 0xFF;
            pkt[n++] = seq & 0xFF;
            pkt[n++] = (m_ts >> 24) & 0xFF;
            pkt[n++] = (m_ts >> 16) & 0xFF;
            pkt[n++] = (m_ts >> 8) & 0xFF;
            pkt[n++] = m_ts & 0xFF;
            pkt[n++] = (m_ssrc >> 24) & 0xFF;
            pkt[n++] = (m_ssrc >> 16) & 0xFF;
            pkt[n++] = (m_ssrc >> 8) & 0xFF;
            pkt[n++] = m_ssrc & 0xFF;

            // Main JPEG header (RFC2435, 8 bytes)
            pkt[n++] = 0x00;
            pkt[n++] = (fragmentOffset >> 16) & 0xFF;
            pkt[n++] = (fragmentOffset >> 8) & 0xFF;
            pkt[n++] = fragmentOffset & 0xFF;
            pkt[n++] = fi.type;
            pkt[n++] = 0xFF; // Quantization=255: tables transported
            pkt[n++] = fi.width8;
            pkt[n++] = fi.height8;

            if (first) {
                pkt[n++] = 0x00; // MBZ
                pkt[n++] = 0x00; // Precision: 8-bit
                pkt[n++] = 0x00;
                pkt[n++] = QUANT_TABLE_LEN;
                if (fi.dqtValid) {
                    memcpy(pkt + n, fi.dqt, QUANT_TABLE_LEN);
                } else {
                    memcpy(pkt + n, kDefaultTables, QUANT_TABLE_LEN);
                }
                n += QUANT_TABLE_LEN;
                first = false;
            }

            memcpy(pkt + n, entropy + off, chunk);
            n += chunk;
            off += chunk;
            fragmentOffset = off;

            m_udp.beginPacket(m_dstIp, RTP_RELAY_PORT);
            m_udp.write(pkt, n);
            if (!m_udp.endPacket()) {
                // TX buffer full: abort the rest of this frame rather than
                // keep hammering a socket that can't absorb it. The partial
                // frame is discarded on the server ("wrong fragment"); the
                // next frame starts fresh. loop() widens the pacing gap.
                error = true;
                break;
            }
            m_packetsSent++;

            seq++;
            if (last) break;

            // A 720p frame is ~20-40 thousand bytes -> 15-30 datagrams. Firing
            // them back-to-back exhausts lwIP's UDP TX pbuf pool before the
            // WiFi driver drains a single slot, and endPacket() starts failing
            // with err 12 (ENOMEM). Pacing each datagram with a yield lets the
            // driver flush; the gap grows automatically when the uplink is
            // congested (see the congestion logic in loop()).
            if (m_paceMs > 0) {
                vTaskDelay(m_paceMs);
            }
        }

        m_seq = seq;
        m_ts += 90000UL / RTP_TARGET_FPS;
        m_framesSent++;
        return error;
    }

    CameraManager* m_cam = nullptr;
    WiFiUDP m_udp;
    IPAddress m_dstIp;
    bool m_enabled = false;
    uint32_t m_ssrc = 0;
    uint16_t m_seq = 0;
    uint32_t m_ts = 0;
    uint32_t m_lastSendMs = 0;
    uint32_t m_framesSent = 0;
    uint32_t m_packetsSent = 0;
    uint32_t m_dropCount = 0;

    // Adaptive pacing state (see loop()): gap between UDP datagrams grows
    // under congestion, shrinks after a clean stretch.
    uint32_t m_paceMs = RTP_PACING_MIN_MS;
    uint32_t m_cleanFrames = 0;

    // Fallback standard JPEG tables (ESA/JPEG Annex K), used only if a frame
    // lacks a valid 2-table DQT. Identical in layout to MediaMTX's decoder
    // defaults, so decode stays consistent.
    static const uint8_t kDefaultTables[QUANT_TABLE_LEN];
};

// Standard luma then chroma quantizers, in zig-zag order.
const uint8_t RtpPusher::kDefaultTables[RtpPusher::QUANT_TABLE_LEN] = {
    // Luma
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99,
    // Chroma
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

#endif // RTP_PUSHER_H