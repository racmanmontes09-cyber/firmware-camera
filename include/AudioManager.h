#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <Arduino.h>
#include <cmath>
#include <driver/i2s.h>
#include "Config.h"

// INMP441 digital MEMS microphone capture over ESP32-S3 I2S.
//
// Hardware notes:
//  - INMP441 outputs 24-bit two's-complement data, left-justified in a
//    32-bit slot. We clock 32-bit slots and shift >>8 to recover the
//    effective 24-bit sample amplitude.
//  - Both I2S slots are sampled so either L/R strap is accepted.
//
// The reported level is a RELATIVE AMPLITUDE (RMS of raw sample values),
// not calibrated dB SPL — no acoustic calibration has been performed.
class AudioManager {
public:
    struct Stats {
        bool initialized = false;
        bool receiving = false;       // valid non-silent samples are arriving
        uint32_t totalSamples = 0;
        uint32_t rmsLevel = 0;        // relative amplitude, 24-bit scale
        uint32_t peakLevel = 0;
        float soundDbSpl = 30.0f;     // Localized acoustic dB SPL metric (RA 10173 data minimization)
        uint32_t noiseFloor = 0;
        int32_t dcOffset = 0;
        uint16_t clippedSamples = 0;
        uint16_t nonZeroSamples = 0;
        uint32_t rawPeak = 0;
        char activeChannel = '-';
        uint32_t readErrors = 0;
    };

    bool begin() {
        Serial.println("[AUDIO] Initializing INMP441...");

        i2s_config_t cfg = {};
        cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
        cfg.sample_rate = AUDIO_SAMPLE_RATE_HZ;
        cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
        cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
        cfg.communication_format = I2S_COMM_FORMAT_I2S_MSB;
        cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
        cfg.dma_buf_count = AUDIO_DMA_BUF_COUNT;
        cfg.dma_buf_len = AUDIO_DMA_BUF_LEN_SAMPLES;
        cfg.use_apll = false;
        cfg.tx_desc_auto_clear = false;
        cfg.fixed_mclk = 0;

        esp_err_t err = i2s_driver_install(AUDIO_I2S_PORT, &cfg, 0, nullptr);
        if (err != ESP_OK) {
            Serial.printf("[AUDIO] ERROR: i2s_driver_install failed: %s (err=0x%x)\n",
                          esp_err_to_name(err), err);
            return false;
        }

        i2s_pin_config_t pins = {};
        pins.mck_io_num = I2S_PIN_NO_CHANGE;
        pins.bck_io_num = INMP441_SCK_PIN;
        pins.ws_io_num = INMP441_WS_PIN;
        pins.data_out_num = I2S_PIN_NO_CHANGE;
        pins.data_in_num = INMP441_SD_PIN;

        err = i2s_set_pin(AUDIO_I2S_PORT, &pins);
        if (err != ESP_OK) {
            Serial.printf("[AUDIO] ERROR: i2s_set_pin failed: %s (err=0x%x)\n",
                          esp_err_to_name(err), err);
            i2s_driver_uninstall(AUDIO_I2S_PORT);
            return false;
        }
        i2s_zero_dma_buffer(AUDIO_I2S_PORT);

        Serial.printf("[AUDIO] I2S initialized: %d Hz, %d-bit slots, auto L/R, DMA %dx%d\n",
                      AUDIO_SAMPLE_RATE_HZ, AUDIO_BITS_PER_SAMPLE,
                      AUDIO_DMA_BUF_COUNT, AUDIO_DMA_BUF_LEN_SAMPLES);
        Serial.printf("[AUDIO] Pins: BCLK=GPIO%d WS=GPIO%d SD=GPIO%d\n",
                      INMP441_SCK_PIN, INMP441_WS_PIN, INMP441_SD_PIN);

        m_stats.initialized = true;

        // Dedicated capture task: short non-blocking-ish reads paced by the
        // DMA buffer fill rate (~16ms per chunk at 16 kHz), so camera and
        // networking keep running while audio streams continuously.
        if (xTaskCreatePinnedToCore(taskTrampoline, "audioTask", AUDIO_TASK_STACK_SIZE,
                                    this, AUDIO_TASK_PRIORITY, &m_taskHandle,
                                    AUDIO_TASK_CORE) != pdPASS) {
            Serial.println("[AUDIO] ERROR: failed to create audio task");
            i2s_driver_uninstall(AUDIO_I2S_PORT);
            m_stats.initialized = false;
            return false;
        }
        return true;
    }

