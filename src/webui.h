#pragma once
/*
 * webui.h - Веб-интерфейс настройки устройства
 *
 * Предоставляет HTTP API и веб-страницу для конфигурации:
 * - Подключение к WiFi
 * - Настройка параметров устройства
 * - Настройка интеркома
 * - Настройка аудио
 * - Состояние устройства
 *
 * В режиме AP (точки доступа) работает как captive portal.
 */

#include <WebServer.h>
#include <DNSServer.h>

class WebUI {
public:
    // Инициализация веб-сервера
    static void init();

    // Обработка запросов (вызывать в loop)
    static void handleClient();

    // Запуск в режиме AP (точки доступа для настройки)
    static void startAP();

    // Запуск в режиме STA (подключение к WiFi)
    static bool startSTA();

    // Проверка: работает ли AP
    static bool isAPMode();

private:
    // Роуты API
    static void handleRoot();
    static void handleIndexHTML();
    static void handleAPIStatus();
    static void handleAPIGetConfig();
    static void handleAPISetConfig();
    static void handleAPIWiFiScan();
    static void handleAPIWiFiConnect();
    static void handleAPIReboot();
    static void handleAPIFactoryReset();
    static void handleNotFound();

    // Вспомогательные
    static void sendJSON(int code, const char* json);
    static void sendCaptivePortal();

    static WebServer* server;
    static DNSServer* dnsServer;
    static bool apMode;
    static uint32_t apStartTime;
};
