/*
 * intercom.cpp - Реализация модуля интеркома (логика рации)
 *
 * Принцип:
 *   - Кнопка нажата → сразу передаём аудио по UDP
 *   - Кнопка отпущена → перестаём передавать
 *   - Входящие аудио-пакеты ВСЕГДА воспроизводятся
 *   - Если оба нажали — работают одновременно (дуплекс)
 *
 * Управляющие сообщения tx_on/tx_off служат для:
 *   1. LED индикации на удалённом устройстве
 *   2. Таймаутов и проверки связи
 *   3. Мьютинга динамика когда никто не говорит
 */

#include "intercom.h"
#include "config.h"
#include "audio.h"
#include "pins.h"
#include <Arduino.h>

// ==================== Статические члены ====================
WiFiUDP Intercom::ctrlUdp;
WiFiUDP Intercom::audioUdp;

volatile bool Intercom::pttDown = false;
bool Intercom::remoteTx = false;
uint32_t Intercom::remoteTxTimeout = 0;
uint32_t Intercom::remoteTxLastSeen = 0;
uint32_t Intercom::txStartTime = 0;
uint16_t Intercom::sendSeqNum = 0;

uint32_t Intercom::lastPingSent = 0;
uint32_t Intercom::lastPongReceived = 0;
bool Intercom::peerOnline = false;

ComState Intercom::state = ComState::IDLE;
char Intercom::ctrlBuf[256];

// Таймауты
#define REMOTE_TX_TIMEOUT_MS     500    // Если нет аудио/tx_off от собеседника 500 мс — считаем что он перестал
#define PING_INTERVAL_MS         5000   // Пинг каждые 5 сек
#define PEER_ONLINE_TIMEOUT_MS   15000  // Если нет ответа 15 сек — собеседник оффлайн


// ==================== Инициализация ====================

bool Intercom::init() {
    DeviceConfig& cfg = Config::get();

    if (!ctrlUdp.begin(cfg.ctrl_port)) {
        Serial.printf("[INTERCOM] Ошибка открытия ctrl UDP порта %u\n", cfg.ctrl_port);
        return false;
    }
    if (!audioUdp.begin(cfg.audio_port)) {
        Serial.printf("[INTERCOM] Ошибка открытия audio UDP порта %u\n", cfg.audio_port);
        ctrlUdp.stop();
        return false;
    }

    state = ComState::IDLE;
    pttDown = false;
    remoteTx = false;
    peerOnline = false;
    sendSeqNum = 0;

    Serial.printf("[INTERCOM] UDP готов (ctrl=%u, audio=%u) — режим рации\n",
                  cfg.ctrl_port, cfg.audio_port);
    return true;
}

void Intercom::deinit() {
    if (pttDown) pttRelease();
    ctrlUdp.stop();
    audioUdp.stop();
}


// ==================== Кнопка PTT ====================

void Intercom::pttPress() {
    if (!Config::hasRemote()) {
        Serial.println("[INTERCOM] Удалённый IP не задан!");
        return;
    }

    if (pttDown) return;  // Уже нажата
    pttDown = true;
    txStartTime = millis();
    sendSeqNum = 0;

    sendCtrlMessage(MSG_TX_ON);
    updateState();
    Serial.println("[INTERCOM] 🎤 PTT ON — начинаю передачу");
}

void Intercom::pttRelease() {
    if (!pttDown) return;  // Уже отпущена
    pttDown = false;

    sendCtrlMessage(MSG_TX_OFF);
    updateState();
    Serial.println("[INTERCOM] 🔇 PTT OFF — передача прекращена");
}


// ==================== Главный цикл ====================

void Intercom::update() {
    processCtrlPacket();
    updateState();
    updateLED();

    uint32_t now = millis();

    // Таймаут: если собеседник давно не присылал данные — считаем что перестал
    if (remoteTx && (now - remoteTxLastSeen > REMOTE_TX_TIMEOUT_MS)) {
        remoteTx = false;
        updateState();
    }

    // Периодический пинг для проверки связи
    if (now - lastPingSent > PING_INTERVAL_MS && Config::hasRemote()) {
        lastPingSent = now;
        sendCtrlMessage(MSG_PING);
    }

    // Таймаут проверки he's online
    if (peerOnline && (now - lastPongReceived > PEER_ONLINE_TIMEOUT_MS)) {
        peerOnline = false;
    }
}


