/*
 * config.cpp - Реализация модуля конфигурации
 * Хранение и загрузка параметров через NVS (Preferences)
 */

#include "config.h"
#include "pins.h"
#include <Preferences.h>
#include <WiFi.h>

// Глобальный экземпляр Preferences
static Preferences preferences;

// Экземпляр конфигурации
DeviceConfig Config::cfg;

void Config::init() {
    // Установить значения по умолчанию
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.device_name, "ESP32-Intercom", MAX_NAME_LEN + 1);
    cfg.button_mode = BTN_MODE_PHONE;
    cfg.ctrl_port = DEFAULT_CTRL_PORT;
    cfg.audio_port = DEFAULT_AUDIO_PORT;
    cfg.sample_rate = SAMPLE_RATE;
    cfg.mic_gain = 5;
    cfg.spk_volume = 7;
    cfg.dhcp_enabled = true;
    cfg.wifi_configured = false;
    cfg.remote_configured = false;

    // Попробовать загрузить из NVS
    load();
}

void Config::load() {
    if (!preferences.begin(CONFIG_NAMESPACE, true)) {
        Serial.println("[CONFIG] NVS не инициализирован, используем настройки по умолчанию");
        return;
    }

    // WiFi
    if (preferences.isKey("ssid")) {
        String ssid = preferences.getString("ssid", "");
        strncpy(cfg.wifi_ssid, ssid.c_str(), MAX_SSID_LEN);
        cfg.wifi_configured = true;
    }
    if (preferences.isKey("pass")) {
        String pass = preferences.getString("pass", "");
        strncpy(cfg.wifi_password, pass.c_str(), MAX_PASS_LEN);
    }
    cfg.dhcp_enabled = preferences.getBool("dhcp", true);
    if (preferences.isKey("ip")) {
        String ip = preferences.getString("ip", "");
        strncpy(cfg.static_ip, ip.c_str(), MAX_IP_LEN);
    }
    if (preferences.isKey("gw")) {
        String gw = preferences.getString("gw", "");
        strncpy(cfg.static_gw, gw.c_str(), MAX_IP_LEN);
    }
    if (preferences.isKey("mask")) {
        String mask = preferences.getString("mask", "");
        strncpy(cfg.static_netmask, mask.c_str(), MAX_IP_LEN);
    }

    // Устройство
    if (preferences.isKey("name")) {
        String name = preferences.getString("name", "");
        strncpy(cfg.device_name, name.c_str(), MAX_NAME_LEN);
    } else {
        setDefaultName();
    }
    cfg.button_mode = (ButtonMode)preferences.getInt("btnmode", BTN_MODE_PHONE);

    // Интерком
    if (preferences.isKey("rip")) {
        String rip = preferences.getString("rip", "");
        strncpy(cfg.remote_ip, rip.c_str(), MAX_IP_LEN);
        cfg.remote_configured = (strlen(cfg.remote_ip) > 0);
    }
    cfg.ctrl_port = preferences.getUInt("cport", DEFAULT_CTRL_PORT);
    cfg.audio_port = preferences.getUInt("aport", DEFAULT_AUDIO_PORT);

    // Аудио
    cfg.sample_rate = preferences.getUInt("srate", SAMPLE_RATE);
    cfg.mic_gain = preferences.getUInt("mgain", 5);
    cfg.spk_volume = preferences.getUInt("svol", 7);

    preferences.end();

    Serial.printf("[CONFIG] Загружены настройки: %s (IP удалённого: %s)\n",
                  cfg.device_name, cfg.remote_ip);
}

void Config::save() {
    if (!preferences.begin(CONFIG_NAMESPACE, false)) {
        Serial.println("[CONFIG] Ошибка записи в NVS!");
        return;
    }

    // WiFi
    preferences.putString("ssid", cfg.wifi_ssid);
    preferences.putString("pass", cfg.wifi_password);
    preferences.putBool("dhcp", cfg.dhcp_enabled);
    preferences.putString("ip", cfg.static_ip);
    preferences.putString("gw", cfg.static_gw);
    preferences.putString("mask", cfg.static_netmask);

    // Устройство
    preferences.putString("name", cfg.device_name);
    preferences.putInt("btnmode", (int)cfg.button_mode);

    // Интерком
    preferences.putString("rip", cfg.remote_ip);
    preferences.putUInt("cport", cfg.ctrl_port);
    preferences.putUInt("aport", cfg.audio_port);

    // Аудио
    preferences.putUInt("srate", cfg.sample_rate);
    preferences.putUInt("mgain", cfg.mic_gain);
    preferences.putUInt("svol", cfg.spk_volume);

    preferences.end();
    Serial.println("[CONFIG] Настройки сохранены в NVS");
}

void Config::reset() {
    preferences.begin(CONFIG_NAMESPACE, false);
    preferences.clear();
    preferences.end();
    Serial.println("[CONFIG] Все настройки сброшены");
    init();  // Перезагрузить значения по умолчанию
}

DeviceConfig& Config::get() {
    return cfg;
}

bool Config::hasWiFi() {
    return cfg.wifi_configured && strlen(cfg.wifi_ssid) > 0;
}

bool Config::hasRemote() {
    return cfg.remote_configured && strlen(cfg.remote_ip) > 0;
}

void Config::setWiFi(const char* ssid, const char* password) {
    strncpy(cfg.wifi_ssid, ssid, MAX_SSID_LEN);
    cfg.wifi_ssid[MAX_SSID_LEN] = '\0';
    strncpy(cfg.wifi_password, password, MAX_PASS_LEN);
    cfg.wifi_password[MAX_PASS_LEN] = '\0';
    cfg.wifi_configured = true;
    save();
}

void Config::setStaticIP(const char* ip, const char* gw, const char* mask) {
    strncpy(cfg.static_ip, ip, MAX_IP_LEN);
    strncpy(cfg.static_gw, gw, MAX_IP_LEN);
    strncpy(cfg.static_netmask, mask, MAX_IP_LEN);
    save();
}

void Config::setIntercom(const char* remoteIp, uint16_t ctrlPort, uint16_t audioPort) {
    strncpy(cfg.remote_ip, remoteIp, MAX_IP_LEN);
    cfg.ctrl_port = ctrlPort;
    cfg.audio_port = audioPort;
    cfg.remote_configured = (strlen(cfg.remote_ip) > 0);
    save();
}

void Config::setDefaultName() {
    // Генерация имени на основе MAC-адреса
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(cfg.device_name, MAX_NAME_LEN + 1, "Intercom-%02X%02X",
             mac[4], mac[5]);
}
