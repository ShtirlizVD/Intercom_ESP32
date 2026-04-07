/*
 * main.cpp - ESP32 Intercom firmware
 *
 * Полнодуплексный интерком на базе ESP32:
 * - I2S микрофон (SPH0645LM4H-B)
 * - MAX98357A усилитель + динамик
 * - Кнопка для управления вызовами
 * - Wi-Fi связь с другим устройством
 * - Веб-интерфейс настройки
 *
 * Режимы работы:
 *   1. Телефонный (полнодуплекс) — кнопка: вызов/ответ/завершение
 *   2. Push-to-Talk (PTT) — удержание кнопки для передачи речи
 *
 * Автор: ShtirlizVD
 * Лицензия: MIT
 */

#include <Arduino.h>
#include "pins.h"
#include "config.h"
#include "audio.h"
#include "intercom.h"
#include "webui.h"
#include <LITTLEFS.h>
#include <WiFi.h>

// ==================== Глобальные переменные ====================

// Состояние кнопки
static volatile bool buttonPressed = false;
static volatile uint32_t buttonPressTime = 0;
static volatile bool buttonHeld = false;
static uint32_t buttonDebounceTime = 0;

// Флаги для задач
static volatile bool audioTaskRunning = false;

// ==================== Прерывания кнопки ====================

void IRAM_ATTR buttonISR() {
    buttonPressed = true;
    buttonPressTime = millis();
}

// ==================== Обработка кнопки ====================

void handleButton() {
    uint32_t now = millis();

    // Дебаунс
    if (buttonPressed && (now - buttonDebounceTime > BUTTON_DEBOUNCE_MS)) {
        buttonDebounceTime = now;
        buttonPressed = false;

        bool isDown = (digitalRead(BUTTON_PIN) == LOW);

        if (isDown && !buttonHeld) {
            // Кнопка нажата
            buttonHeld = true;
            Serial.println("[BTN] Нажата");

            CallState state = Intercom::getState();

            switch (state) {
                case CallState::IDLE:
                    // Начинаем вызов
                    Intercom::startCall();
                    break;

                case CallState::RINGING_IN:
                    // Отвечаем на входящий вызов
                    Intercom::answerCall();
                    break;

                case CallState::RINGING_OUT:
                    // Отменяем исходящий вызов
                    Intercom::endCall();
                    break;

                case CallState::IN_CALL:
                    // В телефонном режиме — завершаем вызов
                    // В PTT режиме — уже передаём (handled below)
                    if (Config::get().button_mode == BTN_MODE_PHONE) {
                        Intercom::endCall();
                    } else {
                        // PTT: кнопка нажата — начинаем передачу
                        Intercom::setPTTActive(true);
                    }
                    break;
            }
        } else if (!isDown && buttonHeld) {
            // Кнопка отпущена
            buttonHeld = false;
            Serial.println("[BTN] Отпущена");

            // В PTT режиме — прекращаем передачу
            if (Intercom::isCallActive() && Config::get().button_mode == BTN_MODE_PTT) {
                Intercom::setPTTActive(false);
            }
        }
    }
}

// ==================== Задача: Отправка аудио ====================

void audioSendTask(void* param) {
    Serial.println("[TASK] Задача отправки аудио запущена (Core 1)");

    // Буфер для сэмплов с микрофона
    int16_t micBuffer[FRAME_SAMPLES + 64];  // С запасом
    uint32_t lastSendTime = 0;
    uint32_t frameInterval = 1000000 / Config::get().sample_rate * FRAME_SAMPLES;  // мкс

    while (true) {
        // Проверяем, активен ли разговор и можно ли передавать
        if (Intercom::canTransmit()) {
            // Считываем кадр с микрофона
            int samplesRead = Audio::readFrame(micBuffer, FRAME_SAMPLES);

            if (samplesRead > 0) {
                // Отправляем по UDP
                Intercom::sendAudio(micBuffer, samplesRead);
            }

            lastSendTime = micros();
        } else {
            // Не передаём — но всё равно читаем из I2S чтобы не переполнился буфер
            if (Intercom::isCallActive()) {
                Audio::readFrame(micBuffer, FRAME_SAMPLES);
            }
        }

        // Расчёт задержки для поддержания частоты
        uint32_t elapsed = micros() - lastSendTime;
        if (elapsed < frameInterval) {
            vTaskDelay(pdMS_TO_TICKS((frameInterval - elapsed) / 1000));
        } else {
            // Если обработка заняла больше времени — не задерживаем
            taskYIELD();
        }
    }
}

// ==================== Задача: Приём аудио ====================

void audioReceiveTask(void* param) {
    Serial.println("[TASK] Задача приёма аудио запущена (Core 0)");

    // Буфер для принимаемых сэмплов
    int16_t spkBuffer[FRAME_SAMPLES + 64];
    uint32_t lastRecvTime = 0;
    uint32_t frameInterval = 1000000 / Config::get().sample_rate * FRAME_SAMPLES;  // мкс

    while (true) {
        if (Intercom::isCallActive()) {
            // Пробуем принять аудио-пакет
            int samplesRead = Intercom::receiveAudio(spkBuffer, FRAME_SAMPLES);

            if (samplesRead > 0) {
                // Воспроизводим через динамик
                Audio::writeFrame(spkBuffer, samplesRead);
                lastRecvTime = micros();
            } else {
                // Нет данных — небольшая пауза
                vTaskDelay(1);
            }
        } else {
            // Нет активного вызова — очищаем буфер динамика
            vTaskDelay(100);
        }
    }
}

// ==================== Задача: Сетевая ====================

