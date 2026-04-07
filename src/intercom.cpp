/*
 * intercom.cpp - Реализация модуля интеркома
 *
 * Управляет состоянием вызова, обработкой управляющих сообщений
 * и передачей/приёмом аудио по UDP.
 */

#include "intercom.h"
#include "config.h"
#include "audio.h"
#include "pins.h"
#include <Arduino.h>

// Статические члены
WiFiUDP Intercom::ctrlUdp;
WiFiUDP Intercom::audioUdp;
CallState Intercom::state = CallState::IDLE;
uint16_t Intercom::sendSeqNum = 0;
uint32_t Intercom::callStartTime = 0;
uint32_t Intercom::lastPacketTime = 0;
uint32_t Intercom::lastRingTime = 0;
bool Intercom::pttActive = false;
uint32_t Intercom::ringRepeatTimer = 0;
char Intercom::ctrlBuf[256];
char Intercom::lastCallerName[33] = {0};

bool Intercom::init() {
    DeviceConfig& cfg = Config::get();

    // Открываем UDP сокет для управляющих сообщений
    if (!ctrlUdp.begin(cfg.ctrl_port)) {
        Serial.printf("[INTERCOM] Ошибка открытия ctrl UDP порта %u\n", cfg.ctrl_port);
        return false;
    }

    // Открываем UDP сокет для аудио
    if (!audioUdp.begin(cfg.audio_port)) {
        Serial.printf("[INTERCOM] Ошибка открытия audio UDP порта %u\n", cfg.audio_port);
        ctrlUdp.stop();
        return false;
    }

    state = CallState::IDLE;
    sendSeqNum = 0;
    Serial.printf("[INTERCOM] UDP сокеты открыты (ctrl=%u, audio=%u)\n",
                  cfg.ctrl_port, cfg.audio_port);
    return true;
}

void Intercom::deinit() {
    endCall();
    ctrlUdp.stop();
    audioUdp.stop();
}

void Intercom::update() {
    processCtrlPacket();
    updateLED();

    uint32_t now = millis();

    switch (state) {
        case CallState::IDLE:
            // Ничего не делаем
            break;

        case CallState::RINGING_OUT:
            // Повторяем кольцо каждые 2 секунды
            if (now - ringRepeatTimer >= 2000) {
                ringRepeatTimer = now;
                sendCtrlMessage(MSG_RING);
                Serial.println("[INTERCOM] Отправка кольца...");
            }
            // Таймаут вызова
            if (now - callStartTime > RING_TIMEOUT_MS) {
                Serial.println("[INTERCOM] Таймаут вызова, возврат в IDLE");
                state = CallState::IDLE;
            }
            break;

        case CallState::RINGING_IN:
            // Таймаут входящего вызова
            if (now - lastRingTime > RING_TIMEOUT_MS) {
                Serial.println("[INTERCOM] Входящий вызов просрочен");
                state = CallState::IDLE;
                memset(lastCallerName, 0, sizeof(lastCallerName));
            }
            break;

        case CallState::IN_CALL:
            // Отправляем пинг каждые 3 секунды
            if (now - ringRepeatTimer >= 3000) {
                ringRepeatTimer = now;
                sendCtrlMessage(MSG_PING);
            }
            // Таймаут отсутствия пакетов
            if (now - lastPacketTime > CALL_TIMEOUT_MS) {
                Serial.println("[INTERCOM] Таймаут разговора (нет пакетов)");
                state = CallState::IDLE;
                Audio::silenceSpeaker();  // Очистить буфер динамика
            }
            break;
    }
}

void Intercom::startCall() {
    if (!Config::hasRemote()) {
        Serial.println("[INTERCOM] Не задан IP удалённого устройства!");
        return;
    }

    if (state == CallState::IN_CALL) {
        // Нажатие во время разговора = завершить
        endCall();
        return;
    }

    if (state != CallState::IDLE) {
        // Если уже звоним или входящий звонок — сбросить
        rejectCall();
        return;
    }

    state = CallState::RINGING_OUT;
    callStartTime = millis();
    lastPacketTime = millis();
    ringRepeatTimer = millis();
    sendCtrlMessage(MSG_RING);
    Serial.printf("[INTERCOM] Исходящий вызов → %s\n", Config::get().remote_ip);
}

