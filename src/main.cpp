/*
 * main.cpp - ESP32 Intercom firmware v2.0
 *
 * Режим рации (Walkie-Talkie):
 *   - Нажал кнопку → речь СРАЗУ передаётся на другое устройство
 *   - Отпустил кнопку → передача прекращается, слушаешь ответ
 *   - Если на обоих устройствах нажаты кнопки → полнодуплекс (как по телефону)
 *
 * Аппаратное обеспечение:
 *   - ESP32 DevKit
 *   - SPH0645LM4H-B (I2S MEMS микрофон)
 *   - MAX98357A (I2S Class D усилитель + динамик)
 *   - Тактовая кнопка (GPIO4 → GND)
 *   - LED (GPIO2)
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

// ==================== Кнопка ====================

static volatile bool buttonEvent = false;   // Событие кнопки (прерывание)
static volatile uint32_t buttonEventTime = 0;
static bool buttonHeld = false;
static uint32_t buttonDebounceTime = 0;


// ==================== Прерывание ====================

void IRAM_ATTR buttonISR() {
    buttonEvent = true;
    buttonEventTime = millis();
}

// ==================== Обработка кнопки ====================

void handleButton() {
    uint32_t now = millis();

    if (buttonEvent && (now - buttonDebounceTime > BUTTON_DEBOUNCE_MS)) {
        buttonDebounceTime = now;
        buttonEvent = false;

        bool isDown = (digitalRead(BUTTON_PIN) == LOW);

        if (isDown && !buttonHeld) {
            // ======== НАЖАЛИ ========
            buttonHeld = true;
            Serial.println("[BTN] ▶ Нажата — передаю");

            // Сразу начинаем передачу аудио
            Intercom::pttPress();

        } else if (!isDown && buttonHeld) {
            // ======== ОТПУСТИЛИ ========
            buttonHeld = false;
            Serial.println("[BTN] ⏹ Отпущена — молчу, слушаю");

            // Прекращаем передачу
            Intercom::pttRelease();
        }
    }
}


// ==================== Задача: Отправка аудио ====================

void audioSendTask(void* param) {
    Serial.println("[TASK] Отправка аудио запущена (Core 1)");

    int16_t micBuffer[FRAME_SAMPLES + 64];
    uint32_t lastSendTime = 0;
    uint32_t frameInterval = 1000000UL / Config::get().sample_rate * FRAME_SAMPLES;

    while (true) {
        if (Intercom::canTransmit()) {
            // Считываем микрофон и отправляем
            int samplesRead = Audio::readFrame(micBuffer, FRAME_SAMPLES);
            if (samplesRead > 0) {
                Intercom::sendAudio(micBuffer, samplesRead);
            }
            lastSendTime = micros();
        } else {
            // Не передаём — читаем микрофон в пустоту чтобы не переполнился I2S буфер
            Audio::readFrame(micBuffer, FRAME_SAMPLES);
        }

        // Соблюдаем частоту кадров
        uint32_t elapsed = micros() - lastSendTime;
        if (elapsed < frameInterval) {
            vTaskDelay(pdMS_TO_TICKS((frameInterval - elapsed) / 1000));
        } else {
            taskYIELD();
        }
    }
}


// ==================== Задача: Приём аудио ====================

void audioReceiveTask(void* param) {
    Serial.println("[TASK] Приём аудио запущен (Core 0)");

    int16_t spkBuffer[FRAME_SAMPLES + 64];
    bool wasPlaying = false;

    while (true) {
        if (Intercom::shouldPlay()) {
            // Принимаем и воспроизводим
            int samplesRead = Intercom::receiveAudio(spkBuffer, FRAME_SAMPLES);
            if (samplesRead > 0) {
                Audio::writeFrame(spkBuffer, samplesRead);
                if (!wasPlaying) {
                    wasPlaying = true;
                    // Serial.println("[AUDIO] ▶ Воспроизведение...");
                }
            } else {
                // Нет пакета — короткая пауза
                vTaskDelay(1);
            }
        } else {
            // Ничего не воспроизводим — очищаем буфер динамика от шума
            if (wasPlaying) {
                wasPlaying = false;
                Audio::silenceSpeaker();
                // Serial.println("[AUDIO] ⏹ Тишина");
            }
            vTaskDelay(50);
        }
    }
}


// ==================== Задача: Сетевая ====================

void networkTask(void* param) {
    Serial.println("[TASK] Сетевая задача запущена");

    while (true) {
        // Обработка управляющих пакетов и LED
        Intercom::update();

        // Обработка веб-запросов
        WebUI::handleClient();

        // Обработка кнопки
        handleButton();

        // Статус каждые 30 сек
        static uint32_t lastStatus = 0;
        if (millis() - lastStatus > 30000) {
            lastStatus = millis();
            Serial.printf("[STATUS] WiFi: %s | Рация: %s | Peer: %s | Heap: %d KB\n",
                          WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "нет",
                          Intercom::getStateName(),
                          Intercom::remoteActive() ? "ON" : "OFF",
                          ESP.getFreeHeap() / 1024);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


// ==================== Setup ====================

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n========================================");
    Serial.println("   ESP32 Intercom v2.0");
    Serial.println("   Режим рации (Walkie-Talkie)");
    Serial.println("========================================\n");

    // 1. Файловая система
    if (!LITTLEFS.begin(true)) {
        Serial.println("[FS] Ошибка LITTLEFS (продолжаем без неё)");
    } else {
        Serial.println("[FS] LITTLEFS OK");
        if (LITTLEFS.exists("/index.html")) {
            Serial.println("[FS] /index.html найден");
        } else {
            Serial.println("[FS] /index.html не найден (используем встроенный)");
        }
    }

    // 2. Конфигурация
    Config::init();
    DeviceConfig& cfg = Config::get();

    // 3. GPIO
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LED_OFF);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, CHANGE);

    Serial.printf("[GPIO] LED=%d | Кнопка=%d\n", LED_PIN, BUTTON_PIN);
    Serial.printf("[I2S] Mic:  BCK=%d WS=%d DIN=%d\n", I2S_MIC_BCK, I2S_MIC_WS, I2S_MIC_DIN);
    Serial.printf("[I2S] Spk:  BCK=%d WS=%d DOUT=%d\n", I2S_SPK_BCK, I2S_SPK_WS, I2S_SPK_DOUT);

    // 4. Аудио
    if (!Audio::init()) {
        Serial.println("[AUDIO] ОШИБКА инициализации I2S!");
    }
    Audio::setMicGain(cfg.mic_gain);
    Audio::setVolume(cfg.spk_volume);

    // 5. WiFi
    if (Config::hasWiFi()) {
        if (!WebUI::startSTA()) {
            Serial.println("[SETUP] AP режим");
        }
    } else {
        Serial.println("[SETUP] WiFi не настроен → AP режим");
        WebUI::startAP();
    }

    // 6. UDP интерком
    if (!Intercom::init()) {
        Serial.println("[SETUP] ОШИБКА UDP!");
    }

    // 7. FreeRTOS задачи
    xTaskCreatePinnedToCore(audioReceiveTask, "AudioRecv", AUDIO_TASK_STACK, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(audioSendTask,   "AudioSend",  AUDIO_TASK_STACK, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(networkTask,     "Network",    NET_TASK_STACK,   NULL, 1, NULL, 0);

    // 8. Готово
    Serial.println("\n[SETUP] Готово!\n");
    Serial.printf("[SETUP] Имя: %s\n", cfg.device_name);
    if (Config::hasRemote()) {
        Serial.printf("[SETUP] Удалённый: %s (ctrl:%u audio:%u)\n",
                      cfg.remote_ip, cfg.ctrl_port, cfg.audio_port);
    } else {
        Serial.println("[SETUP] Удалённый IP не задан — настрояйте через веб!");
    }

    if (WebUI::isAPMode()) {
        Serial.println("\n========================================");
        Serial.printf("  Wi-Fi: %s_%s\n", AP_SSID_PREFIX, cfg.device_name);
        Serial.printf("  Пароль: %s\n", AP_PASSWORD);
        Serial.println("  URL: http://192.168.4.1");
        Serial.println("========================================\n");
    } else {
        Serial.println("\n========================================");
        Serial.printf("  URL: http://%s\n", WiFi.localIP().toString().c_str());
        Serial.println("========================================\n");
    }
}


// ==================== Loop ====================

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
