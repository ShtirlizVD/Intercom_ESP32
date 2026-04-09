#pragma once
/*
 * audio.h - Модуль аудио ввода/вывода через I2S
 *
 * Использует два I2S порта ESP32:
 * - I2S_NUM_0: Ввод от I2S микрофона (SPH0645LM4H-B)
 * - I2S_NUM_1: Вывод на MAX98357A усилитель
 *
 * Микрофон выдаёт 32-битные I2S кадры с 18-битными данными.
 * Мы извлекаем 16-битные сэмплы для передачи.
 * Динамик принимает 16-битные сэмплы напрямую.
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

private:
    static float micGainMult;   // Множитель усиления микрофона
    static float volumeMult;    // Множитель громкости
    static uint32_t currentSampleRate;
    static bool initialized;

    // Буфер для сырых 32-битных данных с микрофона
    static const int RAW_BUF_SIZE = 1024;
    static int32_t rawMicBuffer[RAW_BUF_SIZE];
};
