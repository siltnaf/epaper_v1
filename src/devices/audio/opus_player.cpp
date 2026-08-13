#include "devices/audio/opus_player.h"

#include <AudioFileSourceFS.h>
#include <AudioGeneratorOpus.h>
#include <AudioOutput.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

#include "board_pins.h"
#include "devices/es8311/es8311.h"

#include <cstring>

namespace {

// ESP-IDF expresses task stack sizes and high-water marks in bytes. A 16 KiB
// task left no heap block large enough for the 17,776-byte mono decoder state.
// The libopus CELT low-stack path removes its ~4 KiB normalized-MDCT VLA, making
// this 8 KiB stack compatible with the contiguous-heap requirement.
constexpr uint32_t PLAYBACK_TASK_STACK_BYTES = 8192;

class Es8311AudioOutput final : public AudioOutput {
public:
    explicit Es8311AudioOutput(Es8311 *audio) : audio_(audio) {}

    bool begin() override {
        bufferedSamples_ = 0;
        sourceFrame_ = 0;
        sumLeft_ = 0;
        sumRight_ = 0;
        active_ = audio_ && audio_->isInitialized();
        if (active_) audio_->setSpeakerEnabled(true);
        return active_;
    }

    bool SetRate(int hz) override {
        sourceRate_ = hz;
        // Ogg Opus always decodes at 48 kHz. The board codec is initialized at
        // 16 kHz, so consume one averaged output frame for every three decoded
        // frames. Reject unexpected rates rather than playing at the wrong pitch.
        return hz == 48000 && AudioOutput::SetRate(hz);
    }

    bool SetBitsPerSample(int bits) override {
        return bits == 16 && AudioOutput::SetBitsPerSample(bits);
    }
    bool SetChannels(int channels) override {
        return (channels == 1 || channels == 2) && AudioOutput::SetChannels(channels);
    }

    void resetQuota() { sourceFramesRemaining_ = 768; }

    bool ConsumeSample(int16_t sample[2]) override {
        if (!active_ || sourceFramesRemaining_ == 0) return false;
        --sourceFramesRemaining_;
        MakeSampleStereo16(sample);
        sumLeft_ += Amplify(sample[LEFTCHANNEL]);
        sumRight_ += Amplify(sample[RIGHTCHANNEL]);
        ++sourceFrame_;
        if (sourceFrame_ < 3) return true;

        pcm_[bufferedSamples_++] = static_cast<int16_t>(sumLeft_ / 3);
        pcm_[bufferedSamples_++] = static_cast<int16_t>(sumRight_ / 3);
        sourceFrame_ = 0;
        sumLeft_ = 0;
        sumRight_ = 0;
        if (bufferedSamples_ < PCM_SAMPLE_CAPACITY) return true;
        return flushBuffer();
    }

    bool stop() override {
        flush();
        active_ = false;
        return true;
    }

    void flush() override {
        if (sourceFrame_ > 0 && bufferedSamples_ + 2 <= PCM_SAMPLE_CAPACITY) {
            pcm_[bufferedSamples_++] = static_cast<int16_t>(sumLeft_ / sourceFrame_);
            pcm_[bufferedSamples_++] = static_cast<int16_t>(sumRight_ / sourceFrame_);
            sourceFrame_ = 0;
            sumLeft_ = 0;
            sumRight_ = 0;
        }
        flushBuffer();
    }

private:
    bool flushBuffer() {
        if (bufferedSamples_ == 0) return true;
        if (!audio_ || audio_->write(pcm_, bufferedSamples_, 1000) != bufferedSamples_) {
            Serial.printf("[OPUS] ES8311 short write samples=%u\n", bufferedSamples_);
            return false;
        }
        bufferedSamples_ = 0;
        return true;
    }

