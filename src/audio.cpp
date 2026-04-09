/*
 * audio.cpp - Реализация модуля аудио I2S
 *
 * Работает с двумя I2S портами ESP32:
 * - I2S_NUM_0 (RX): Ввод от MEMS I2S микрофона
 * - I2S_NUM_1 (TX): Вывод на MAX98357A
 */

#include "audio.h"
#include "config.h"
#include "pins.h"
#include <Arduino.h>

// Статические члены
float Audio::micGainMult = 1.0f;
float Audio::volumeMult = 0.7f;
uint32_t Audio::currentSampleRate = SAMPLE_RATE;
bool Audio::initialized = false;
int32_t Audio::rawMicBuffer[Audio::RAW_BUF_SIZE];

bool Audio::init() {
    if (initialized) {
        deinit();
    }

    DeviceConfig& cfg = Config::get();
    currentSampleRate = cfg.sample_rate;
    micGainMult = cfg.mic_gain / 5.0f;   // 5 = среднее значение → множитель 1.0
    volumeMult = cfg.spk_volume / 10.0f;

    esp_err_t err;

    // ==================== I2S порт 0: Микрофон ICS-43434 (RX) ====================
    i2s_config_t i2s_mic_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = currentSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // ICS-43434: 24-бит в 32-бит кадре
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,    // Только левый канал
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
        Serial.printf("[AUDIO] Ошибка установки драйвера микрофона: %d\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_MIC_PORT, &i2s_mic_pins);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] Ошибка настройки пинов микрофона: %d\n", err);
        i2s_driver_uninstall(I2S_MIC_PORT);
        return false;
    }

    // ==================== I2S порт 1: Динамик MAX98357A (TX) ====================
    i2s_config_t i2s_spk_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = currentSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,  // MAX98357A работает с 16-бит
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,    // Только левый канал
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = true,  // Автоочистка для предотвращения заикания
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
        Serial.printf("[AUDIO] Ошибка установки драйвера динамика: %d\n", err);
        i2s_driver_uninstall(I2S_MIC_PORT);
        return false;
    }

    err = i2s_set_pin(I2S_SPK_PORT, &i2s_spk_pins);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] Ошибка настройки пинов динамика: %d\n", err);
        i2s_driver_uninstall(I2S_MIC_PORT);
        i2s_driver_uninstall(I2S_SPK_PORT);
        return false;
    }

    // Очистить буфер динамика от шума
    i2s_zero_dma_buffer(I2S_SPK_PORT);

    initialized = true;
    Serial.printf("[AUDIO] Инициализация I2S прошла успешно (SR=%u Hz)\n", currentSampleRate);
    return true;
}

void Audio::deinit() {
    if (!initialized) return;
    i2s_driver_uninstall(I2S_MIC_PORT);
    i2s_driver_uninstall(I2S_SPK_PORT);
    initialized = false;
    Serial.println("[AUDIO] I2S драйверы удалены");
}

int Audio::readFrame(int16_t* buffer, int maxSamples) {
    if (!initialized) return 0;

    // Ограничиваем размер чтения буфером
    int samplesToRead = min(maxSamples, RAW_BUF_SIZE);
    size_t bytesRead = 0;

    // Читаем 32-битные I2S кадры с микрофона
    esp_err_t err = i2s_read(I2S_MIC_PORT, rawMicBuffer,
                              samplesToRead * sizeof(int32_t),
                              &bytesRead, pdMS_TO_TICKS(100));

    if (err != ESP_OK) {
        return 0;
    }

    int samplesRead = bytesRead / sizeof(int32_t);

    // Конвертация 32-бит → 16-бит с извлечением данных микрофона
    // ICS-43434 передаёт 24-битные данные в старших битах 32-битного I2S слова.
    // Данные занимают позиции [31:8], 8 младших бит — мусор.
    for (int i = 0; i < samplesRead; i++) {
        int32_t raw = rawMicBuffer[i];
        int32_t sample = raw >> 8;  // Извлекаем 24-битные данные
        // Ограничиваем диапазон
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        // Применяем усиление микрофона
        float amplified = (float)sample * micGainMult;
        if (amplified > 32767.0f) amplified = 32767.0f;
        if (amplified < -32768.0f) amplified = -32768.0f;
        buffer[i] = (int16_t)amplified;
    }

    return samplesRead;
}

