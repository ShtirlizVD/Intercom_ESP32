/*
 * audio.cpp - I2S audio module implementation
 *
 * Works with two ESP32 I2S ports:
 * - I2S_NUM_0 (RX): Input from MEMS I2S microphone
 * - I2S_NUM_1 (TX): Output to MAX98357A
 */

#include "audio.h"
#include "config.h"
#include "pins.h"
#include <Arduino.h>

// Static members
float Audio::micGainMult = 1.0f;
float Audio::volumeMult = 0.7f;
uint32_t Audio::currentSampleRate = SAMPLE_RATE;
bool Audio::initialized = false;
int32_t Audio::rawMicBuffer[Audio::RAW_BUF_SIZE];

int Audio::readFrame(int16_t* buffer, int maxSamples) {
    if (!initialized) return 0;

    int samplesToRead = min(maxSamples, RAW_BUF_SIZE);
    size_t bytesRead = 0;

    esp_err_t err = i2s_read(I2S_MIC_PORT, rawMicBuffer,
                              samplesToRead * sizeof(int32_t),
                              &bytesRead, pdMS_TO_TICKS(100));

    if (err != ESP_OK) {
        return 0;
    }

    int samplesRead = bytesRead / sizeof(int32_t);

    // Convert 32-bit -> 16-bit, extracting mic data
    // ICS-43434 sends 24-bit data in upper bits of 32-bit I2S word.
    // Data occupies bits [31:8], lower 8 bits are padding.
    for (int i = 0; i < samplesRead; i++) {
        int32_t raw = rawMicBuffer[i];
        int32_t sample = raw >> 8;  // Extract 24-bit data
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        // Apply mic gain
        float amplified = (float)sample * micGainMult;
        if (amplified > 32767.0f) amplified = 32767.0f;
        if (amplified < -32768.0f) amplified = -32768.0f;
        buffer[i] = (int16_t)amplified;
    }

    return samplesRead;
}

int Audio::writeFrame(const int16_t* buffer, int samples) {
    if (!initialized || samples <= 0) return 0;

    int16_t volBuffer[samples];  // VLA
    for (int i = 0; i < samples; i++) {
        float scaled = (float)buffer[i] * volumeMult;
        if (scaled > 32767.0f) scaled = 32767.0f;
        if (scaled < -32768.0f) scaled = -32768.0f;
        volBuffer[i] = (int16_t)scaled;
    }

    size_t bytesWritten = 0;
    esp_err_t err = i2s_write(I2S_SPK_PORT, volBuffer,
                               samples * sizeof(int16_t),
                               &bytesWritten, pdMS_TO_TICKS(100));

    if (err != ESP_OK) {
        return 0;
    }

    return bytesWritten / sizeof(int16_t);
}

void Audio::silenceSpeaker() {
    if (!initialized) return;
    int16_t silence[128] = {0};
    size_t written;
    for (int i = 0; i < 10; i++) {
        i2s_write(I2S_SPK_PORT, silence, sizeof(silence), &written, pdMS_TO_TICKS(10));
    }
    i2s_zero_dma_buffer(I2S_SPK_PORT);
}

void Audio::setMicGain(uint8_t gain) {
    micGainMult = gain / 5.0f;
    if (micGainMult > 4.0f) micGainMult = 4.0f;
    Serial.printf("[AUDIO] Mic gain: %d -> %.2f\n", gain, micGainMult);
}

void Audio::setVolume(uint8_t volume) {
    volumeMult = volume / 10.0f;
    Serial.printf("[AUDIO] Volume: %d -> %.2f\n", volume, volumeMult);
}

uint8_t Audio::getMicGain() {
    return (uint8_t)(micGainMult * 5.0f + 0.5f);
}

uint8_t Audio::getVolume() {
    return (uint8_t)(volumeMult * 10.0f + 0.5f);
}

int Audio::getFrameSamples() {
    return currentSampleRate * FRAME_SIZE_MS / 1000;
}

int Audio::getFrameBytes() {
    return getFrameSamples() * sizeof(int16_t);
}

uint32_t Audio::getSampleRate() {
    return currentSampleRate;
}