    static constexpr uint16_t PCM_SAMPLE_CAPACITY = 512;
    Es8311 *audio_ = nullptr;
    int16_t pcm_[PCM_SAMPLE_CAPACITY] = {};
    uint16_t bufferedSamples_ = 0;
    uint16_t sourceFramesRemaining_ = 0;
    uint8_t sourceFrame_ = 0;
    int32_t sumLeft_ = 0;
    int32_t sumRight_ = 0;
    int sourceRate_ = 0;
    bool active_ = false;
};

Es8311 *codec = nullptr;
AudioFileSourceFS *source = nullptr;
AudioGeneratorOpus *decoder = nullptr;
Es8311AudioOutput *output = nullptr;
TaskHandle_t playbackTaskHandle = nullptr;
volatile bool playbackActive = false;
volatile bool stopRequested = false;
volatile bool pauseRequested = false;
volatile bool playbackPaused = false;
volatile TickType_t playbackStartedTick = 0;
char requestedPath[128] = {};

void releasePlayback() {
    if (decoder) {
        if (decoder->isRunning()) decoder->stop();
        delete decoder;
        decoder = nullptr;
    }
    if (source) {
        source->close();
        delete source;
        source = nullptr;
    }
    delete output;
    output = nullptr;
}

void playbackTask(void *) {
    // NS4150B is downstream of the ES8311 analog output. Assert its active-high
    // power-amplifier enable explicitly for every playback session and allow
    // the amplifier to settle before decoded PCM reaches I2S.
    pinMode(BoardPins::PA_EN, OUTPUT);
    digitalWrite(BoardPins::PA_EN, HIGH);
    if (codec) codec->setSpeakerEnabled(true);
    vTaskDelay(pdMS_TO_TICKS(60));
    Serial.printf("[AUDIO PA] NS4150B enabled PA_EN=%d level=%d\n",
                  BoardPins::PA_EN, digitalRead(BoardPins::PA_EN));
    Serial.printf("[OPUS] Heap before open free=%u largest=%u mono_decoder_state=%d stack_free=%u\n",
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                  opus_decoder_get_size(1),
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    decoder = new AudioGeneratorOpus();
    const bool decoderReserved = decoder && decoder->reserveDecoder();
    Serial.printf("[OPUS] Decoder reserve=%s state=%d heap_free=%u largest=%u\n",
                  decoderReserved ? "yes" : "no", opus_decoder_get_size(1),
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

    // Claim the 4 KiB PCM block before the smaller file/output objects can
    // fragment the remaining 7 KiB contiguous region.
    const bool buffersReserved = decoderReserved && decoder->reserveBuffers();
    Serial.printf("[OPUS] PCM reserve=%s bytes=%u heap_free=%u largest=%u\n",
                  buffersReserved ? "yes" : "no", 2048U * sizeof(opus_int16),
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    if (buffersReserved) {
        source = new AudioFileSourceFS(SD_MMC, requestedPath);
        output = new Es8311AudioOutput(codec);
    }
    bool started = buffersReserved && source && source->isOpen() && output;
    if (source && source->isOpen()) {
        uint8_t header[64] = {};
        const uint32_t headerBytes = source->read(header, sizeof(header));
        const bool oggSignature = headerBytes >= 4 && std::memcmp(header, "OggS", 4) == 0;
        bool opusHead = false;
        for (uint32_t i = 0; i + 8 <= headerBytes; ++i) {
            if (std::memcmp(header + i, "OpusHead", 8) == 0) {
                opusHead = true;
                break;
            }
        }
        const bool rewound = source->seek(0, SEEK_SET);
        Serial.printf("[OPUS] Header bytes=%u first=%02X%02X%02X%02X OggS=%s "
                      "OpusHead=%s rewind=%s pos=%u\n",
                      headerBytes, header[0], header[1], header[2], header[3],
                      oggSignature ? "yes" : "no", opusHead ? "yes" : "no",
                      rewound ? "yes" : "no", source->getPos());
        Serial.print("[OPUS] Header hex=");
        for (uint32_t i = 0; i < headerBytes; ++i) Serial.printf("%02X", header[i]);
        Serial.println();
        started = started && oggSignature && opusHead && rewound;
    }
    if (started) {
        output->SetGain(1.0f);
        started = decoder->begin(source, output);
    }
    if (!started) {
        const int decoderError = decoder ? decoder->getLastError() : 0;
        Serial.printf("[OPUS] Decoder begin failed path=%s bytes=%u open=%s stage=%d "
                      "error=%d(%s) decoder_state=%d heap_free=%u largest=%u stack_free=%u\n",
                      requestedPath, source ? source->getSize() : 0,
                      source && source->isOpen() ? "yes" : "no",
                      decoder ? decoder->getOpenStage() : 0,
                      decoderError,
                      decoderError == OPUS_ALLOC_FAIL ? "allocation-failed" : "other",
                      opus_decoder_get_size(1),
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    } else {
        Serial.printf("[OPUS] Playback started path=%s bytes=%u source=48000Hz output=16000Hz stack=%u\n",
                      requestedPath, source->getSize(),
                      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        while (!stopRequested && decoder && decoder->isRunning()) {
            if (pauseRequested) {
                if (!playbackPaused) {
                    playbackPaused = true;
                    digitalWrite(BoardPins::PA_EN, LOW);
                    Serial.println("[OPUS] Playback paused");
                }
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            if (playbackPaused) {
                playbackPaused = false;
                digitalWrite(BoardPins::PA_EN, HIGH);
                vTaskDelay(pdMS_TO_TICKS(20));
                Serial.println("[OPUS] Playback resumed");
            }
            output->resetQuota();
            if (!decoder->loop()) break;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        Serial.printf("[OPUS] Playback %s stack_free=%u error=%d\n",
                      stopRequested ? "stopped" : "complete",
                      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                      decoder ? decoder->getLastError() : 0);
    }
    releasePlayback();
    // Shut the external amplifier down only after the decoder/output has been
    // flushed and stopped, preventing a cutoff transient at the speaker.
    digitalWrite(BoardPins::PA_EN, LOW);
    Serial.printf("[AUDIO PA] NS4150B disabled PA_EN=%d level=%d\n",
                  BoardPins::PA_EN, digitalRead(BoardPins::PA_EN));
    playbackActive = false;
    pauseRequested = false;
    playbackPaused = false;
    playbackTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

}

namespace OpusPlayer {

void setAudio(Es8311 *audio) {
    if (codec != audio) stop();
    codec = audio;
}

bool play(const char *path) {
    stop();
    if (!codec || !codec->isInitialized() || !path || path[0] == '\0' ||
        !SD_MMC.exists(path)) {
        Serial.printf("[OPUS] Cannot play path=%s codec=%s\n",
                      path ? path : "(null)",
                      codec && codec->isInitialized() ? "ready" : "not-ready");
        return false;
    }

    std::strncpy(requestedPath, path, sizeof(requestedPath) - 1);
    requestedPath[sizeof(requestedPath) - 1] = '\0';
    stopRequested = false;
    pauseRequested = false;
    playbackPaused = false;
    playbackActive = true;
    playbackStartedTick = xTaskGetTickCount();

    const unsigned heapBeforeTask = static_cast<unsigned>(ESP.getFreeHeap());
    const unsigned largestBeforeTask = static_cast<unsigned>(
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (xTaskCreatePinnedToCore(playbackTask, "opus-playback", PLAYBACK_TASK_STACK_BYTES,
                                nullptr, 2,
                                &playbackTaskHandle, 0) != pdPASS) {
        playbackActive = false;
        playbackTaskHandle = nullptr;
        Serial.printf("[OPUS] Could not create playback task stack=%u "
                      "heap_before=%u largest_before=%u heap_after=%u largest_after=%u\n",
                      static_cast<unsigned>(PLAYBACK_TASK_STACK_BYTES),
                      heapBeforeTask, largestBeforeTask,
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        return false;
    }
    return true;
}

bool acceptsTouchStop(uint32_t interruptTick) {
    // Ignore an IRQ generated by the touch that launched playback. A genuine
    // later stop touch has a tick at or after playbackStartedTick.
    return playbackActive &&
           static_cast<int32_t>(interruptTick - playbackStartedTick) >= 0;
}

bool pause() {
    if (!playbackActive) return false;
    pauseRequested = true;
    const uint32_t started = millis();
    while (playbackActive && !playbackPaused && millis() - started < 500) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return playbackPaused;
}

bool resume() {
    if (!playbackActive) return false;
    pauseRequested = false;
    return true;
}

bool isPaused() { return playbackActive && (pauseRequested || playbackPaused); }

void IRAM_ATTR requestStopFromIsr() {
    // Kept for existing page-level ISR hooks. Shared touch/audio coordination
    // now decides whether to stop in task context using the interrupt timestamp.
}

void stop() {
    if (!playbackTaskHandle && !playbackActive) return;
    stopRequested = true;
    pauseRequested = false;
    const uint32_t started = millis();
    while (playbackTaskHandle && millis() - started < 2000) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

bool loop() {
    return playbackActive;
}

bool isPlaying() { return playbackActive; }

}