void networkTask(void* param) {
    Serial.println("[TASK] Сетевая задача запущена");

    while (true) {
        // Обработка управляющих пакетов интеркома
        Intercom::update();

        // Обработка веб-запросов
        WebUI::handleClient();

        // Обработка кнопки
        handleButton();

        // Периодическая печать состояния (каждые 30 сек)
        static uint32_t lastStatusPrint = 0;
        if (millis() - lastStatusPrint > 30000) {
            lastStatusPrint = millis();
            Serial.printf("[STATUS] WiFi: %s | Вызов: %s | Heap: %d KB\n",
                          WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "нет",
                          Intercom::getStateName(),
                          ESP.getFreeHeap() / 1024);
        }

        vTaskDelay(pdMS_TO_TICKS(10));  // 100 Гц
    }
}

// ==================== Setup ====================

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n========================================");
    Serial.println("   ESP32 Intercom v1.0");
    Serial.println("   Полнодуплексный Wi-Fi интерком");
    Serial.println("========================================\n");

    // 1. Инициализация файловой системы
    if (!LITTLEFS.begin(true)) {
        Serial.println("[FS] Ошибка инициализации LITTLEFS!");
        // Продолжаем без файловой системы (будет использоваться встроенный HTML)
    } else {
        Serial.println("[FS] LITTLEFS инициализирована");
        // Проверяем наличие index.html
        if (LITTLEFS.exists("/index.html")) {
            Serial.println("[FS] /index.html найден");
        } else {
            Serial.println("[FS] /index.html не найден, используется встроенный HTML");
        }
    }

    // 2. Загрузка конфигурации
    Config::init();
    DeviceConfig& cfg = Config::get();

    // 3. Настройка LED и кнопки
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LED_OFF);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, CHANGE);

    Serial.printf("[SETUP] LED: GPIO%d | Кнопка: GPIO%d\n", LED_PIN, BUTTON_PIN);
    Serial.printf("[SETUP] Микрофон: BCK=%d WS=%d DIN=%d\n", I2S_MIC_BCK, I2S_MIC_WS, I2S_MIC_DIN);
    Serial.printf("[SETUP] Динамик:  BCK=%d WS=%d DOUT=%d\n", I2S_SPK_BCK, I2S_SPK_WS, I2S_SPK_DOUT);

    // 4. Инициализация I2S аудио
    if (!Audio::init()) {
        Serial.println("[SETUP] ОШИБКА: Не удалось инициализировать I2S!");
        // Продолжаем работу — пользователь может настроить через веб-интерфейс
    }

    // Применяем настройки усиления
    Audio::setMicGain(cfg.mic_gain);
    Audio::setVolume(cfg.spk_volume);

    // 5. Подключение к Wi-Fi / запуск AP
    if (Config::hasWiFi()) {
        if (!WebUI::startSTA()) {
            Serial.println("[SETUP] AP режим (не удалось подключиться к WiFi)");
        }
    } else {
        Serial.println("[SETUP] WiFi не настроен, запуск AP режима");
        WebUI::startAP();
    }

    // 6. Инициализация интеркома (UDP)
    if (!Intercom::init()) {
        Serial.println("[SETUP] ОШИБКА: Не удалось открыть UDP порты!");
    }

    // 7. Создание FreeRTOS задач
    // Задача приёма аудио на ядре 0 (процессорного ядра, обычно используется для Wi-Fi)
    xTaskCreatePinnedToCore(
        audioReceiveTask,    // Функция задачи
        "AudioRecv",         // Имя
        AUDIO_TASK_STACK,    // Размер стека
        NULL,                // Параметр
        3,                   // Приоритет
        NULL,                // Хэндл
        0                    // Ядро 0
    );

    // Задача отправки аудио на ядре 1
    xTaskCreatePinnedToCore(
        audioSendTask,
        "AudioSend",
        AUDIO_TASK_STACK,
        NULL,
        3,
        NULL,
        1                    // Ядро 1
    );

    // Сетевая задача на ядре 0 (низкий приоритет)
    xTaskCreatePinnedToCore(
        networkTask,
        "Network",
        NET_TASK_STACK,
        NULL,
        1,                   // Низкий приоритет
        NULL,
        0                    // Ядро 0
    );

    Serial.println("\n[SETUP] Инициализация завершена!\n");
    Serial.printf("[SETUP] Имя устройства: %s\n", cfg.device_name);
    Serial.printf("[SETUP] Режим кнопки: %s\n",
                  cfg.button_mode == BTN_MODE_PHONE ? "Телефон" : "PTT");
    if (Config::hasRemote()) {
        Serial.printf("[SETUP] Удалённый IP: %s (ctrl:%u, audio:%u)\n",
                      cfg.remote_ip, cfg.ctrl_port, cfg.audio_port);
    } else {
        Serial.println("[SETUP] Удалённый IP не задан!");
    }

    if (WebUI::isAPMode()) {
        Serial.println("\n========================================");
        Serial.println("  Подключитесь к Wi-Fi для настройки:");
        Serial.printf("  AP: %s_%s\n", AP_SSID_PREFIX, cfg.device_name);
        Serial.println("  Пароль: " AP_PASSWORD);
        Serial.println("  Откройте: http://192.168.4.1");
        Serial.println("========================================\n");
    } else {
        Serial.println("\n========================================");
        Serial.printf("  Web интерфейс: http://%s\n",
                      WiFi.localIP().toString().c_str());
        Serial.println("========================================\n");
    }
}

// ==================== Loop (не используется, всё в FreeRTOS) ====================

void loop() {
    // Основная логика работает в FreeRTOS задачах
    // Здесь только Watchdog feed
    vTaskDelay(pdMS_TO_TICKS(1000));
}
