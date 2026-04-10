#pragma once
/*
 * pins.h - Определение пинов для ESP32 Intercom v2.0
 *
 * Две конфигурации:
 *
 * 1) ESP32 DevKit (USE_WIFI) — дом, подключение по Wi-Fi:
 *
 *    ESP32           ICS-43434 (I2S микрофон)
 *    GPIO14 (BCK) -> BCLK
 *    GPIO15 (WS)  -> LRCLK
 *    GPIO32 (DIN) <- DOUT
 *    3.3V         -> VDD, L/R, GND
 *
 *    ESP32           MAX98357A
 *    GPIO26 (BCK) -> BCLK
 *    GPIO27 (WS)  -> LRCLK
 *    GPIO25 (DOUT)-> DIN
 *
 *    GPIO4  — кнопка (внутренняя подтяжка)
 *    GPIO2  — LED
 *
 *
 * 2) WT32-ETH01 v1.4 (USE_ETHERNET) — гараж, подключение по кабелю:
 *
 *    Ethernet (RMII) занимает: 16, 17, 18, 19, 21, 22, 23, 25, 26, 27
 *
 *    Свободные для I2S и периферии: 2, 4, 5, 12, 13, 14, 32, 33, 34, 35
 *
 *    WT32-ETH01       ICS-43434 (I2S микрофон, I2S_NUM_0)
 *    GPIO32 (BCK) -> BCLK
 *    GPIO33 (WS)  -> LRCLK
 *    GPIO34 (DIN) <- DOUT
 *
 *    WT32-ETH01       MAX98357A (I2S_NUM_1)
 *    GPIO13 (BCK) -> BCLK
 *    GPIO12 (WS)  -> LRCLK
 *    GPIO5  (DOUT)-> DIN
 *
 *    GPIO14 — кнопка
 *    GPIO2  — LED (встроенный синий)
 */

// ==================================================================
//  ПИНЫ — автоматический выбор по build flag
// ==================================================================

#ifdef USE_ETHERNET
// ==================== WT32-ETH01 v1.4 ====================

// I2S микрофон ICS-43434 (I2S_NUM_0, RX)
#define I2S_MIC_PORT       I2S_NUM_0
#define I2S_MIC_BCK        32
#define I2S_MIC_WS         33
#define I2S_MIC_DIN        34

// I2S динамик MAX98357A (I2S_NUM_1, TX)
#define I2S_SPK_PORT       I2S_NUM_1
#define I2S_SPK_BCK        13
#define I2S_SPK_WS         12
#define I2S_SPK_DOUT       5

// Кнопка PTT
#define BUTTON_PIN         14
#define BUTTON_DEBOUNCE_MS 50

// LED
#define LED_PIN            2
#define LED_ON             LOW
#define LED_OFF            HIGH

#else
// ==================== ESP32 DevKit (WiFi) ====================

// I2S микрофон ICS-43434 (I2S_NUM_0, RX)
#define I2S_MIC_PORT       I2S_NUM_0
#define I2S_MIC_BCK        14
#define I2S_MIC_WS         15
#define I2S_MIC_DIN        32

// I2S динамик MAX98357A (I2S_NUM_1, TX)
#define I2S_SPK_PORT       I2S_NUM_1
#define I2S_SPK_BCK        26
#define I2S_SPK_WS         27
#define I2S_SPK_DOUT       25

// Кнопка PTT
#define BUTTON_PIN         4
#define BUTTON_DEBOUNCE_MS 50

// LED
#define LED_PIN            2
#define LED_ON             LOW
#define LED_OFF            HIGH

#endif // USE_ETHERNET


// ==================================================================
//  ОБЩИЕ ПАРАМЕТРЫ (для обеих плат)
// ==================================================================

// Аудио
#define SAMPLE_RATE        16000
#define BITS_PER_SAMPLE    16
#define FRAME_SIZE_MS      20
#define FRAME_SAMPLES      (SAMPLE_RATE * FRAME_SIZE_MS / 1000)  // 320
#define FRAME_BYTES        (FRAME_SAMPLES * BITS_PER_SAMPLE / 8) // 640

// Сеть
#define DEFAULT_CTRL_PORT  8080   // Управляющие сообщения UDP
#define DEFAULT_AUDIO_PORT 8081   // Аудио поток UDP
#define UDP_TIMEOUT_MS     50
#define PEER_ONLINE_MS     15000  // Таймаут he's-online

// Веб
#define WEB_PORT           80
#define AP_SSID_PREFIX     "Intercom_ESP32"
#define AP_PASSWORD        "12345678"

// FreeRTOS
#define AUDIO_TASK_STACK   4096
#define NET_TASK_STACK     4096

// Ethernet (WT32-ETH01 v1.4)
#ifdef USE_ETHERNET
#define ETH_PHY_ADDR       1
#define ETH_PHY_POWER      16
#define ETH_MDC_PIN        23
#define ETH_MDIO_PIN       18
#define ETH_PHY_TYPE       ETH_PHY_LAN8720
#define ETH_CLK_MODE       ETH_CLOCK_GPIO17_OUT
#endif
