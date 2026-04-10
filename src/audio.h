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
 * Генератор звуковых сигналов (tone) для уведомлений.
 * Запись последнего принятого сообщения для воспроизведения.
 */

#include <cstdint>
#include <driver/i2s.h>

class Audio {
public:
    // Инициализация I2S портов (микрофон + динамик) + буфер записи
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

    // ===== Запись последнего сообщения =====

    // Начать запись входящего аудио (вызывается при tx_on от пира)
    static void startRecording();

    // Остановить запись (вызывается при tx_off от пира)
    static void stopRecording();

    // Записать сэмплы в буфер записи (вызывается из audioReceiveTask)
    static void recordSamples(const int16_t* samples, int count);

    // Есть ли записанное сообщение?
    static bool hasRecording();

    // Длительность записи в миллисекундах
    static uint32_t getRecordingDuration();

    // Воспроизвести записанное сообщение
    static bool startPlayback();

    // Воспроизводится ли сейчас запись?
    static bool isPlaybackActive();

    // Получить следующий кадр для воспроизведения записи
    // Возвращает количество сэмплов, 0 если воспроизведение закончилось
    static int getPlaybackFrame(int16_t* buffer, int maxSamples);

    // Очистить запись (после прослушивания или вручную)
    static void clearRecording();

    // Проверка: запись включена?
    static bool isRecording();

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

    // ===== Буфер записи последнего сообщения =====
    static int16_t* recBuffer;         // Динамически выделенный буфер
    static uint32_t recBufferCapacity; // Максимальное количество сэмплов
    static volatile uint32_t recPos;   // Текущая позиция записи
    static volatile uint32_t recLen;   // Общая длина записи (сэмплов)
    static volatile bool recActive;    // Идёт запись прямо сейчас
    static bool recAvailable;          // Есть записанное сообщение для прослушивания
    static uint32_t recStartTime;      // millis() начала записи

    // Воспроизведение записи
    static volatile uint32_t playPos;  // Текущая позиция воспроизведения
    static volatile bool playActive;   // Идёт воспроизведение
};
