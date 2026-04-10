/*
 * main.cpp - ESP32 Intercom firmware v2.5
 *
 * Walkie-Talkie mode:
 *   - Press button -> audio transmitted immediately
 *   - Release -> stop transmitting, listen
 *   - Both pressed -> full duplex
 *   - Missed message -> LED blinks, playback button
 *
 * Two builds:
 *   USE_WIFI     - ESP32 DevKit + WiFi (home)
 *   USE_ETHERNET - WT32-ETH01 v1.4 + Ethernet (garage)
 *   USE_SD       - Enable SD card recording (WiFi board only)
 *
 * Mic: ICS-43434 (24-bit I2S MEMS)
 * Speaker: MAX98357A (I2S Class D)
 *
 * Author: ShtirlizVD
 * License: MIT
 */

#include <Arduino.h>
#include "pins.h"
#include "config.h"
#include "audio.h"
#include "recorder.h"
#include "intercom.h"
#include "webui.h"
#include <LittleFS.h>

#ifdef USE_ETHERNET
#include <ETH.h>
static bool ethConnected = false;
#else
#include <WiFi.h>
#endif

// ==================== PTT Button ====================
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
            // Clear missed recording when PTT pressed
            Recorder::clearRecording();
            Serial.println("[BTN] PTT pressed - transmitting");
            Intercom::pttPress();
        } else if (!isDown && buttonHeld) {
            buttonHeld = false;
            Serial.println("[BTN] PTT released");
            Intercom::pttRelease();
        }
    }
}

// ==================== Playback Button ====================
static bool playbackLastState = HIGH;
static uint32_t playbackDebounceTime = 0;

void handlePlaybackButton() {
    uint32_t now = millis();
    if (now - playbackDebounceTime < PLAYBACK_DEBOUNCE_MS) return;

    bool state = digitalRead(PLAYBACK_PIN);
    if (state == LOW && playbackLastState == HIGH) {
        playbackDebounceTime = now;
        if (Recorder::hasRecording()) {
            if (!Recorder::isPlaybackActive()) {
                Recorder::startPlayback();
                Serial.printf("[BTN] Playback (%u ms, %s)\n",
                              Recorder::getDuration(),
                              Recorder::getStorageName());
            }
        } else {
            Serial.println("[BTN] No recording to play");
        }
    }
    playbackLastState = state;
}

