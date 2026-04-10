/*
 * recorder.cpp - Recording backend: SD card with RAM fallback
 *
 * Flow:
 *   init() → try SD card → if OK, use SD; else allocate RAM buffer
 *   Recording: startRecording() → recordSamples() → stopRecording()
 *   Playback: startPlayback() → getPlaybackFrame() → auto-stop
 *
 * SD card stores raw PCM files: /last_msg.pcm (16-bit, mono, sample_rate Hz)
 * RAM uses a ring buffer sized to fit REC_TARGET_SEC (shrinks if heap low).
 */

#include "recorder.h"
#include "pins.h"
#include "config.h"
#include <Arduino.h>

#ifdef USE_SD
#include <SD.h>
#include <SPI.h>
#endif

// ==================== Constants ====================

// RAM fallback: target 10 seconds, minimum 3 seconds
static const uint32_t REC_TARGET_SEC = 10;
static const uint32_t REC_MIN_SEC = 3;
static const uint32_t REC_HEAP_RESERVE = 80000;  // Reserve 80KB for system

// SD card file
#define SD_FILENAME "/last_msg.pcm"

// ==================== Static members ====================

Recorder::StorageType Recorder::storage = Recorder::STORE_NONE;
uint32_t Recorder::sampleRate = 16000;

volatile bool Recorder::recActive = false;
bool Recorder::recAvailable = false;
uint32_t Recorder::recStartTime = 0;
uint32_t Recorder::recDurationMs = 0;
volatile uint32_t Recorder::recWrittenSamples = 0;

volatile bool Recorder::playActive = false;

// SD state
bool Recorder::sdMounted = false;
bool Recorder::sdFileOpen = false;
uint32_t Recorder::sdFileSize = 0;

// RAM state
int16_t* Recorder::ramBuffer = nullptr;
uint32_t Recorder::ramCapacity = 0;
volatile uint32_t Recorder::ramWritePos = 0;
volatile uint32_t Recorder::ramRecLen = 0;
volatile uint32_t Recorder::ramReadPos = 0;


// ==================== Initialization ====================

bool Recorder::init(uint32_t sr) {
    sampleRate = sr;
    storage = STORE_NONE;

#ifdef USE_SD
    if (initSD()) {
        storage = STORE_SD;
        Serial.println("[REC] Storage: SD card");
        return true;
    }
    Serial.println("[REC] SD not available, trying RAM...");
#endif

    if (initRAM(sr)) {
        storage = STORE_RAM;
        Serial.printf("[REC] Storage: RAM (%.1f sec)\n", (float)ramCapacity / sr);
        return true;
    }

    storage = STORE_NONE;
    Serial.println("[REC] WARNING: No storage available for recording!");
    return false;
}


// ==================== SD Card Backend ====================

#ifdef USE_SD

