#pragma once
/*
 * recorder.h - Module for recording the last received transmission
 *
 * Supports two storage backends:
 *   1. SD card via SPI (ESP32 DevKit only) - practically unlimited
 *   2. RAM ring buffer (fallback) - ~5 seconds
 *
 * On WT32-ETH01, SD is not available due to pin conflicts with Ethernet.
 * The module auto-detects at init() time and uses the best available backend.
 *
 * API:
 *   init()          - detect SD, fall back to RAM
 *   startRecording()- begin capturing incoming audio
 *   recordSamples() - feed audio data (called from receive task)
 *   stopRecording() - finalize recording, mark as available
 *   hasRecording()  - is there a saved message?
 *   getDuration()   - duration in milliseconds
 *   getStorage()    - "SD" or "RAM"
 *   startPlayback() - begin playing back the recording
 *   isPlaybackActive() - currently playing?
 *   getPlaybackFrame()  - get next chunk for speaker
 *   clearRecording()- delete saved message
 *   isRecording()   - currently capturing?
 */

#include <cstdint>

class Recorder {
public:
    enum StorageType : uint8_t {
        STORE_NONE = 0,
        STORE_RAM,
        STORE_SD
    };

    // Initialize: try SD first, fall back to RAM
    static bool init(uint32_t sampleRate);

    // Recording
    static void startRecording();
    static void stopRecording();
    static void recordSamples(const int16_t* samples, int count);
    static bool isRecording();

    // Query
    static bool hasRecording();
    static uint32_t getDuration();
    static StorageType getStorage();
    static const char* getStorageName();
    static uint32_t getMaxDuration();  // Max possible recording time in seconds

    // Playback
    static bool startPlayback();
    static bool isPlaybackActive();
    static int getPlaybackFrame(int16_t* buffer, int maxSamples);

    // Clear
    static void clearRecording();

private:
    // ===== SD Card backend =====
    static bool initSD();
    static void sdWriteSamples(const int16_t* samples, int count);
    static void sdStartWrite();
    static void sdStopWrite();
    static void sdStartRead();
    static void sdStopRead();
    static int sdReadSamples(int16_t* buffer, int maxSamples);
    static void sdDeleteFile();

    // ===== RAM fallback backend =====
    static bool initRAM(uint32_t sampleRate);
    static void ramWriteSamples(const int16_t* samples, int count);
    static int ramReadSamples(int16_t* buffer, int maxSamples);

    // State
    static StorageType storage;
    static uint32_t sampleRate;

    // Recording state
    static volatile bool recActive;
    static bool recAvailable;
    static uint32_t recStartTime;
    static uint32_t recDurationMs;     // Final duration after stopRecording()
    static volatile uint32_t recWrittenSamples;

    // Playback state
    static volatile bool playActive;

    // ===== SD state =====
    static bool sdMounted;
    static bool sdFileOpen;
    static uint32_t sdFileSize;       // Bytes written to current file

    // ===== RAM state =====
    static int16_t* ramBuffer;
    static uint32_t ramCapacity;      // Max samples
    static volatile uint32_t ramWritePos;
    static volatile uint32_t ramRecLen;
    static volatile uint32_t ramReadPos;
};