void Intercom::answerCall() {
    if (state == CallState::RINGING_IN) {
        state = CallState::IN_CALL;
        callStartTime = millis();
        lastPacketTime = millis();
        ringRepeatTimer = millis();
        sendCtrlMessage(MSG_ANSWER);
        sendSeqNum = 0;
        Serial.printf("[INTERCOM] Вызов принят, разговор с %s\n", lastCallerName);
    }
}

void Intercom::endCall() {
    if (state != CallState::IDLE) {
        sendCtrlMessage(MSG_HANGUP);
        Serial.printf("[INTERCOM] Вызов завершён (длительность: %lu мс)\n",
                      getCallDuration());
    }
    state = CallState::IDLE;
    memset(lastCallerName, 0, sizeof(lastCallerName));
    pttActive = false;
}

void Intercom::rejectCall() {
    if (state == CallState::RINGING_IN) {
        sendCtrlMessage(MSG_BUSY);
        Serial.println("[INTERCOM] Вызов отклонён");
    }
    state = CallState::IDLE;
    memset(lastCallerName, 0, sizeof(lastCallerName));
}

CallState Intercom::getState() {
    return state;
}

bool Intercom::isCallActive() {
    return state == CallState::IN_CALL;
}

const char* Intercom::getStateName() {
    switch (state) {
        case CallState::IDLE:        return "Ожидание";
        case CallState::RINGING_OUT: return "Вызов...";
        case CallState::RINGING_IN:  return "Входящий вызов";
        case CallState::IN_CALL:     return "Разговор";
        default:                     return "Неизвестно";
    }
}

uint32_t Intercom::getCallDuration() {
    if (state == CallState::IN_CALL || state == CallState::RINGING_OUT) {
        return millis() - callStartTime;
    }
    return 0;
}

int Intercom::sendAudio(const int16_t* samples, int count) {
    if (!canTransmit() || count <= 0) return 0;

    DeviceConfig& cfg = Config::get();

    // Формируем пакет: заголовок + данные
    AudioPacketHeader hdr;
    hdr.seq = sendSeqNum++;
    hdr.timestamp = millis() - callStartTime;

    audioUdp.beginPacket(cfg.remote_ip, cfg.audio_port);
    audioUdp.write((uint8_t*)&hdr, sizeof(hdr));
    audioUdp.write((uint8_t*)samples, count * sizeof(int16_t));
    bool ok = audioUdp.endPacket();

    return ok ? count : 0;
}

int Intercom::receiveAudio(int16_t* buffer, int maxSamples) {
    if (state != CallState::IN_CALL) return 0;

    int avail = audioUdp.parsePacket();
    if (avail <= 0) return 0;

    // Читаем заголовок
    AudioPacketHeader hdr;
    if (audioUdp.available() < (int)sizeof(hdr)) {
        audioUdp.flush();
        return 0;
    }
    audioUdp.read((uint8_t*)&hdr, sizeof(hdr));

    // Обновляем время последнего полученного пакета
    lastPacketTime = millis();

    // Читаем аудио данные
    int bytesToRead = min(avail - sizeof(hdr), maxSamples * sizeof(int16_t));
    if (bytesToRead <= 0) return 0;

    int bytesRead = audioUdp.read((uint8_t*)buffer, bytesToRead);
    return bytesRead / sizeof(int16_t);
}

bool Intercom::isPTTActive() {
    return pttActive;
}

void Intercom::setPTTActive(bool active) {
    pttActive = active;
}

const char* Intercom::getRemoteIP() {
    return Config::get().remote_ip;
}

bool Intercom::canTransmit() {
    if (state != CallState::IN_CALL) return false;

    // В режиме PTT — передаём только при нажатой кнопке
    if (Config::get().button_mode == BTN_MODE_PTT) {
        return pttActive;
    }

    // В телефонном режиме — всегда передаём (полнодуплекс)
    return true;
}

// ==================== Приватные методы ====================