int Audio::writeFrame(const int16_t* buffer, int samples) {
    if (!initialized || samples <= 0) return 0;

    // Применяем громкость
    int16_t volBuffer[samples];  // VLA — корректно для ESP32/GCC
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
    // Записываем нули для очистки буфера DMA
    int16_t silence[128] = {0};
    size_t written;
    for (int i = 0; i < 10; i++) {
        i2s_write(I2S_SPK_PORT, silence, sizeof(silence), &written, pdMS_TO_TICKS(10));
    }
    i2s_zero_dma_buffer(I2S_SPK_PORT);
}

void Audio::setMicGain(uint8_t gain) {
    // gain: 0-10, маппируем в множитель 0.0 - 4.0
    micGainMult = gain / 5.0f;  // gain=5 → 1.0, gain=10 → 2.0, gain=0 → 0.0
    if (micGainMult > 4.0f) micGainMult = 4.0f;
    Serial.printf("[AUDIO] Усиление микрофона: %d → %.2f\n", gain, micGainMult);
}

void Audio::setVolume(uint8_t volume) {
    // volume: 0-10, маппируем в множитель 0.0 - 1.0
    volumeMult = volume / 10.0f;
    Serial.printf("[AUDIO] Громкость: %d → %.2f\n", volume, volumeMult);
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

    // Поддерживаемые частоты
    if (sampleRate != 8000 && sampleRate != 16000 &&
        sampleRate != 22050 && sampleRate != 44100) {
        Serial.printf("[AUDIO] Неподдерживаемая частота: %u\n", sampleRate);
        return false;
    }

    currentSampleRate = sampleRate;

    if (initialized) {
        // Обновляем частоту на обоих портах
        i2s_set_sample_rates(I2S_MIC_PORT, currentSampleRate);
        i2s_set_sample_rates(I2S_SPK_PORT, currentSampleRate);
        Serial.printf("[AUDIO] Частота дискретизации изменена: %u Hz\n", currentSampleRate);
    }

    return true;
}


// ==================== Генератор звуковых сигналов ====================

// Статические члены генератора тонов
volatile Audio::ToneType Audio::toneType = Audio::TONE_NONE;
Audio::TonePhase Audio::tonePhase = Audio::PHASE_OFF;
uint16_t Audio::toneFreq = 0;
uint32_t Audio::tonePhaseStart = 0;
uint32_t Audio::toneStepIndex = 0;
const Audio::ToneStep* Audio::toneSequence = nullptr;
uint32_t Audio::toneSequenceLen = 0;
uint32_t Audio::toneSamplePhase = 0;
int16_t Audio::toneVolume = 20000;  // ~60% громкости

// Шаблоны сигналов (частота, длительность_мс)
// Сигнал ошибки: нисходящий "ду-ду" — пир не в сети
const Audio::ToneStep Audio::errorSequence[] = {
    {480,  200},   // "Ду"  — 480 Гц, 200 мс
    {0,    80},    // Пауза 80 мс
    {380,  200},   // "Ду"  — 380 Гц, 200 мс
    {0,    100},   // Пауза
    {300,  300},   // "Дуу" — 300 Гц, 300 мс (подтверждение)
};

// Сигнал подтверждения: короткий бип
const Audio::ToneStep Audio::confirmSequence[] = {
    {800,  100},   // Короткий высокий бип
    {0,    50},    // Пауза
    {1000, 100},   // Ещё один
};

// Сигнал отмены: три быстрых нисходящих тона
const Audio::ToneStep Audio::cancelSequence[] = {
    {600,  120},   // Да
    {0,    60},    // Пауза
    {450,  120},   // да
    {0,    60},    // Пауза
    {300,  200},   // да!
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
    Serial.println("[AUDIO] 🔔 Сигнал ошибки: пир не в сети");
    startTone(TONE_ERROR, errorSequence, sizeof(errorSequence) / sizeof(errorSequence[0]));
}

