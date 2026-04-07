#pragma once
/*
 * intercom.h - Модуль интеркома (логика рации)
 *
 * Логика работы:
 *   1. Нажал кнопку → речь СРАЗУ передаётся на другое устройство
 *   2. Отпустил кнопку → передача прекращается
 *   3. Если на обоих устройствах нажаты кнопки → полнодуплекс (как по телефону)
 *
 * Устройство ВСЕГДА слушает входящие аудио-пакеты и воспроизводит их.
 * Кнопка управляет только отправкой (PTT — Push To Talk).
 *
 * Протокол:
 *   - Управляющие сообщения: JSON по UDP (порт ctrl_port)
 *   - Аудио поток: PCM по UDP (порт audio_port)
 *
 * Формат аудио-пакета:
 *   [seq: uint16][timestamp: uint32][pcm_data: N байт]
 *
 * Управляющие сообщения (JSON):
 *   {"t":"tx_on","f":"<name>"}    - Я начал передавать
 *   {"t":"tx_off","f":"<name>"}   - Я прекратил передавать
 *   {"t":"ping","f":"<name>"}     - Проверка связи
 *   {"t":"pong","f":"<name>"}     - Ответ на пинг
 */

#include <cstdint>
#include <cstring>
#include <WiFi.h>
#include <WiFiUdp.h>

// Состояние интеркома
enum class ComState : uint8_t {
    IDLE = 0,        // Ожидание (кнопка не нажата, никто не говорит)
    TRANSMITTING,    // Я передаю (моя кнопка нажата)
    RECEIVING,       // Собеседник передаёт (принимаю аудио)
    DUPLEX           // Оба передают одновременно (полнодуплекс)
};

// Заголовок аудио-пакета
struct AudioPacketHeader {
    uint16_t seq;         // Порядковый номер
    uint32_t timestamp;   // Временная метка (мс от начала передачи)
} __attribute__((packed));

// Типы управляющих сообщений
enum CtrlMsgType : uint8_t {
    MSG_TX_ON = 0,
    MSG_TX_OFF,
    MSG_PING,
    MSG_PONG
};

class Intercom {
public:
    // Инициализация UDP сокетов
    static bool init();

    // Деинициализация
    static void deinit();

    // Главный цикл обработки (вызывать в loop)
    static void update();

    // ===== Управление передачей (вызывается по кнопке) =====

    // Нажали кнопку — начать передачу
    static void pttPress();

    // Отпустили кнопку — прекратить передачу
    static void pttRelease();

    // ===== Состояние =====

    static ComState getState();
    static const char* getStateName();
    static bool amTransmitting();    // Моя кнопка нажата?
    static bool remoteActive();      // Собеседник передаёт?
    static bool isDuplex();          // Оба передают?
    static uint32_t getTxDuration(); // Длительность текущей передачи (мс)
    static uint32_t getSessionUptime(); // Время с последнего tx_on/tx_off от собеседника

    // ===== Аудио =====

    // Передать кадр аудио (вызывается из задачи отправки)
    static int sendAudio(const int16_t* samples, int count);

    // Принять кадр аудио (вызывается из задачи приёма)
    static int receiveAudio(int16_t* buffer, int maxSamples);

    // Нужно ли сейчас передавать?
    static bool canTransmit();

    // Нужно ли сейчас воспроизводить входящий звук?
    static bool shouldPlay();

    // Получить IP удалённого устройства
    static const char* getRemoteIP();

private:
    // Обработка входящих управляющих пакетов
    static void processCtrlPacket();

    // Отправка управляющего сообщения
    static void sendCtrlMessage(CtrlMsgType type);

    // Обновление LED индикатора
    static void updateLED();

    // Вычисление текущего состояния
    static void updateState();

    // Член-данные
    static WiFiUDP ctrlUdp;
    static WiFiUDP audioUdp;

    // Флаги
    static volatile bool pttDown;         // Моя кнопка нажата
    static bool remoteTx;                 // Собеседник передаёт
    static uint32_t remoteTxTimeout;      // Таймаут активности собеседника
    static uint32_t remoteTxLastSeen;     // Последний признак жизни собеседника
    static uint32_t txStartTime;          // Когда я начал передавать
    static uint16_t sendSeqNum;           // Счётчик пакетов

    // Пинг/связь
    static uint32_t lastPingSent;
    static uint32_t lastPongReceived;
    static bool peerOnline;

    // Вычисленное состояние
    static ComState state;

    // Буфер
    static char ctrlBuf[256];
};