void Intercom::processCtrlPacket() {
    int packetSize = ctrlUdp.parsePacket();
    if (packetSize <= 0) return;

    // Ограничиваем размер буфера
    int len = min(packetSize, (int)sizeof(ctrlBuf) - 1);
    int readLen = ctrlUdp.read((uint8_t*)ctrlBuf, len);
    if (readLen <= 0) return;
    ctrlBuf[readLen] = '\0';

    // Сохраняем IP отправителя для возможного ответа
    // String senderIP = ctrlUdp.remoteIP().toString();

    // Парсим простое JSON-подобное сообщение
    // Формат: {"t":"<type>","f":"<from_name>"}
    char type[16] = {0};
    char from[33] = {0};

    // Простой парсер (без ArduinoJson для управляющих сообщений — быстрее)
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

    if (strlen(type) == 0) return;  // Неизвестный формат

    // Обработка сообщений
    if (strcmp(type, "ring") == 0) {
        // Входящий вызов
        if (state == CallState::IDLE) {
            state = CallState::RINGING_IN;
            lastRingTime = millis();
            strncpy(lastCallerName, from, sizeof(lastCallerName) - 1);
            Serial.printf("[INTERCOM] Входящий вызов от: %s\n",
                          strlen(from) > 0 ? from : "Unknown");
        } else if (state == CallState::IN_CALL) {
            // Уже разговариваем — отправляем "занято"
            sendCtrlMessage(MSG_BUSY);
        }
        // Если уже исходящий звонок — игнорируем
    }
    else if (strcmp(type, "answer") == 0) {
        // Ответ на наш исходящий вызов
        if (state == CallState::RINGING_OUT) {
            state = CallState::IN_CALL;
            callStartTime = millis();
            lastPacketTime = millis();
            ringRepeatTimer = millis();
            sendSeqNum = 0;
            Serial.printf("[INTERCOM] Вызов принят! Разговор с %s\n",
                          strlen(from) > 0 ? from : "Unknown");
        }
    }
    else if (strcmp(type, "hangup") == 0) {
        if (state != CallState::IDLE) {
            Serial.printf("[INTERCOM] Собеседник завершил вызов (%s)\n",
                          strlen(from) > 0 ? from : "Unknown");
            state = CallState::IDLE;
            memset(lastCallerName, 0, sizeof(lastCallerName));
            pttActive = false;
        }
    }
    else if (strcmp(type, "ping") == 0) {
        // Отвечаем на пинг
        if (state == CallState::IN_CALL) {
            sendCtrlMessage(MSG_PONG);
            lastPacketTime = millis();
        }
    }
    else if (strcmp(type, "pong") == 0) {
        // Обновляем время последнего пакета
        if (state == CallState::IN_CALL) {
            lastPacketTime = millis();
        }
    }
    else if (strcmp(type, "busy") == 0) {
        if (state == CallState::RINGING_OUT) {
            Serial.printf("[INTERCOM] Абонент занят\n");
            state = CallState::IDLE;
        }
    }
}

void Intercom::sendCtrlMessage(CtrlMsgType type) {
    if (!Config::hasRemote()) return;

    const char* typeStr;
    switch (type) {
        case MSG_RING:   typeStr = "ring";   break;
        case MSG_ANSWER: typeStr = "answer"; break;
        case MSG_HANGUP: typeStr = "hangup"; break;
        case MSG_PING:   typeStr = "ping";   break;
        case MSG_PONG:   typeStr = "pong";   break;
        case MSG_BUSY:   typeStr = "busy";   break;
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

void Intercom::updateLED() {
    static uint32_t lastBlink = 0;
    static bool ledState = false;
    uint32_t now = millis();

    switch (state) {
        case CallState::IDLE:
            // LED выключен
            digitalWrite(LED_PIN, LED_OFF);
            break;

        case CallState::RINGING_OUT:
            // Быстрое мигание (вызов)
            if (now - lastBlink > 300) {
                lastBlink = now;
                ledState = !ledState;
                digitalWrite(LED_PIN, ledState ? LED_ON : LED_OFF);
            }
            break;

        case CallState::RINGING_IN:
            // Медленное мигание (входящий вызов)
            if (now - lastBlink > 500) {
                lastBlink = now;
                ledState = !ledState;
                digitalWrite(LED_PIN, ledState ? LED_ON : LED_OFF);
            }
            break;

        case CallState::IN_CALL:
            // LED горит постоянно
            digitalWrite(LED_PIN, LED_ON);
            break;
    }
}