// ==================== Audio Send Task ====================
void audioSendTask(void* param) {
    Serial.println("[TASK] Audio send started (Core 1)");

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

// ==================== Audio Receive Task ====================
void audioReceiveTask(void* param) {
    Serial.println("[TASK] Audio receive started (Core 0)");

    int16_t spkBuffer[FRAME_SAMPLES + 64];
    bool wasPlaying = false;

    while (true) {
        // Priority: tones > recording playback > live audio stream
        if (Audio::isTonePlaying()) {
            int toneSamples = Audio::getToneFrame(spkBuffer, FRAME_SAMPLES);
            if (toneSamples > 0) {
                Audio::writeFrame(spkBuffer, toneSamples);
                wasPlaying = true;
            } else {
                wasPlaying = false;
                Audio::silenceSpeaker();
            }
        } else if (Recorder::isPlaybackActive()) {
            // Playing back recorded message
            int pbSamples = Recorder::getPlaybackFrame(spkBuffer, FRAME_SAMPLES);
            if (pbSamples > 0) {
                Audio::writeFrame(spkBuffer, pbSamples);
                wasPlaying = true;
            } else {
                // Playback finished - clear recording
                Recorder::clearRecording();
                Audio::silenceSpeaker();
                wasPlaying = false;
                Serial.println("[TASK] Playback finished");
            }
        } else if (Intercom::shouldPlay()) {
            int samplesRead = Intercom::receiveAudio(spkBuffer, FRAME_SAMPLES);
            if (samplesRead > 0) {
                // Record to buffer/SD in parallel
                if (Recorder::isRecording()) {
                    Recorder::recordSamples(spkBuffer, samplesRead);
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

// ==================== Network Task ====================
void networkTask(void* param) {
    Serial.println("[TASK] Network task started");

    while (true) {
        Intercom::update();
        WebUI::handleClient();
        handleButton();
        handlePlaybackButton();

        // Status every 30 sec
        static uint32_t lastStatus = 0;
        if (millis() - lastStatus > 30000) {
            lastStatus = millis();
#ifdef USE_ETHERNET
            Serial.printf("[STATUS] ETH: %s | Radio: %s | Peer: %s | Heap: %d KB | Rec: %s (%s)\n",
                          ethConnected ? ETH.localIP().toString().c_str() : "no",
                          Intercom::getStateName(),
                          Intercom::remoteActive() ? "ON" : "OFF",
                          ESP.getFreeHeap() / 1024,
                          Recorder::hasRecording() ? "YES" : "no",
                          Recorder::getStorageName());
#else
            Serial.printf("[STATUS] WiFi: %s | Radio: %s | Peer: %s | Heap: %d KB | Rec: %s (%s)\n",
                          WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "no",
                          Intercom::getStateName(),
                          Intercom::remoteActive() ? "ON" : "OFF",
                          ESP.getFreeHeap() / 1024,
                          Recorder::hasRecording() ? "YES" : "no",
                          Recorder::getStorageName());
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
            Serial.println("[ETH] Start");
            ETH.setHostname(Config::get().device_name);
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println("[ETH] Cable connected");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            Serial.printf("[ETH] Got IP: %s\n", ETH.localIP().toString().c_str());
            Serial.printf("[ETH] Mask: %s | GW: %s\n",
                          ETH.subnetMask().toString().c_str(),
                          ETH.gatewayIP().toString().c_str());
            ethConnected = true;
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            Serial.println("[ETH] Cable disconnected");
            ethConnected = false;
            break;
        case ARDUINO_EVENT_ETH_STOP:
            Serial.println("[ETH] Stopped");
            ethConnected = false;
            break;
    }
}

static bool initEthernet() {
    Serial.println("[SETUP] Starting Ethernet (WT32-ETH01 v1.4)...");
    WiFi.onEvent(onEthEvent);

    if (!ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_POWER,
                   ETH_MDC_PIN, ETH_MDIO_PIN, ETH_CLK_MODE)) {
        Serial.println("[ETH] ERROR init!");
        return false;
    }

    Serial.print("[ETH] Waiting for DHCP");
    int attempts = 0;
    while (!ethConnected && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (ethConnected) {
        Config::setDefaultName();
        Config::save();

        Serial.printf("[ETH] Connected! IP: %s\n", ETH.localIP().toString().c_str());
        WebUI::init();
        return true;
    }

    Serial.println("[ETH] Failed to get IP via DHCP");
    return false;
}

#endif // USE_ETHERNET

// ==================== Setup ====================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n========================================");
    Serial.println("   ESP32 Intercom v2.5");
    Serial.println("   Walkie-Talkie mode");
#ifdef USE_ETHERNET
    Serial.println("   WT32-ETH01 + Ethernet");
#else
    Serial.println("   ESP32 DevKit + WiFi");
#endif
    Serial.println("   Mic: ICS-43434 (24-bit)");
    Serial.println("   Speaker: MAX98357A");
    Serial.println("   Last message recording");
#ifdef USE_SD
    Serial.println("   SD card: enabled");
#endif
    Serial.println("========================================\n");

    // 1. Filesystem
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS error");
    } else {
        Serial.println("[FS] LittleFS OK");
        if (LittleFS.exists("/index.html")) {
            Serial.println("[FS] /index.html found");
        } else {
            Serial.println("[FS] /index.html not found (builtin)");
        }
    }

    // 2. Config
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

    // 4. Audio (I2S)
    if (!Audio::init()) {
        Serial.println("[AUDIO] I2S ERROR!");
    }
    Audio::setMicGain(cfg.mic_gain);
    Audio::setVolume(cfg.spk_volume);

    // 5. Recorder (SD or RAM)
    if (!Recorder::init(cfg.sample_rate)) {
        Serial.println("[REC] WARNING: Recording not available!");
    }

    // 6. Network: Ethernet or WiFi
#ifdef USE_ETHERNET
    initEthernet();
#else
    if (Config::hasWiFi()) {
        if (!WebUI::startSTA()) {
            Serial.println("[SETUP] AP mode");
        }
    } else {
        Serial.println("[SETUP] WiFi not configured -> AP mode");
        WebUI::startAP();
    }
#endif

    // 7. Intercom (UDP)
    if (!Intercom::init()) {
        Serial.println("[SETUP] UDP ERROR!");
    }

    // 8. FreeRTOS tasks
    xTaskCreatePinnedToCore(audioReceiveTask, "AudioRecv", AUDIO_TASK_STACK, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(audioSendTask,   "AudioSend",  AUDIO_TASK_STACK, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(networkTask,     "Network",    NET_TASK_STACK,   NULL, 1, NULL, 0);

    // 9. Ready
    Serial.println("\n[SETUP] Done!\n");
    Serial.printf("[SETUP] Name: %s\n", cfg.device_name);
    Serial.printf("[SETUP] Recorder: %s (max %u sec)\n",
                  Recorder::getStorageName(), Recorder::getMaxDuration());
    if (Config::hasRemote()) {
        Serial.printf("[SETUP] Remote: %s (ctrl:%u audio:%u)\n",
                      cfg.remote_ip, cfg.ctrl_port, cfg.audio_port);
    } else {
        Serial.println("[SETUP] Remote IP not set - configure via web!");
    }

    Serial.println("\n========================================");
#ifdef USE_ETHERNET
    if (ethConnected) {
        Serial.printf("  Ethernet: http://%s\n", ETH.localIP().toString().c_str());
    } else {
        Serial.println("  Ethernet: NOT CONNECTED");
    }
#else
    if (WebUI::isAPMode()) {
        Serial.printf("  Wi-Fi: %s_%s\n", AP_SSID_PREFIX, cfg.device_name);
        Serial.printf("  Password: %s\n", AP_PASSWORD);
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
