#pragma once
/*
 * audio.h - Модуль аудио ввода/вывода через I2S
 *
 * Использует два I2S порта ESP32:
 * - I2S_NUM_0: Ввод от I2S микрофона (ICS-43434)
 * - I2S_NUM_1: Вывод на MAX98357A усилитель
 *
 * Микрофон выдаёт 24-битные I2S данные в 32-битном кадре.
 * Мы извлекаем 16-битные сэмплы для передачи.
 * Динамик принимает 16-битные сэмплы напрямую.
 *
 * Также содержит генератор звуковых сигналов (tone) для
 * уведомлений: сигнал ошибки при недоступности пира.
 */

#include <cstdint>
#include <driver/i2s.h>

class Audio {
public:
    // Инициализация I2S портов (микрофон + динамик)
    static bool init();

    // Деинициализация I2S
    static void deinit();

    // Прочитать кадр аудио с микрофона
    // Возвращает количество сэмплов (16-бит, моно)
    static int readFrame(int16_t* buffer, int maxSamples);

    // Записать кадр аудио на динамик
    // Возвращает количество записанных сэмплов
    static int writeFrame(const int16_t* buffer, int samples);

    // Пропустить воспроизведение (очистить буфер)
    static void silenceSpeaker();

    // Управление усилением
    static void setMicGain(uint8_t gain);     // 0-10
    static void setVolume(uint8_t volume);    // 0-10

    // Получить текущие значения
    static uint8_t getMicGain();
    static uint8_t getVolume();

    // Получить информацию о размерах кадров
    static int getFrameSamples();
    static int getFrameBytes();
    static uint32_t getSampleRate();

    // Переконфигурация частоты дискретизации
    static bool setSampleRate(uint32_t sampleRate);

    // ===== Генератор звуковых сигналов =====

    // Воспроизвести сигнал ошибки (пир не в сети) — двухтональный "ду-ду"
    static void playErrorTone();

    // Воспроизвести сигнал подтверждения (короткий бип)
    static void playConfirmTone();

    // Воспроизвести сигнал отмены (собеседник отключился во время передачи)
    static void playCancelTone();

    // Сейчас воспроизводится тональный сигнал?
    static bool isTonePlaying();

    // Сгенерировать следующий кадр тонального сигнала (для audioReceiveTask)
    // Возвращает количество сэмплов, 0 если сигнал закончился
    static int getToneFrame(int16_t* buffer, int maxSamples);

private:
    static float micGainMult;   // Множитель усиления микрофона
    static float volumeMult;    // Множитель громкости
    static uint32_t currentSampleRate;
    static bool initialized;

    // Буфер для сырых 32-битных данных с микрофона
    static const int RAW_BUF_SIZE = 1024;
    static int32_t rawMicBuffer[RAW_BUF_SIZE];

    // ===== Генератор тонов (state machine) =====
    enum ToneType : uint8_t {
        TONE_NONE = 0,      // Нет сигнала
        TONE_ERROR,         // "Ду-ду" — пир оффлайн
        TONE_CONFIRM,       // "Бип" — подтверждение
        TONE_CANCEL         // "Да-да-да" — отмена во время передачи
    };

    enum TonePhase : uint8_t {
        PHASE_OFF = 0,      // Выкл / пауза между частями
        PHASE_BEEP,         // Тональный сигнал
        PHASE_SILENCE       // Тихая пауза
    };

    // Параметры сигнала
    struct ToneStep {
        uint16_t freq;          // Частота в Гц (0 = тишина)
        uint16_t duration_ms;   // Длительность в мс
    };

    static volatile ToneType toneType;      // Текущий тип сигнала
    static TonePhase tonePhase;             // Текущая фаза
    static uint16_t toneFreq;               // Текущая частота
    static uint32_t tonePhaseStart;         // millis() старта фазы
    static uint32_t toneStepIndex;          // Индекс текущего шага
    static const ToneStep* toneSequence;    // Указатель на массив шагов
    static uint32_t toneSequenceLen;        // Длина массива шагов
    static uint32_t toneSamplePhase;        // Фаза для генерации синуса
    static int16_t toneVolume;              // Громкость сигнала (0-32767)

    // Шаблоны сигналов
    static const ToneStep errorSequence[];
    static const ToneStep confirmSequence[];
    static const ToneStep cancelSequence[];

    static void startTone(ToneType type, const ToneStep* seq, uint32_t len);
    static void advanceTonePhase();
};
