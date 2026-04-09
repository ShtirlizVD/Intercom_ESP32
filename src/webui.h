#pragma once
/*
 * webui.h - Веб-интерфейс настройки устройства
 *
 * Поддерживает два режима подключения:
 * - USE_WIFI:     WiFi (STA + AP с captive portal)
 * - USE_ETHERNET: Ethernet (DHCP, WT32-ETH01)
 */

#include <WebServer.h>
#include <DNSServer.h>

#ifdef USE_ETHERNET
#include <ETH.h>
#endif

class WebUI {
public:
    static void init();
    static void handleClient();

#ifdef USE_WIFI
    static void startAP();
    static bool startSTA();
    static bool isAPMode();
#endif

private:
    static void handleRoot();
    static void handleIndexHTML();
    static void handleAPIStatus();
    static void handleAPIGetConfig();
    static void handleAPISetConfig();
    static void handleAPIReboot();
    static void handleAPIFactoryReset();
    static void handleNotFound();
    static void sendJSON(int code, const char* json);

#ifdef USE_WIFI
    static void handleAPIWiFiScan();
    static void handleAPIWiFiConnect();
    static void handleAPISetWiFi();
#endif

    static WebServer* server;
#ifdef USE_WIFI
    static DNSServer* dnsServer;
    static bool apMode;
    static uint32_t apStartTime;
#endif
};
