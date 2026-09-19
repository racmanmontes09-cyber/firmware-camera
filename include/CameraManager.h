#ifndef CAMERA_MANAGER_H
#define CAMERA_MANAGER_H

#include <Arduino.h>
#include "esp_camera.h"
#include "Config.h"

// Camera bring-up, OV3660 ISP optimization, and frame capture for ESP32-S3-CAM N16R8.
// Resolution: 1280x720 (HD), 3x PSRAM framebuffers, CAMERA_GRAB_LATEST.
class CameraManager {
public:
    bool begin() {
        Serial.println("[CAM] Initializing camera subsystem...");
        m_psramReady = psramFound() && ESP.getPsramSize() > 0;
        if (!m_psramReady) {
            Serial.println("[CAM] ERROR: PSRAM not detected — 720p HD streaming requires PSRAM");
            return false;
        }
        Serial.printf("[CAM] Octal PSRAM detected: %u KB total, %u KB free\n",
                      static_cast<unsigned>(ESP.getPsramSize() / 1024),
                      static_cast<unsigned>(ESP.getFreePsram() / 1024));

        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_0;
        config.ledc_timer = LEDC_TIMER_0;
        config.pin_pwdn = CAM_PIN_PWDN;
        config.pin_reset = CAM_PIN_RESET;
        config.pin_xclk = CAM_PIN_XCLK;
        config.pin_sccb_sda = CAM_PIN_SIOD;
        config.pin_sccb_scl = CAM_PIN_SIOC;
        config.pin_d7 = CAM_PIN_D7;
        config.pin_d6 = CAM_PIN_D6;
        config.pin_d5 = CAM_PIN_D5;
        config.pin_d4 = CAM_PIN_D4;
        config.pin_d3 = CAM_PIN_D3;
        config.pin_d2 = CAM_PIN_D2;
        config.pin_d1 = CAM_PIN_D1;
        config.pin_d0 = CAM_PIN_D0;
        config.pin_vsync = CAM_PIN_VSYNC;
        config.pin_href = CAM_PIN_HREF;
        config.pin_pclk = CAM_PIN_PCLK;
        config.xclk_freq_hz = CAMERA_XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_JPEG;
        config.frame_size = CAMERA_FRAME_SIZE;      // 1280x720 (FRAMESIZE_HD)
        config.jpeg_quality = CAMERA_JPEG_QUALITY;  // 12 (high fidelity, low compression artifacts)
        config.fb_count = CAMERA_FB_COUNT;          // 3 framebuffers in PSRAM for zero-tearing DMA
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;      // always serve freshest complete frame
        config.sccb_i2c_port = -1;

        esp_err_t err = esp_camera_init(&config);
        if (err != ESP_OK) {
            Serial.printf("[CAM] Init FAILED: %s (err=0x%x)\n", esp_err_to_name(err), err);
            m_ready = false;
            return false;
        }

        sensor_t* s = esp_camera_sensor_get();
        if (s) {
            m_sensorPid = s->id.PID;
            m_sensorName = sensorNameFromPid(s->id.PID);
            Serial.printf("[CAM] Sensor identified: %s (PID: 0x%04x)\n", m_sensorName, s->id.PID);
            configureSensor(s);
        } else {
            Serial.println("[CAM] WARNING: Unable to retrieve sensor handle");
        }

        Serial.printf("[CAM] Frame config: %s JPEG q=%d, %d PSRAM framebuffer(s)\n",
                      resolutionName(CAMERA_FRAME_SIZE), CAMERA_JPEG_QUALITY, CAMERA_FB_COUNT);

        // Warm-up capture verification: sensors freshly powered (especially OV3660)
        // require a few frame cycles for auto-exposure & SCCB registers to settle.
        camera_fb_t* fb = nullptr;
        for (uint8_t attempt = 1; attempt <= CAMERA_FIRST_FRAME_ATTEMPTS && !fb; attempt++) {
            fb = esp_camera_fb_get();
            if (!fb) {
                Serial.printf("[CAM] First frame pending (attempt %u/%u)...\n",
                              attempt, CAMERA_FIRST_FRAME_ATTEMPTS);
                delay(CAMERA_FIRST_FRAME_RETRY_DELAY_MS);
            }
        }
        if (!fb) {
            Serial.println("[CAM] ERROR: No initial frame captured — will retry via background self-test");
            return false;
        }
        m_captureOk++;
        Serial.printf("[CAM] First frame captured: %u bytes (%lums)\n",
                      fb->len, static_cast<unsigned long>(m_lastCaptureMs));
        returnFrame(fb);
        m_ready = true;
        Serial.printf("[CAM] Subsystem ready for %s live streaming\n", resolutionName());
        return true;
    }

