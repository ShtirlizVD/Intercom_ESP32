#pragma once
/*
 * audio.h - Audio I/O module via I2S
 *
 * Uses two ESP32 I2S ports:
 * - I2S_NUM_0: Input from I2S microphone (ICS-43434)
 * - I2S_NUM_1: Output to MAX98357A amplifier
 *
 * Mic outputs 24-bit I2S data in 32-bit frames.
 * We extract 16-bit samples for transmission.
 * Speaker accepts 16-bit samples directly.
 *
 * Tone generator for notifications.
 * Recording/playback delegated to Recorder module.
 */

#include <cstdint>
#include <driver/i2s.h>

class Audio {
public:
    // Initialize I2S ports (mic + speaker)
    static bool init();

    // Deinitialize I2S
    static void deinit();

    // Read audio frame from microphone
    // Returns number of samples (16-bit, mono)
    static int readFrame(int16_t* buffer, int maxSamples);

    // Write audio frame to speaker
    // Returns number of written samples
    static int writeFrame(const int16_t* buffer, int samples);

    // Silence speaker (clear DMA buffer)
    static void silenceSpeaker();

    // Gain control
    static void setMicGain(uint8_t gain);     // 0-10
    static void setVolume(uint8_t volume);    // 0-10

    // Get current values
    static uint8_t getMicGain();
    static uint8_t getVolume();

    // Frame size info
    static int getFrameSamples();
    static int getFrameBytes();
    static uint32_t getSampleRate();

    // Reconfigure sample rate
    static bool setSampleRate(uint32_t sampleRate);

    // ===== Tone generator =====

    // Play error tone (peer offline) — two-tone "du-du"
    static void playErrorTone();

    // Play confirm tone (short beep)
    static void playConfirmTone();

    // Play cancel tone (peer disconnected during transmission)
    static void playCancelTone();

    // Is a tone currently playing?
    static bool isTonePlaying();

    // Generate next tone frame (for audioReceiveTask)
    // Returns sample count, 0 if tone finished
    static int getToneFrame(int16_t* buffer, int maxSamples);

private:
    static float micGainMult;   // Mic gain multiplier
    static float volumeMult;    // Volume multiplier
    static uint32_t currentSampleRate;
    static bool initialized;

    // Raw 32-bit buffer for mic data
    static const int RAW_BUF_SIZE = 1024;
    static int32_t rawMicBuffer[RAW_BUF_SIZE];

    // ===== Tone generator (state machine) =====
    enum ToneType : uint8_t {
        TONE_NONE = 0,      // No tone
        TONE_ERROR,         // "Du-du" — peer offline
        TONE_CONFIRM,       // "Beep" — confirm
        TONE_CANCEL         // "Da-da-da" — cancel
    };

    enum TonePhase : uint8_t {
        PHASE_OFF = 0,      // Off / pause between parts
        PHASE_BEEP,         // Tone signal
        PHASE_SILENCE       // Silent pause
    };

    // Tone parameters
    struct ToneStep {
        uint16_t freq;          // Frequency in Hz (0 = silence)
        uint16_t duration_ms;   // Duration in ms
    };

    static volatile ToneType toneType;
    static TonePhase tonePhase;
    static uint16_t toneFreq;
    static uint32_t tonePhaseStart;
    static uint32_t toneStepIndex;
    static const ToneStep* toneSequence;
    static uint32_t toneSequenceLen;
    static uint32_t toneSamplePhase;
    static int16_t toneVolume;

    // Tone patterns
    static const ToneStep errorSequence[];
    static const ToneStep confirmSequence[];
    static const ToneStep cancelSequence[];

    static void startTone(ToneType type, const ToneStep* seq, uint32_t len);
    static void advanceTonePhase();
};
