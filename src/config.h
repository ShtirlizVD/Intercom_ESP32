#pragma once
/*
 * config.h - Модуль конфигурации устройства
 * Хранение параметров в NVS (Non-Volatile Storage) через Preferences
 */

#include <cstdint>
#include <cstring>

// Пространство имён NVS
#define CONFIG_NAMESPACE "intercom"

// Максимальная длина строк
#define MAX_SSID_LEN     32
#define MAX_PASS_LEN     64
#define MAX_NAME_LEN     32
#define MAX_IP_LEN       15
#define MAX_MODE_LEN     8

// Режим работы кнопки
enum ButtonMode {
    BTN_MODE_PHONE = 0,  // Телефонный режим (полнодуплекс)
    BTN_MODE_PTT   = 1   // Push-to-Talk (полудуплекс с кнопкой)
};

// Структура конфигурации
struct DeviceConfig {
    // WiFi параметры
    char wifi_ssid[MAX_SSID_LEN + 1];
    char wifi_password[MAX_PASS_LEN + 1];
    bool dhcp_enabled;
    char static_ip[MAX_IP_LEN + 1];
    char static_gw[MAX_IP_LEN + 1];
    char static_netmask[MAX_IP_LEN + 1];

    // Параметры устройства
    char device_name[MAX_NAME_LEN + 1];
    ButtonMode button_mode;

    // Параметры интеркома
    char remote_ip[MAX_IP_LEN + 1];
    uint16_t ctrl_port;
    uint16_t audio_port;

    // Аудио параметры
    uint32_t sample_rate;
    uint8_t mic_gain;      // 0-10
    uint8_t spk_volume;    // 0-10

    // Заданы ли минимальные настройки
    bool wifi_configured;
    bool remote_configured;
};

class Config {
public:
    // Инициализация (загрузка из NVS)
    static void init();

    // Загрузить конфигурацию из NVS
    static void load();

    // Сохранить конфигурацию в NVS
    static void save();

    // Сбросить к заводским настройкам
    static void reset();

    // Получить текущую конфигурацию
    static DeviceConfig& get();

    // Проверки
    static bool hasWiFi();
    static bool hasRemote();

    // Установить WiFi параметры
    static void setWiFi(const char* ssid, const char* password);

    // Установить статический IP
    static void setStaticIP(const char* ip, const char* gw, const char* mask);

    // Установить параметры интеркома
    static void setIntercom(const char* remoteIp, uint16_t ctrlPort, uint16_t audioPort);

    // Получить имя устройства по умолчанию (на основе MAC)
    static void setDefaultName();

private:
    static DeviceConfig cfg;
};