    void reportIfDue(uint32_t now) {
        if (!m_stats.initialized || now - m_lastReport < AUDIO_REPORT_INTERVAL_MS) {
            return;
        }
        m_lastReport = now;
        Stats snapshot = stats();
        Serial.printf("[AUDIO] SPL: %.1f dB | RMS: %lu Peak: %lu RawPeak: %lu Noise: %lu DC: %ld Clip: %u/%u Samples: %u Ch: %c Status: %s\n",
                  snapshot.soundDbSpl,
                  static_cast<unsigned long>(snapshot.rmsLevel),
                  static_cast<unsigned long>(snapshot.peakLevel),
                  static_cast<unsigned long>(snapshot.rawPeak),
                  static_cast<unsigned long>(snapshot.noiseFloor),
                  static_cast<long>(snapshot.dcOffset), snapshot.clippedSamples,
                  AUDIO_READ_SAMPLES,
                  snapshot.totalSamples,
                  snapshot.activeChannel,
                  snapshot.receiving ? "Audio samples received" : "Silence / no signal");
    }

    Stats stats() const {
        Stats out;
        portENTER_CRITICAL(&m_mux);
        out = m_stats;
        portEXIT_CRITICAL(&m_mux);
        return out;
    }

    size_t readPcm(int16_t* output, size_t capacity) {
        if (!output || capacity == 0) return 0;
        size_t count = 0;
        portENTER_CRITICAL(&m_mux);
        while (count < capacity && m_pcmRead != m_pcmWrite) {
            output[count++] = m_pcmRing[m_pcmRead];
            m_pcmRead = (m_pcmRead + 1) % AUDIO_RING_BUFFER_SAMPLES;
        }
        portEXIT_CRITICAL(&m_mux);
        return count;
    }

    // Second read pointer for the relay client. Allows the relay to
    // consume audio independently from the HTTP audio stream without
    // either consumer losing data the other has already read.
    size_t readPcmRelay(int16_t* output, size_t capacity) {
        if (!output || capacity == 0) return 0;
        size_t count = 0;
        portENTER_CRITICAL(&m_mux);
        while (count < capacity && m_pcmReadRelay != m_pcmWrite) {
            output[count++] = m_pcmRing[m_pcmReadRelay];
            m_pcmReadRelay = (m_pcmReadRelay + 1) % AUDIO_RING_BUFFER_SAMPLES;
        }
        portEXIT_CRITICAL(&m_mux);
        return count;
    }

private:
    static void taskTrampoline(void* arg) {
        static_cast<AudioManager*>(arg)->taskLoop();
    }

