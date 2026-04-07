#pragma once
/*
 * intercom.h - Модуль интеркома
 *
 * Управление состоянием вызова и UDP передачей аудио.
 * Поддерживаются режимы:
 *   - Phone (телефонный): полнодуплекс, как обычный телефон
 *   - PTT (push-to-talk): полудуплекс с удержанием кнопки
 *
 * Протокол:
 *   - Управляющие сообщения: JSON по UDP (порт ctrl_port)
 *   - Аудио поток: PCM по UDP (порт audio_port)
 *
 * Формат аудио-пакета:
 *   [seq: uint16][timestamp: uint32][pcm_data: N байт]
 *
 * Управляющие сообщения (JSON):
 *   {"t":"ring","f":"<name>"}      - Вызов
 *   {"t":"answer","f":"<name>"}    - Ответ на вызов
 *   {"t":"hangup","f":"<name>"}    - Завершение вызова
 *   {"t":"ping","f":"<name>"}      - Проверка связи
 *   {"t":"pong","f":"<name>"}      - Ответ на пинг
 *   {"t":"busy","f":"<name>"}      - Занято
 */

#include <cstdint>
#include <cstring>
#include <WiFi.h>
#include <WiFiUdp.h>

// Состояния вызова
enum class CallState : uint8_t {
    IDLE = 0,        // Ожидание
    RINGING_OUT,     // Исходящий вызов (жду ответ)
    RINGING_IN,      // Входящий вызов (звонок)
    IN_CALL          // Разговор
};

// Заголовок аудио-пакета
struct AudioPacketHeader {
    uint16_t seq;         // Порядковый номер
    uint32_t timestamp;   // Временная метка (мс от начала вызова)
} __attribute__((packed));

// Типы управляющих сообщений
enum CtrlMsgType : uint8_t {
    MSG_RING = 0,
    MSG_ANSWER,
    MSG_HANGUP,
    MSG_PING,
    MSG_PONG,
    MSG_BUSY
};

class Intercom {
public:
    // Инициализация UDP сокетов
    static bool init();

    // Деинициализация
    static void deinit();

    // Главный цикл обработки (вызывать в loop)
    static void update();

    // Управление вызовами
    static void startCall();      // Начать исходящий вызов
    static void answerCall();     // Ответить на входящий вызов
    static void endCall();        // Завершить текущий вызов
    static void rejectCall();     // Отклонить входящий вызов

    // Состояние
    static CallState getState();
    static bool isCallActive();
    static const char* getStateName();
    static uint32_t getCallDuration();

    // Передача аудио (вызывается из задачи)
    // Возвращает количество отправленных байт
    static int sendAudio(const int16_t* samples, int count);

    // Приём аудио (вызывается из задачи)
    // Возвращает количество полученных сэмплов
    static int receiveAudio(int16_t* buffer, int maxSamples);

    // Получить/установить флаг PTT (для push-to-talk режима)
    static bool isPTTActive();
    static void setPTTActive(bool active);

    // Получить IP адрес удалённого устройства
    static const char* getRemoteIP();

    // Проверить, можно ли передавать аудио
    static bool canTransmit();

private:
    // Обработка входящих управляющих пакетов
    static void processCtrlPacket();

    // Отправка управляющего сообщения
    static void sendCtrlMessage(CtrlMsgType type);

    // Обновление LED индикатора
    static void updateLED();

    // Член-данные
    static WiFiUDP ctrlUdp;
    static WiFiUDP audioUdp;
    static CallState state;
    static uint16_t sendSeqNum;
    static uint32_t callStartTime;
    static uint32_t lastPacketTime;
    static uint32_t lastRingTime;
    static bool pttActive;
    static uint32_t ringRepeatTimer;

    // Буферы
    static char ctrlBuf[256];
    static char lastCallerName[33];
};