// ==================== Состояние ====================

void Intercom::updateState() {
    if (pttDown && remoteTx) {
        state = ComState::DUPLEX;
    } else if (pttDown) {
        state = ComState::TRANSMITTING;
    } else if (remoteTx) {
        state = ComState::RECEIVING;
    } else {
        state = ComState::IDLE;
    }
}

ComState Intercom::getState() {
    return state;
}

const char* Intercom::getStateName() {
    switch (state) {
        case ComState::IDLE:         return "Ожидание";
        case ComState::TRANSMITTING: return "Передача (PTT)";
        case ComState::RECEIVING:    return "Приём";
        case ComState::DUPLEX:       return "Дуплекс";
        default:                     return "?";
    }
}

bool Intercom::amTransmitting() {
    return pttDown;
}

bool Intercom::remoteActive() {
    return remoteTx;
}

bool Intercom::isDuplex() {
    return pttDown && remoteTx;
}

uint32_t Intercom::getTxDuration() {
    if (pttDown && txStartTime > 0) {
        return millis() - txStartTime;
    }
    return 0;
}

uint32_t Intercom::getSessionUptime() {
    if (peerOnline) {
        return (millis() - lastPongReceived) / 1000;
    }
    return 0;
}


// ==================== Аудио ====================

bool Intercom::canTransmit() {
    return pttDown && Config::hasRemote();
}

bool Intercom::shouldPlay() {
    // Воспроизводить если собеседник активен
    // В режиме передачи (PTT только мой) — можно мьютить динамик,
    // чтобы не было эха от собственного голоса через собеседника
    if (remoteTx) {
        if (pttDown) {
            // Дуплекс: оба нажали — воспроизводим (эхо возможен, но это ожидаемо)
            return true;
        }
        // Только собеседник передаёт — воспроизводим
        return true;
    }
    return false;
}

int Intercom::sendAudio(const int16_t* samples, int count) {
    if (!canTransmit() || count <= 0) return 0;

    DeviceConfig& cfg = Config::get();

    AudioPacketHeader hdr;
    hdr.seq = sendSeqNum++;
    hdr.timestamp = millis() - txStartTime;

    audioUdp.beginPacket(cfg.remote_ip, cfg.audio_port);
    audioUdp.write((uint8_t*)&hdr, sizeof(hdr));
    audioUdp.write((uint8_t*)samples, count * sizeof(int16_t));
    bool ok = audioUdp.endPacket();

    return ok ? count : 0;
}

int Intercom::receiveAudio(int16_t* buffer, int maxSamples) {
    if (!Config::hasRemote()) return 0;

    int avail = audioUdp.parsePacket();
    if (avail <= 0) return 0;

    // Читаем заголовок
    AudioPacketHeader hdr;
    if (audioUdp.available() < (int)sizeof(hdr)) {
        audioUdp.flush();
        return 0;
    }
    audioUdp.read((uint8_t*)&hdr, sizeof(hdr));

    // Обновляем время последнего полученного аудио
    remoteTxLastSeen = millis();
    if (!remoteTx) {
        remoteTx = true;
        updateState();
    }

    // Читаем аудио данные
    int bytesToRead = min(avail - (int)sizeof(hdr), maxSamples * (int)sizeof(int16_t));
    if (bytesToRead <= 0) return 0;

    int bytesRead = audioUdp.read((uint8_t*)buffer, bytesToRead);
    return bytesRead / sizeof(int16_t);
}

const char* Intercom::getRemoteIP() {
    return Config::get().remote_ip;
}


// ==================== Управляющие сообщения ====================

