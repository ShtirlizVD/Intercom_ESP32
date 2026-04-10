/*
 * main.cpp - ESP32 Intercom firmware v2.3
 *
 * Режим рации (Walkie-Talkie):
 *   - Нажал кнопку → речь СРАЗУ передаётся
 *   - Отпустил → передача прекращается, слушаешь
 *   - Оба нажали → полнодуплекс
 *   - Пропущенное сообщение → LED мигает, кнопка воспроизведения
 *
 * Две сборки:
 *   USE_WIFI     — ESP32 DevKit + WiFi (дом)
 *   USE_ETHERNET — WT32-ETH01 v1.4 + Ethernet (гараж)
 *
 * Микрофон: ICS-43434 (24-бит I2S MEMS)
 * Динамик:  MAX98357A (I2S Class D)
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
#include <LittleFS.h>

#ifdef USE_ETHERNET
#include <ETH.h>
static bool ethConnected = false;
#else
#include <WiFi.h>
#endif

// ==================== Кнопка PTT ====================
static volatile bool buttonEvent = false;
static volatile uint32_t buttonEventTime = 0;
static bool buttonHeld = false;
static uint32_t buttonDebounceTime = 0;

void IRAM_ATTR buttonISR() {
    buttonEvent = true;
    buttonEventTime = millis();
}

void handleButton() {
    uint32_t now = millis();
    if (buttonEvent && (now - buttonDebounceTime > BUTTON_DEBOUNCE_MS)) {
        buttonDebounceTime = now;
        buttonEvent = false;

        bool isDown = (digitalRead(BUTTON_PIN) == LOW);
        if (isDown && !buttonHeld) {
            buttonHeld = true;
            // Если есть непрослушанная запись — очищаем при нажатии PTT
            Audio::clearRecording();
            Serial.println("[BTN] ▶ Нажата — передаю");
            Intercom::pttPress();
        } else if (!isDown && buttonHeld) {
            buttonHeld = false;
            Serial.println("[BTN] ⏹ Отпущена — молчу");
            Intercom::pttRelease();
        }
    }
}

// ==================== Кнопка воспроизведения ====================
static bool playbackLastState = HIGH;
static uint32_t playbackDebounceTime = 0;

void handlePlaybackButton() {
    uint32_t now = millis();
    if (now - playbackDebounceTime < PLAYBACK_DEBOUNCE_MS) return;

    bool state = digitalRead(PLAYBACK_PIN);
    if (state == LOW && playbackLastState == HIGH) {
        playbackDebounceTime = now;
        // Кнопка нажата
        if (Audio::hasRecording()) {
            if (!Audio::isPlaybackActive()) {
                Audio::startPlayback();
                Serial.printf("[BTN] 🔊 Воспроизведение (%u мс)\n",
                              Audio::getRecordingDuration());
            }
        } else {
            // Нет записи — короткий сигнал
            Serial.println("[BTN] Нет записи для воспроизведения");
        }
    }
    playbackLastState = state;
}

// ==================== Задача: Отправка аудио ====================
void audioSendTask(void* param) {
    Serial.println("[TASK] Отправка аудио запущена (Core 1)");

    int16_t micBuffer[FRAME_SAMPLES + 64];
    uint32_t lastSendTime = 0;
    uint32_t frameInterval = 1000000UL / Config::get().sample_rate * FRAME_SAMPLES;

    while (true) {
        if (Intercom::canTransmit()) {
            int samplesRead = Audio::readFrame(micBuffer, FRAME_SAMPLES);
            if (samplesRead > 0) {
                Intercom::sendAudio(micBuffer, samplesRead);
            }
            lastSendTime = micros();
        } else {
            Audio::readFrame(micBuffer, FRAME_SAMPLES);
        }

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
        // Приоритет: тональные сигналы > воспроизведение записи > входящий аудио-поток
        if (Audio::isTonePlaying()) {
            int toneSamples = Audio::getToneFrame(spkBuffer, FRAME_SAMPLES);
            if (toneSamples > 0) {
                Audio::writeFrame(spkBuffer, toneSamples);
                wasPlaying = true;
            } else {
                wasPlaying = false;
                Audio::silenceSpeaker();
            }
        } else if (Audio::isPlaybackActive()) {
            // Воспроизведение записанного сообщения
            int pbSamples = Audio::getPlaybackFrame(spkBuffer, FRAME_SAMPLES);
            if (pbSamples > 0) {
                Audio::writeFrame(spkBuffer, pbSamples);
                wasPlaying = true;
            } else {
                // Воспроизведение завершено — очистить запись
                Audio::clearRecording();
                Audio::silenceSpeaker();
                wasPlaying = false;
                Serial.println("[TASK] Воспроизведение завершено");
            }
        } else if (Intercom::shouldPlay()) {
            int samplesRead = Intercom::receiveAudio(spkBuffer, FRAME_SAMPLES);
            if (samplesRead > 0) {
                // Параллельно записываем в буфер
                if (Audio::isRecording()) {
                    Audio::recordSamples(spkBuffer, samplesRead);
                }
                Audio::writeFrame(spkBuffer, samplesRead);
                wasPlaying = true;
            } else {
                vTaskDelay(1);
            }
        } else {
            if (wasPlaying) {
                wasPlaying = false;
                Audio::silenceSpeaker();
            }
            vTaskDelay(50);
        }
    }
}

// ==================== Сетевая задача ====================
void networkTask(void* param) {
    Serial.println("[TASK] Сетевая задача запущена");

    while (true) {
        Intercom::update();
        WebUI::handleClient();
        handleButton();
        handlePlaybackButton();

        // Статус каждые 30 сек
        static uint32_t lastStatus = 0;
        if (millis() - lastStatus > 30000) {
            lastStatus = millis();
#ifdef USE_ETHERNET
            Serial.printf("[STATUS] ETH: %s | Рация: %s | Peer: %s | Heap: %d KB | Rec: %s\n",
                          ethConnected ? ETH.localIP().toString().c_str() : "нет",
                          Intercom::getStateName(),
                          Intercom::remoteActive() ? "ON" : "OFF",
                          ESP.getFreeHeap() / 1024,
                          Audio::hasRecording() ? "YES" : "no");
#else
            Serial.printf("[STATUS] WiFi: %s | Рация: %s | Peer: %s | Heap: %d KB | Rec: %s\n",
                          WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "нет",
                          Intercom::getStateName(),
                          Intercom::remoteActive() ? "ON" : "OFF",
                          ESP.getFreeHeap() / 1024,
                          Audio::hasRecording() ? "YES" : "no");
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ==================== Ethernet (WT32-ETH01) ====================
#ifdef USE_ETHERNET

static void onEthEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            Serial.println("[ETH] Старт");
            ETH.setHostname(Config::get().device_name);
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println("[ETH] Кабель подключён");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            Serial.printf("[ETH] Получен IP: %s\n", ETH.localIP().toString().c_str());
            Serial.printf("[ETH] Маска: %s | Шлюз: %s\n",
                          ETH.subnetMask().toString().c_str(),
                          ETH.gatewayIP().toString().c_str());
            ethConnected = true;
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            Serial.println("[ETH] Кабель отключён");
            ethConnected = false;
            break;
        case ARDUINO_EVENT_ETH_STOP:
            Serial.println("[ETH] Остановлен");
            ethConnected = false;
            break;
    }
}

static bool initEthernet() {
    Serial.println("[SETUP] Запуск Ethernet (WT32-ETH01 v1.4)...");
    WiFi.onEvent(onEthEvent);

    if (!ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_POWER,
                   ETH_MDC_PIN, ETH_MDIO_PIN, ETH_CLK_MODE)) {
        Serial.println("[ETH] ОШИБКА инициализации!");
        return false;
    }

    // Ждём IP по DHCP
    Serial.print("[ETH] Ожидание DHCP");
    int attempts = 0;
    while (!ethConnected && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (ethConnected) {
        // Устанавливаем имя по MAC если дефолтное
        Config::setDefaultName();
        Config::save();

        Serial.printf("[ETH] Подключён! IP: %s\n", ETH.localIP().toString().c_str());
        WebUI::init();
        return true;
    }

    Serial.println("[ETH] Не удалось получить IP по DHCP");
    return false;
}

#endif // USE_ETHERNET

// ==================== Setup ====================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n========================================");
    Serial.println("   ESP32 Intercom v2.3");
    Serial.println("   Режим рации (Walkie-Talkie)");
#ifdef USE_ETHERNET
    Serial.println("   WT32-ETH01 + Ethernet");
#else
    Serial.println("   ESP32 DevKit + WiFi");
#endif
    Serial.println("   Микрофон: ICS-43434 (24-бит)");
    Serial.println("   Динамик:  MAX98357A");
    Serial.println("   Запись последнего сообщения");
    Serial.println("========================================\n");

    // 1. Файловая система
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] Ошибка LittleFS");
    } else {
        Serial.println("[FS] LittleFS OK");
        if (LittleFS.exists("/index.html")) {
            Serial.println("[FS] /index.html найден");
        } else {
            Serial.println("[FS] /index.html не найден (встроенный)");
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
    pinMode(PLAYBACK_PIN, INPUT_PULLUP);

    Serial.printf("[GPIO] LED=%d | PTT=%d | Playback=%d\n", LED_PIN, BUTTON_PIN, PLAYBACK_PIN);
    Serial.printf("[I2S]  Mic: BCK=%d WS=%d DIN=%d\n", I2S_MIC_BCK, I2S_MIC_WS, I2S_MIC_DIN);
    Serial.printf("[I2S]  Spk: BCK=%d WS=%d DOUT=%d\n", I2S_SPK_BCK, I2S_SPK_WS, I2S_SPK_DOUT);

    // 4. Аудио
    if (!Audio::init()) {
        Serial.println("[AUDIO] ОШИБКА I2S!");
    }
    Audio::setMicGain(cfg.mic_gain);
    Audio::setVolume(cfg.spk_volume);

    // 5. Сеть: Ethernet или WiFi
#ifdef USE_ETHERNET
    initEthernet();
#else
    if (Config::hasWiFi()) {
        if (!WebUI::startSTA()) {
            Serial.println("[SETUP] AP режим");
        }
    } else {
        Serial.println("[SETUP] WiFi не настроен → AP режим");
        WebUI::startAP();
    }
#endif

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
        Serial.println("[SETUP] Удалённый IP не задан — настройте через веб!");
    }

    Serial.println("\n========================================");
#ifdef USE_ETHERNET
    if (ethConnected) {
        Serial.printf("  Ethernet: http://%s\n", ETH.localIP().toString().c_str());
    } else {
        Serial.println("  Ethernet: НЕ ПОДКЛЮЧЁН");
    }
#else
    if (WebUI::isAPMode()) {
        Serial.printf("  Wi-Fi: %s_%s\n", AP_SSID_PREFIX, cfg.device_name);
        Serial.printf("  Пароль: %s\n", AP_PASSWORD);
        Serial.println("  URL: http://192.168.4.1");
    } else {
        Serial.printf("  URL: http://%s\n", WiFi.localIP().toString().c_str());
    }
#endif
    Serial.println("========================================\n");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