bool Audio::setSampleRate(uint32_t sampleRate) {
    if (sampleRate == currentSampleRate && initialized) return true;

    if (sampleRate != 8000 && sampleRate != 16000 &&
        sampleRate != 22050 && sampleRate != 44100) {
        Serial.printf("[AUDIO] Unsupported rate: %u\n", sampleRate);
        return false;
    }

    currentSampleRate = sampleRate;

    if (initialized) {
        i2s_set_sample_rates(I2S_MIC_PORT, currentSampleRate);
        i2s_set_sample_rates(I2S_SPK_PORT, currentSampleRate);
        Serial.printf("[AUDIO] Sample rate changed: %u Hz\n", currentSampleRate);
    }

    return true;
}


// ==================== Tone Generator ====================

volatile Audio::ToneType Audio::toneType = Audio::TONE_NONE;
Audio::TonePhase Audio::tonePhase = Audio::PHASE_OFF;
uint16_t Audio::toneFreq = 0;
uint32_t Audio::tonePhaseStart = 0;
uint32_t Audio::toneStepIndex = 0;
const Audio::ToneStep* Audio::toneSequence = nullptr;
uint32_t Audio::toneSequenceLen = 0;
uint32_t Audio::toneSamplePhase = 0;
int16_t Audio::toneVolume = 20000;

// Tone patterns (frequency, duration_ms)
const Audio::ToneStep Audio::errorSequence[] = {
    {480,  200},
    {0,    80},
    {380,  200},
    {0,    100},
    {300,  300},
};

const Audio::ToneStep Audio::confirmSequence[] = {
    {800,  100},
    {0,    50},
    {1000, 100},
};

const Audio::ToneStep Audio::cancelSequence[] = {
    {600,  120},
    {0,    60},
    {450,  120},
    {0,    60},
    {300,  200},
};

// Test tone — pleasant two-tone melody to verify speaker works
const Audio::ToneStep Audio::testSequence[] = {
    {523,  200},   // C5
    {0,    80},
    {659,  200},   // E5
    {0,    80},
    {784,  200},   // G5
    {0,    80},
    {1047, 400},   // C6
    {0,    100},
};

void Audio::startTone(ToneType type, const ToneStep* seq, uint32_t len) {
    toneType = type;
    tonePhase = PHASE_BEEP;
    toneSequence = seq;
    toneSequenceLen = len;
    toneStepIndex = 0;
    tonePhaseStart = millis();
    toneSamplePhase = 0;
    toneFreq = seq[0].freq;
}

void Audio::playErrorTone() {
    Serial.println("[AUDIO] Error tone: peer offline");
    startTone(TONE_ERROR, errorSequence, sizeof(errorSequence) / sizeof(errorSequence[0]));
}

void Audio::playConfirmTone() {
    Serial.println("[AUDIO] Confirm tone");
    startTone(TONE_CONFIRM, confirmSequence, sizeof(confirmSequence) / sizeof(confirmSequence[0]));
}

void Audio::playCancelTone() {
    Serial.println("[AUDIO] Cancel tone: link lost");
    startTone(TONE_CANCEL, cancelSequence, sizeof(cancelSequence) / sizeof(cancelSequence[0]));
}

void Audio::playTestTone() {
    Serial.println("[AUDIO] Test tone: speaker check");
    startTone(TONE_TEST, testSequence, sizeof(testSequence) / sizeof(testSequence[0]));

    // Play in blocking mode — spin until tone finishes
    int16_t toneBuf[512];
    while (isTonePlaying()) {
        int n = getToneFrame(toneBuf, 512);
        if (n > 0) {
            writeFrame(toneBuf, n);
        }
    }
    silenceSpeaker();
    Serial.println("[AUDIO] Test tone done");
}

bool Audio::isTonePlaying() {
    return toneType != TONE_NONE;
}

void Audio::advanceTonePhase() {
    toneStepIndex++;
    if (toneStepIndex >= toneSequenceLen) {
        toneType = TONE_NONE;
        tonePhase = PHASE_OFF;
        toneFreq = 0;
        return;
    }
    tonePhase = (toneSequence[toneStepIndex].freq > 0) ? PHASE_BEEP : PHASE_SILENCE;
    toneFreq = toneSequence[toneStepIndex].freq;
    tonePhaseStart = millis();
    toneSamplePhase = 0;
}