bool Recorder::initSD() {
    Serial.println("[REC] Trying SD card...");

    // On WT32-ETH01, SD pins conflict with Ethernet — skip
#ifdef USE_ETHERNET
    Serial.println("[REC] SD not supported on WT32-ETH01 (pin conflict)");
    return false;
#endif

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

    if (!SD.begin(SD_CS)) {
        Serial.println("[REC] SD.begin() failed — no card or bad wiring");
        SPI.end();
        return false;
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    uint64_t freeSpace = (SD.totalBytes() - SD.usedBytes()) / (1024 * 1024);
    Serial.printf("[REC] SD: %llu MB total, %llu MB free\n", cardSize, freeSpace);

    // Delete old recording file if exists
    if (SD.exists(SD_FILENAME)) {
        SD.remove(SD_FILENAME);
    }

    sdMounted = true;
    sdFileOpen = false;
    sdFileSize = 0;
    return true;
}

void Recorder::sdStartWrite() {
    if (!sdMounted) return;

    // Remove old file
    if (SD.exists(SD_FILENAME)) {
        SD.remove(SD_FILENAME);
    }

    File f = SD.open(SD_FILENAME, FILE_WRITE);
    if (f) {
        sdFileOpen = true;
        sdFileSize = 0;
        f.close();
    } else {
        Serial.println("[REC] ERROR: Cannot create file on SD");
        sdFileOpen = false;
    }
}

void Recorder::sdWriteSamples(const int16_t* samples, int count) {
    if (!sdFileOpen) return;

    File f = SD.open(SD_FILENAME, FILE_APPEND);
    if (!f) {
        sdFileOpen = false;
        return;
    }

    size_t bytes = count * sizeof(int16_t);
    size_t written = f.write((const uint8_t*)samples, bytes);
    f.close();

    sdFileSize += written;
}

void Recorder::sdStopWrite() {
    sdFileOpen = false;
    // File is already closed after each write
    Serial.printf("[REC] SD file written: %u bytes (%.1f sec)\n",
                  sdFileSize, (float)sdFileSize / (sampleRate * sizeof(int16_t)));
}

void Recorder::sdStartRead() {
    // File will be opened on first read
}

void Recorder::sdStopRead() {
    // File is closed after each read
}

int Recorder::sdReadSamples(int16_t* buffer, int maxSamples) {
    static File readFile;
    static bool fileOpened = false;
    static uint32_t readPos = 0;

    if (!sdMounted || !recAvailable) return 0;

    if (!fileOpened) {
        readFile = SD.open(SD_FILENAME, FILE_READ);
        if (!readFile) {
            return 0;
        }
        fileOpened = true;
        readPos = 0;
        // Seek to current position
        readFile.seek(readPos);
    }

    // Calculate how many bytes left
    uint32_t bytesLeft = sdFileSize - readPos;
    if (bytesLeft == 0) {
        readFile.close();
        fileOpened = false;
        readPos = 0;
        return 0;
    }

    int bytesToRead = min((uint32_t)(maxSamples * sizeof(int16_t)), bytesLeft);
    int bytesRead = readFile.read((uint8_t*)buffer, bytesToRead);
    readPos += bytesRead;

    if (readPos >= sdFileSize) {
        readFile.close();
        fileOpened = false;
        readPos = 0;
    }

    return bytesRead / sizeof(int16_t);
}

void Recorder::sdDeleteFile() {
    if (sdMounted && SD.exists(SD_FILENAME)) {
        SD.remove(SD_FILENAME);
    }
    sdFileSize = 0;
}

#endif // USE_SD


// ==================== RAM Fallback Backend ====================

bool Recorder::initRAM(uint32_t sr) {
    uint32_t targetSamples = sr * REC_TARGET_SEC;
    uint32_t minSamples = sr * REC_MIN_SEC;
    uint32_t targetBytes = targetSamples * sizeof(int16_t);

    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxBytes = (freeHeap > REC_HEAP_RESERVE) ? (freeHeap - REC_HEAP_RESERVE) : 0;

    if (maxBytes < minSamples * sizeof(int16_t)) {
        ramBuffer = nullptr;
        ramCapacity = 0;
        Serial.printf("[REC] Not enough RAM: heap=%u KB, need min=%u KB\n",
                      freeHeap / 1024, (minSamples * sizeof(int16_t)) / 1024);
        return false;
    }

    if (targetBytes > maxBytes) {
        targetSamples = maxBytes / sizeof(int16_t);
        targetBytes = targetSamples * sizeof(int16_t);
    }

    ramBuffer = (int16_t*)malloc(targetBytes);
    if (!ramBuffer) {
        ramCapacity = 0;
        Serial.println("[REC] ERROR: malloc failed for RAM buffer!");
        return false;
    }

    ramCapacity = targetSamples;
    return true;
}

void Recorder::ramWriteSamples(const int16_t* samples, int count) {
    if (!ramBuffer) return;

    for (int i = 0; i < count; i++) {
        ramBuffer[ramWritePos % ramCapacity] = samples[i];
        ramWritePos++;
    }

    // Ring buffer: length is min of total written and capacity
    ramRecLen = (ramWritePos > ramCapacity) ? ramCapacity : ramWritePos;
}

int Recorder::ramReadSamples(int16_t* buffer, int maxSamples) {
    if (!ramBuffer || ramRecLen == 0) return 0;

    // Calculate start of recording in ring buffer
    uint32_t startOffset = (ramWritePos >= ramRecLen) ? (ramWritePos - ramRecLen) : 0;
    uint32_t currentPos = startOffset + ramReadPos;
    uint32_t samplesLeft = ramRecLen - ramReadPos;
    if (samplesLeft == 0) return 0;

    int toRead = min(maxSamples, (int)samplesLeft);

    for (int i = 0; i < toRead; i++) {
        buffer[i] = ramBuffer[(currentPos + i) % ramCapacity];
    }

    ramReadPos += toRead;

    if ((uint32_t)ramReadPos >= ramRecLen) {
        ramReadPos = 0;
        return toRead;  // Signal end: caller checks ramReadPos reset
    }

    return toRead;
}


// ==================== Public API (delegates to backend) ====================

void Recorder::startRecording() {
    if (storage == STORE_NONE) return;

    playActive = false;
    recActive = false;
    recAvailable = false;
    recDurationMs = 0;
    recWrittenSamples = 0;
    recStartTime = millis();

#ifdef USE_SD
    if (storage == STORE_SD) {
        sdStartWrite();
    }
#endif
    if (storage == STORE_RAM) {
        ramWritePos = 0;
        ramRecLen = 0;
        ramReadPos = 0;
    }

    recActive = true;
    Serial.printf("[REC] Recording started (storage: %s)\n", getStorageName());
}

void Recorder::stopRecording() {
    if (!recActive) return;
    recActive = false;

    if (storage == STORE_RAM && recWrittenSamples > 0) {
        recAvailable = true;
        recDurationMs = (uint32_t)((float)ramRecLen / sampleRate * 1000);
    }

#ifdef USE_SD
    if (storage == STORE_SD) {
        sdStopWrite();
        if (sdFileSize > 0) {
            recAvailable = true;
            recDurationMs = (uint32_t)((float)sdFileSize / (sampleRate * sizeof(int16_t)) * 1000);
        }
    }
#endif

    if (recAvailable) {
        Serial.printf("[REC] Recording saved: %u ms (%s)\n", recDurationMs, getStorageName());
    } else {
        Serial.println("[REC] Recording empty (0 samples)");
    }
}

void Recorder::recordSamples(const int16_t* samples, int count) {
    if (!recActive || storage == STORE_NONE || count <= 0) return;

    recWrittenSamples += count;

    if (storage == STORE_RAM) {
        ramWriteSamples(samples, count);
    }

#ifdef USE_SD
    if (storage == STORE_SD) {
        sdWriteSamples(samples, count);
    }
#endif
}

bool Recorder::isRecording() {
    return recActive;
}

bool Recorder::hasRecording() {
    return recAvailable && recDurationMs > 0;
}

uint32_t Recorder::getDuration() {
    return recDurationMs;
}

Recorder::StorageType Recorder::getStorage() {
    return storage;
}

const char* Recorder::getStorageName() {
    switch (storage) {
        case STORE_SD:  return "SD";
        case STORE_RAM: return "RAM";
        default:        return "none";
    }
}

uint32_t Recorder::getMaxDuration() {
    if (storage == STORE_RAM && ramCapacity > 0) {
        return ramCapacity / sampleRate;
    }
    if (storage == STORE_SD) {
        // Practically unlimited — report a large number
        return 3600;  // 1 hour
    }
    return 0;
}

bool Recorder::startPlayback() {
    if (!recAvailable || storage == STORE_NONE) return false;
    if (playActive) return false;

#ifdef USE_SD
    if (storage == STORE_SD) {
        sdStartRead();
    }
#endif
    if (storage == STORE_RAM) {
        ramReadPos = 0;
    }

    playActive = true;
    Serial.printf("[REC] Playback started: %u ms (%s)\n", recDurationMs, getStorageName());
    return true;
}

bool Recorder::isPlaybackActive() {
    return playActive;
}

int Recorder::getPlaybackFrame(int16_t* buffer, int maxSamples) {
    if (!playActive || storage == STORE_NONE) return 0;

    int samplesRead = 0;

    if (storage == STORE_RAM) {
        uint32_t prevReadPos = ramReadPos;
        samplesRead = ramReadSamples(buffer, maxSamples);
        // ramReadSamples resets to 0 when done
        if (ramReadPos == 0 && samplesRead > 0 && prevReadPos + samplesRead >= (int)ramRecLen) {
            playActive = false;
        } else if (samplesRead == 0) {
            playActive = false;
        }
    }

#ifdef USE_SD
    if (storage == STORE_SD) {
        samplesRead = sdReadSamples(buffer, maxSamples);
        if (samplesRead == 0) {
            playActive = false;
            sdStopRead();
        }
    }
#endif

    return samplesRead;
}

void Recorder::clearRecording() {
    playActive = false;
    recAvailable = false;
    recDurationMs = 0;
    recWrittenSamples = 0;

    if (storage == STORE_RAM) {
        ramWritePos = 0;
        ramRecLen = 0;
        ramReadPos = 0;
    }

#ifdef USE_SD
    if (storage == STORE_SD) {
        sdDeleteFile();
    }
#endif

    Serial.println("[REC] Recording cleared");
}