    bool isReady() const { return m_ready; }
    uint32_t captureCount() const { return m_captureOk; }
    uint32_t failCount() const { return m_captureFail; }
    bool psramReady() const { return m_psramReady; }
    uint32_t lastCaptureMs() const { return m_lastCaptureMs; }
    uint16_t sensorPid() const { return m_sensorPid; }
    const char* sensorName() const { return m_sensorName; }

    // Returns nullptr on failure. Caller MUST call returnFrame().
    camera_fb_t* captureFrame() {
        if (!m_ready) {
            return nullptr;
        }
        const uint32_t start = millis();
        camera_fb_t* fb = esp_camera_fb_get();
        m_lastCaptureMs = millis() - start;
        if (fb) {
            m_captureOk++;
        } else {
            m_captureFail++;
        }
        return fb;
    }

    void returnFrame(camera_fb_t* fb) {
        if (fb) {
            esp_camera_fb_return(fb);
        }
    }

    // Dynamic camera control methods
    bool setQuality(int quality) {
        sensor_t* s = esp_camera_sensor_get();
        if (!s || quality < 0 || quality > 63) return false;
        return (s->set_quality(s, quality) == 0);
    }

    int getQuality() {
        sensor_t* s = esp_camera_sensor_get();
        return s ? s->status.quality : CAMERA_JPEG_QUALITY;
    }

    bool setBrightness(int level) {
        sensor_t* s = esp_camera_sensor_get();
        if (!s || level < -2 || level > 2) return false;
        return (s->set_brightness(s, level) == 0);
    }

    bool setContrast(int level) {
        sensor_t* s = esp_camera_sensor_get();
        if (!s || level < -2 || level > 2) return false;
        return (s->set_contrast(s, level) == 0);
    }

    bool setSaturation(int level) {
        sensor_t* s = esp_camera_sensor_get();
        if (!s || level < -2 || level > 2) return false;
        return (s->set_saturation(s, level) == 0);
    }

    bool setFramesize(framesize_t size) {
        sensor_t* s = esp_camera_sensor_get();
        if (!s) return false;
        return (s->set_framesize(s, size) == 0);
    }

    framesize_t getFramesize() const {
        sensor_t* s = esp_camera_sensor_get();
        return s ? s->status.framesize : CAMERA_FRAME_SIZE;
    }

    uint16_t frameWidth() const {
        return frameWidthForSize(getFramesize());
    }

    uint16_t frameHeight() const {
        return frameHeightForSize(getFramesize());
    }

    const char* resolutionName(framesize_t fs = CAMERA_FRAME_SIZE) const {
        switch (fs) {
        case FRAMESIZE_HD:   return "1280x720 (HD)";
        case FRAMESIZE_SVGA: return "800x600 (SVGA)";
        case FRAMESIZE_VGA:  return "640x480 (VGA)";
        case FRAMESIZE_HVGA: return "480x320 (HVGA)";
        case FRAMESIZE_XGA:  return "1024x768 (XGA)";
        case FRAMESIZE_SXGA: return "1280x1024 (SXGA)";
        case FRAMESIZE_UXGA: return "1600x1200 (UXGA)";
        case FRAMESIZE_FHD:  return "1920x1080 (FHD)";
        default:             return "Custom / Other";
        }
    }

