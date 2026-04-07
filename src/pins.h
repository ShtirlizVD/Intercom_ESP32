#pragma once
/*
 * pins.h - Определение пинов для ESP32 Intercom
 *
 * Схема подключения:
 *
 * ESP32           SPH0645LM4H-B (I2S микрофон)
 * GPIO14 (BCK) -> BCLK
 * GPIO15 (WS)  -> LRCLK
 * GPIO32 (DIN) <- DOUT
 * 3.3V         -> VDD
 * GND          -> GND
 * 3.3V         -> L/R (Left channel)
 *
 * ESP32           MAX98357A (Усилитель)
 * GPIO26 (BCK) -> BCLK
 * GPIO27 (WS)  -> LRCLK
 * GPIO25 (DOUT)-> DIN
 * 3.3V         -> VDD
 * GND          -> GND
 *
 * ESP32           Кнопка
 * GPIO4  ----[]---- GND (с подтяжкой встроенной)
 *
 * ESP32           LED (индикатор)
 * GPIO2  -> LED -> 220Ω -> GND
 */

// ==================== I2S микрофон (I2S_NUM_0) ====================
#define I2S_MIC_PORT       I2S_NUM_0
#define I2S_MIC_BCK        14   // Bit Clock
#define I2S_MIC_WS         15   // Word Select (L/R Clock)
#define I2S_MIC_DIN        32   // Data In (от микрофона к ESP32)

// ==================== I2S динамик / MAX98357A (I2S_NUM_1) ====================
#define I2S_SPK_PORT       I2S_NUM_1
#define I2S_SPK_BCK        26   // Bit Clock
#define I2S_SPK_WS         27   // Word Select (L/R Clock)
#define I2S_SPK_DOUT       25   // Data Out (от ESP32 к MAX98357A)

// ==================== Кнопка вызова ====================
#define BUTTON_PIN         4    // GPIO кнопки (активный LOW)
#define BUTTON_DEBOUNCE_MS 50   // Дебаунс

// ==================== LED индикатор ====================
#define LED_PIN            2    // Встроенный LED на ESP32
#define LED_ON             LOW  // LOW = включён (общий катод)
#define LED_OFF            HIGH

// ==================== Аудио параметры ====================
#define SAMPLE_RATE        16000
#define BITS_PER_SAMPLE    16
#define FRAME_SIZE_MS      20
#define FRAME_SAMPLES      (SAMPLE_RATE * FRAME_SIZE_MS / 1000)  // 320 сэмплов
#define FRAME_BYTES        (FRAME_SAMPLES * BITS_PER_SAMPLE / 8) // 640 байт

// ==================== Сетевые параметры ====================
#define DEFAULT_CTRL_PORT  8080  // Порт управляющих сообщений
#define DEFAULT_AUDIO_PORT 8081  // Порт аудио потока
#define UDP_TIMEOUT_MS     50    // Таймаут чтения UDP
#define RING_TIMEOUT_MS    30000 // Таймаут вызова (30 сек)
#define CALL_TIMEOUT_MS    15000 // Таймаут отсутствия пакетов (15 сек)

// ==================== WEB интерфейс ====================
#define WEB_PORT           80
#define AP_SSID_PREFIX     "Intercom_ESP32"
#define AP_PASSWORD        "12345678"

// ==================== Задачи FreeRTOS ====================
#define AUDIO_TASK_STACK   4096
#define NET_TASK_STACK     2048