void Audio::playConfirmTone() {
    Serial.println("[AUDIO] 🔔 Сигнал подтверждения");
    startTone(TONE_CONFIRM, confirmSequence, sizeof(confirmSequence) / sizeof(confirmSequence[0]));
}

void Audio::playCancelTone() {
    Serial.println("[AUDIO] 🔔 Сигнал отмены: связь потеряна");
    startTone(TONE_CANCEL, cancelSequence, sizeof(cancelSequence) / sizeof(cancelSequence[0]));
}

bool Audio::isTonePlaying() {
    return toneType != TONE_NONE;
}

void Audio::advanceTonePhase() {
    toneStepIndex++;
    if (toneStepIndex >= toneSequenceLen) {
        // Сигнал завершён
        toneType = TONE_NONE;
        tonePhase = PHASE_OFF;
        toneFreq = 0;
        return;
    }
    tonePhase = (toneSequence[toneStepIndex].freq > 0) ? PHASE_BEEP : PHASE_SILENCE;
    toneFreq = toneSequence[toneStepIndex].freq;
    tonePhaseStart = millis();
    toneSamplePhase = 0;  // Сброс фазы синуса для чистоты
}

int Audio::getToneFrame(int16_t* buffer, int maxSamples) {
    if (toneType == TONE_NONE) return 0;

    uint32_t now = millis();
    uint32_t stepDuration = toneSequence[toneStepIndex].duration_ms;
    uint32_t elapsed = now - tonePhaseStart;
    uint32_t remainingMs = (elapsed < stepDuration) ? (stepDuration - elapsed) : 0;

    // Сколько сэмплов осталось до конца текущего шага
    uint32_t remainingSamples = (uint32_t)currentSampleRate * remainingMs / 1000;
    int samplesToGen = min(maxSamples, (int)remainingSamples);

    if (samplesToGen <= 0) {
        // Текущий шаг закончился, переходим к следующему
        advanceTonePhase();
        if (toneType == TONE_NONE) return 0;  // Весь сигнал завершён
        // Генерируем хотя бы немного для нового шага
        samplesToGen = min(maxSamples, (int)((uint32_t)currentSampleRate * toneSequence[toneStepIndex].duration_ms / 1000));
        if (samplesToGen <= 0) samplesToGen = maxSamples;  // На всякий случай
    }

    uint16_t freq = toneFreq;

    if (freq == 0) {
        // Тишина (пауза)
        memset(buffer, 0, samplesToGen * sizeof(int16_t));
    } else {
        // Генерация синусоиды
        // phase_increment = (freq * 2^16) / sample_rate  (фиксированная точка Q16.16)
        uint32_t phaseInc = ((uint32_t)freq << 16) / currentSampleRate;

        for (int i = 0; i < samplesToGen; i++) {
            // Вычисляем синус через таблицу (быстрая аппроксимация)
            uint16_t phase = (uint16_t)(toneSamplePhase >> 16);
            // Таблица синуса: 256 записей (0..65535)
            // sin(x) ≈ 1 - 2*|x - 0.5| для треугольной волны (чище для маленьких динамиков)
            // Используем улучшенную аппроксимацию через квадратичную интерполяцию
            int32_t val;
            uint8_t p8 = phase >> 8;  // 0..255
            if (p8 < 64) {
                val = p8 * 4;  // 0 → 255
            } else if (p8 < 128) {
                val = 511 - p8 * 4;  // 255 → 0
            } else if (p8 < 192) {
                val = -(p8 * 4 - 767);  // 0 → -255
            } else {
                val = p8 * 4 - 1023;  // -255 → 0
            }
            // Масштабируем к громкости сигнала
            buffer[i] = (int16_t)((int32_t)val * toneVolume / 256);
            toneSamplePhase += phaseInc;
        }
    }

    return samplesToGen;
}