    void taskLoop() {
        static int32_t samples[AUDIO_READ_SAMPLES * 2];
        uint32_t silentWindows = 0;

        for (;;) {
            size_t bytesRead = 0;
            esp_err_t err = i2s_read(AUDIO_I2S_PORT, samples,
                                     sizeof(samples), &bytesRead,
                                     pdMS_TO_TICKS(100));
            if (err != ESP_OK || bytesRead == 0) {
                portENTER_CRITICAL(&m_mux);
                m_stats.readErrors++;
                portEXIT_CRITICAL(&m_mux);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            const size_t slotCount = bytesRead / sizeof(int32_t);
            const size_t count = slotCount / 2;
            if (count == 0) continue;

            uint32_t leftNonZero = 0;
            uint32_t rightNonZero = 0;
            for (size_t i = 0; i < count; ++i) {
                leftNonZero += (samples[i * 2] >> 8) != 0;
                rightNonZero += (samples[i * 2 + 1] >> 8) != 0;
            }
            const bool useRight = rightNonZero > leftNonZero;
            // INMP441: 24-bit sample left-justified in 32 bits -> >>8.
            int64_t sumSquares = 0;
            int64_t sum = 0;
            int32_t peakAbs = 0;
            int32_t noisePeak = 0;
            int32_t minVal = INT32_MAX;
            int32_t maxVal = INT32_MIN;
            uint16_t clippedSamples = 0;
            uint16_t nonZeroSamples = 0;
            uint32_t rawPeak = 0;
            for (size_t i = 0; i < count; ++i) {
                const int32_t raw = samples[i * 2 + (useRight ? 1 : 0)];
                const int32_t v = raw >> 8;
                const uint32_t rawAbs = raw < 0 ? static_cast<uint32_t>(-(static_cast<int64_t>(raw))) : static_cast<uint32_t>(raw);
                if (rawAbs > rawPeak) rawPeak = rawAbs;
                sum += v;
                if (v != 0) nonZeroSamples++;
                const int32_t a = (v >= 0) ? v : -v;
                if (a > peakAbs) peakAbs = a;
                if (a > noisePeak) noisePeak = a;
                if (a >= 0x7FFFF0) clippedSamples++;
                if (v < minVal) minVal = v;
                if (v > maxVal) maxVal = v;
            }

            const int32_t dcOffset = static_cast<int32_t>(sum / static_cast<int64_t>(count));
            sumSquares = 0;
            for (size_t i = 0; i < count; ++i) {
                const int32_t centered = (samples[i * 2 + (useRight ? 1 : 0)] >> 8) - dcOffset;
                sumSquares += static_cast<int64_t>(centered) * centered;
            }
            const double rms = sqrt(static_cast<double>(sumSquares) / count);

            float dbSpl = 30.0f;
            if (rms > 1.0) {
                double dbFs = 20.0 * log10(rms / 8388607.0);
                dbSpl = static_cast<float>(max(30.0, dbFs + 120.0));
            }

            portENTER_CRITICAL(&m_mux);
            m_stats.totalSamples += count;
            m_stats.rmsLevel = static_cast<uint32_t>(min<double>(rms, 0x7FFFFF));
            m_stats.peakLevel = static_cast<uint32_t>(min<int32_t>(peakAbs, 0x7FFFFF));
            m_stats.noiseFloor = static_cast<uint32_t>(min<int32_t>(noisePeak, 0x7FFFFF));
            m_stats.dcOffset = dcOffset;
            m_stats.clippedSamples = clippedSamples;
            m_stats.nonZeroSamples = nonZeroSamples;
            m_stats.rawPeak = rawPeak;
            m_stats.activeChannel = useRight ? 'R' : 'L';
            m_stats.soundDbSpl = dbSpl;

            // RA 10173 data minimization: raw PCM buffering disabled at DMA layer
            m_pcmWrite = 0;
            m_pcmRead = 0;

            // Signal present when RMS clears the silence floor and the
            // window shows waveform variation (rejects DC-stuck lines).
            const bool hasVariation = (maxVal - minVal) > AUDIO_SILENCE_RMS_THRESHOLD;
            const bool audible = rms > AUDIO_SILENCE_RMS_THRESHOLD && hasVariation;
            silentWindows = audible ? 0 : (silentWindows < 100 ? silentWindows + 1 : 100);
            m_stats.receiving = silentWindows < 20; // ~320ms of grace
            portEXIT_CRITICAL(&m_mux);
        }
    }

    Stats m_stats;
    mutable portMUX_TYPE m_mux = portMUX_INITIALIZER_UNLOCKED;
    TaskHandle_t m_taskHandle = nullptr;
    uint32_t m_lastReport = 0;
    int16_t m_pcmRing[AUDIO_RING_BUFFER_SAMPLES] = {};
    size_t m_pcmRead = 0;
    size_t m_pcmReadRelay = 0;
    size_t m_pcmWrite = 0;
};

#endif // AUDIO_MANAGER_H