    // Periodic self-test used by main loop when stream is idle.
    void periodicSelfTest(uint32_t now) {
        if (!m_ready) {
            if (now - m_lastTest < CAMERA_RECOVER_INTERVAL_MS) {
                return;
            }
            m_lastTest = now;
            camera_fb_t* fb = esp_camera_fb_get();
            if (fb) {
                m_captureOk++;
                m_ready = true;
                Serial.printf("[CAM] RECOVERED — captured %u bytes after boot retry\n", fb->len);
                esp_camera_fb_return(fb);
            }
            return;
        }
        if (now - m_lastTest < CAMERA_TEST_INTERVAL_MS) {
            return;
        }
        m_lastTest = now;
        camera_fb_t* fb = captureFrame();
        if (fb) {
            Serial.printf("[CAM] Idle test frame OK: %u bytes in %lums (ok=%u fail=%u)\n",
                          fb->len,
                          static_cast<unsigned long>(m_lastCaptureMs),
                          m_captureOk, m_captureFail);
            returnFrame(fb);
        } else {
            Serial.printf("[CAM] Idle test frame FAILED after %ums (ok=%u fail=%u)\n",
                          static_cast<unsigned long>(m_lastCaptureMs),
                          m_captureOk, m_captureFail);
        }
    }

private:
    static uint16_t frameWidthForSize(framesize_t fs) {
        switch (fs) {
        case FRAMESIZE_VGA: return 640;
        case FRAMESIZE_HVGA: return 480;
        case FRAMESIZE_HD: return 1280;
        case FRAMESIZE_XGA: return 1024;
        case FRAMESIZE_SVGA: return 800;
        case FRAMESIZE_SXGA: return 1280;
        case FRAMESIZE_UXGA: return 1600;
        case FRAMESIZE_FHD: return 1920;
        default: return 0;
        }
    }

    static uint16_t frameHeightForSize(framesize_t fs) {
        switch (fs) {
        case FRAMESIZE_VGA: return 480;
        case FRAMESIZE_HVGA: return 320;
        case FRAMESIZE_HD: return 720;
        case FRAMESIZE_XGA: return 768;
        case FRAMESIZE_SVGA: return 600;
        case FRAMESIZE_SXGA: return 1024;
        case FRAMESIZE_UXGA: return 1200;
        case FRAMESIZE_FHD: return 1080;
        default: return 0;
        }
    }

    void configureSensor(sensor_t* s) {
        if (!s) return;

        // Base orientation
        s->set_vflip(s, CAMERA_VFLIP);
        s->set_hmirror(s, CAMERA_HMIRROR);

        // Check if sensor is OV3660 (PID 0x3660)
        if (s->id.PID == OV3660_PID || s->id.PID == 0x3660) {
            Serial.println("[CAM] Applying OV3660 Greenhouse ISP Tuning Profile...");
            s->set_brightness(s, OV3660_BRIGHTNESS);     // +1 ambient illumination
            s->set_contrast(s, OV3660_CONTRAST);         // +1 leaf venation / plant edge sharpness
            s->set_saturation(s, OV3660_SATURATION);     // 0 natural plant green reproduction
            s->set_sharpness(s, OV3660_SHARPNESS);       // 0 neutral sharpness
            s->set_denoise(s, OV3660_DENOISE);           // 1 sensor noise reduction
            s->set_whitebal(s, 1);                       // Auto White Balance ON
            s->set_awb_gain(s, 1);                       // AWB Gain ON
            s->set_exposure_ctrl(s, 1);                  // Auto Exposure Control ON
            s->set_gain_ctrl(s, 1);                      // Auto Gain Control ON
            s->set_bpc(s, 1);                            // Black pixel cancellation ON
            s->set_wpc(s, 1);                            // White pixel cancellation ON
            s->set_raw_gma(s, 1);                        // Gamma curve correction ON
            s->set_lenc(s, 1);                           // Lens shading correction ON
            s->set_dcw(s, 1);                            // Downsize / crop window ON
            s->set_colorbar(s, 0);                       // Color bar test pattern OFF
        } else {
            // Generic sensor tuning (e.g. OV2640 fallback)
            s->set_brightness(s, 1);
            s->set_contrast(s, 1);
            s->set_saturation(s, 0);
            s->set_whitebal(s, 1);
            s->set_awb_gain(s, 1);
            s->set_exposure_ctrl(s, 1);
            s->set_gain_ctrl(s, 1);
            s->set_gainceiling(s, GAINCEILING_2X);
        }
    }

    static const char* sensorNameFromPid(uint16_t pid) {
        switch (pid) {
        case 0x3660: return "OV3660 (3MP)";
        case 0x2640:
        case 0x2642: return "OV2640 (2MP)";
        case 0x5640: return "OV5640 (5MP)";
        case 0x7725: return "OV7725";
        default:     return "Generic DVP Sensor";
        }
    }

    bool m_ready = false;
    uint16_t m_sensorPid = 0;
    const char* m_sensorName = "Unknown";
    volatile uint32_t m_captureOk = 0;
    volatile uint32_t m_captureFail = 0;
    volatile uint32_t m_lastCaptureMs = 0;
    bool m_psramReady = false;
    uint32_t m_lastTest = 0;
};

#endif // CAMERA_MANAGER_H