int Audio::getToneFrame(int16_t* buffer, int maxSamples) {
    if (toneType == TONE_NONE) return 0;

    uint32_t now = millis();
    uint32_t stepDuration = toneSequence[toneStepIndex].duration_ms;
    uint32_t elapsed = now - tonePhaseStart;
    uint32_t remainingMs = (elapsed < stepDuration) ? (stepDuration - elapsed) : 0;

    uint32_t remainingSamples = (uint32_t)currentSampleRate * remainingMs / 1000;
    int samplesToGen = min(maxSamples, (int)remainingSamples);

    if (samplesToGen <= 0) {
        advanceTonePhase();
        if (toneType == TONE_NONE) return 0;
        samplesToGen = min(maxSamples, (int)((uint32_t)currentSampleRate * toneSequence[toneStepIndex].duration_ms / 1000));
        if (samplesToGen <= 0) samplesToGen = maxSamples;
    }

    uint16_t freq = toneFreq;

    if (freq == 0) {
        memset(buffer, 0, samplesToGen * sizeof(int16_t));
    } else {
        uint32_t phaseInc = ((uint32_t)freq << 16) / currentSampleRate;

        for (int i = 0; i < samplesToGen; i++) {
            uint16_t phase = (uint16_t)(toneSamplePhase >> 16);
            int32_t val;
            uint8_t p8 = phase >> 8;
            if (p8 < 64) {
                val = p8 * 4;
            } else if (p8 < 128) {
                val = 511 - p8 * 4;
            } else if (p8 < 192) {
                val = -(p8 * 4 - 767);
            } else {
                val = p8 * 4 - 1023;
            }
            buffer[i] = (int16_t)((int32_t)val * toneVolume / 256);
            toneSamplePhase += phaseInc;
        }
    }

    return samplesToGen;
}


// ==================== I2S Initialization ====================

bool Audio::init() {
    if (initialized) {
        deinit();
    }

    DeviceConfig& cfg = Config::get();
    currentSampleRate = cfg.sample_rate;
    micGainMult = cfg.mic_gain / 5.0f;
    volumeMult = cfg.spk_volume / 10.0f;

    esp_err_t err;

    // ==================== I2S port 0: Mic ICS-43434 (RX) ====================
    i2s_config_t i2s_mic_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = currentSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t i2s_mic_pins = {
        .bck_io_num = I2S_MIC_BCK,
        .ws_io_num = I2S_MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_MIC_DIN
    };

    err = i2s_driver_install(I2S_MIC_PORT, &i2s_mic_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] Mic driver install error: %d\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_MIC_PORT, &i2s_mic_pins);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] Mic pin config error: %d\n", err);
        i2s_driver_uninstall(I2S_MIC_PORT);
        return false;
    }

    // ==================== I2S port 1: Speaker MAX98357A (TX) ====================
    i2s_config_t i2s_spk_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = currentSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t i2s_spk_pins = {
        .bck_io_num = I2S_SPK_BCK,
        .ws_io_num = I2S_SPK_WS,
        .data_out_num = I2S_SPK_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    err = i2s_driver_install(I2S_SPK_PORT, &i2s_spk_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] Speaker driver install error: %d\n", err);
        i2s_driver_uninstall(I2S_MIC_PORT);
        return false;
    }

    err = i2s_set_pin(I2S_SPK_PORT, &i2s_spk_pins);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] Speaker pin config error: %d\n", err);
        i2s_driver_uninstall(I2S_MIC_PORT);
        i2s_driver_uninstall(I2S_SPK_PORT);
        return false;
    }

    i2s_zero_dma_buffer(I2S_SPK_PORT);

    initialized = true;
    Serial.printf("[AUDIO] I2S init OK (SR=%u Hz)\n", currentSampleRate);
    return true;
}

void Audio::deinit() {
    if (!initialized) return;
    i2s_driver_uninstall(I2S_MIC_PORT);
    i2s_driver_uninstall(I2S_SPK_PORT);
    initialized = false;
    Serial.println("[AUDIO] I2S drivers removed");
}