void Intercom::processCtrlPacket() {
    int packetSize = ctrlUdp.parsePacket();
    if (packetSize <= 0) return;

    int len = min(packetSize, (int)sizeof(ctrlBuf) - 1);
    int readLen = ctrlUdp.read((uint8_t*)ctrlBuf, len);
    if (readLen <= 0) return;
    ctrlBuf[readLen] = '\0';

    // Простой парсер JSON: {"t":"<type>","f":"<name>"}
    char type[16] = {0};
    char from[33] = {0};

    char* p;
    p = strstr(ctrlBuf, "\"t\"");
    if (p) {
        p = strchr(p + 3, '"'); if (p) p++;
        if (p) { int i = 0; while (*p && *p != '"' && i < 15) type[i++] = *p++; }
    }
    p = strstr(ctrlBuf, "\"f\"");
    if (p) {
        p = strchr(p + 3, '"'); if (p) p++;
        if (p) { int i = 0; while (*p && *p != '"' && i < 32) from[i++] = *p++; }
    }

    if (strlen(type) == 0) return;

    if (strcmp(type, "tx_on") == 0) {
        // Собеседник начал передачу
        remoteTx = true;
        remoteTxLastSeen = millis();
        peerOnline = true;
        lastPongReceived = millis();
        updateState();
        Serial.printf("[INTERCOM] 📻 Собеседник передаёт (%s)\n",
                      strlen(from) > 0 ? from : "?");
    }
    else if (strcmp(type, "tx_off") == 0) {
        // Собеседник прекратил передачу
        if (remoteTx) {
            remoteTx = false;
            updateState();
            Serial.printf("[INTERCOM] 🔇 Собеседник прекратил (%s)\n",
                          strlen(from) > 0 ? from : "?");
        }
    }
    else if (strcmp(type, "ping") == 0) {
        // Собеседник проверяет связь — отвечаем
        peerOnline = true;
        lastPongReceived = millis();
        sendCtrlMessage(MSG_PONG);
    }
    else if (strcmp(type, "pong") == 0) {
        peerOnline = true;
        lastPongReceived = millis();
    }
}

void Intercom::sendCtrlMessage(CtrlMsgType type) {
    if (!Config::hasRemote()) return;

    const char* typeStr;
    switch (type) {
        case MSG_TX_ON:  typeStr = "tx_on";  break;
        case MSG_TX_OFF: typeStr = "tx_off"; break;
        case MSG_PING:   typeStr = "ping";   break;
        case MSG_PONG:   typeStr = "pong";   break;
        default:         typeStr = "unknown"; break;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "{\"t\":\"%s\",\"f\":\"%s\"}",
             typeStr, Config::get().device_name);

    DeviceConfig& cfg = Config::get();
    ctrlUdp.beginPacket(cfg.remote_ip, cfg.ctrl_port);
    ctrlUdp.write((uint8_t*)msg, strlen(msg));
    ctrlUdp.endPacket();
}


// ==================== LED ====================

void Intercom::updateLED() {
    static uint32_t lastBlink = 0;
    static bool ledState = false;
    uint32_t now = millis();

    switch (state) {
        case ComState::IDLE:
            // Свободен: короткая вспышка каждые 3 сек если собеседник онлайн,
            // иначе выключен
            if (peerOnline) {
                if (now - lastBlink > 3000 && !ledState) {
                    lastBlink = now;
                    ledState = true;
                    digitalWrite(LED_PIN, LED_ON);
                } else if (now - lastBlink > 200 && ledState) {
                    ledState = false;
                    digitalWrite(LED_PIN, LED_OFF);
                }
            } else {
                digitalWrite(LED_PIN, LED_OFF);
                ledState = false;
            }
            break;

        case ComState::TRANSMITTING:
            // Передаю: быстрое мигание
            if (now - lastBlink > 150) {
                lastBlink = now;
                ledState = !ledState;
                digitalWrite(LED_PIN, ledState ? LED_ON : LED_OFF);
            }
            break;

        case ComState::RECEIVING:
            // Принимаю: плавное мигание (пульсация)
            if (now - lastBlink > 400) {
                lastBlink = now;
                ledState = !ledState;
                digitalWrite(LED_PIN, ledState ? LED_ON : LED_OFF);
            }
            break;

        case ComState::DUPLEX:
            // Дуплекс: горит постоянно
            digitalWrite(LED_PIN, LED_ON);
            ledState = true;
            break;
    }
}
