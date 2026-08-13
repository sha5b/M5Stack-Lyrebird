// The CoreS3 backend: 16-bit PCM out through M5Unified's Speaker.
//
// The ESP32-S3 has no DAC, so none of the Fire's 8-bit machinery applies here —
// no dither, no noise shaping, no mid-level priming, no rail-slam on underrun.
// The CoreS3 sends I2S to an AW88298 class-D amplifier which needs I2C setup and
// a reset line off an AW9523 expander, and M5Unified already does all of that in
// M5.begin(); reimplementing it against a chip we would be guessing at is how
// you get a silent board and no way to tell why.
//
// So this file only produces PCM and keeps the queue fed. M5Unified's speaker
// holds two wave slots per channel: isPlaying() returns 2 when both are taken,
// which is the backpressure this loop paces itself against — the same job
// i2s_write's blocking does on the Fire.
#include "audio.h"
#include "syrinx.h"

#include <Arduino.h>
#include <M5Unified.h>

#define BLOCK_LEN 256           // frames per queued buffer (~11.6 ms at 22.05 kHz)
#define SPK_CHANNEL 0
#define AUDIO_TASK_PRIO 3       // above loopTask, so a repaint cannot starve audio
#define AUDIO_TASK_CORE 1

static volatile bool s_running = false;
static volatile bool s_idleAck = true;
static volatile float s_volume = 0.7f;
static volatile float s_load = 0.0f;

static float s_floatBuf[BLOCK_LEN];
// Two buffers, alternating: playRaw keeps a pointer to what it was handed, so
// the one in flight must not be the one being written.
static int16_t s_pcm[2][BLOCK_LEN];
static uint8_t s_which = 0;

static void waitAudioIdle() {
    for (int i = 0; i < 60 && !s_idleAck; i++) delay(5);
}

static void audioTask(void*) {
    const float blockUs = (float)BLOCK_LEN * 1e6f / (float)AUDIO_SAMPLE_RATE;
    while (true) {
        if (!s_running) {
            s_idleAck = true;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        s_idleAck = false;

        // Both slots full: nothing to do until one drains. This is the clock.
        if (M5.Speaker.isPlaying(SPK_CHANNEL) >= 2) {
            vTaskDelay(1);
            continue;
        }

        uint32_t t0 = micros();
        syrinxRender(s_floatBuf, BLOCK_LEN);
        int16_t* out = s_pcm[s_which];
        s_which ^= 1;
        float vol = s_volume;
        for (int i = 0; i < BLOCK_LEN; i++) {
            float s = s_floatBuf[i] * vol;
            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;
            out[i] = (int16_t)lroundf(s * 32767.0f);
        }
        float used = (float)(micros() - t0) / blockUs;
        s_load = s_load * 0.9f + used * 0.1f;

        // repeat 1, stop_current_sound false: append to the channel's queue
        M5.Speaker.playRaw(out, BLOCK_LEN, AUDIO_SAMPLE_RATE, false, 1, SPK_CHANNEL, false);
    }
}

void audioInit() {
    syrinxInit(AUDIO_SAMPLE_RATE);

    auto cfg = M5.Speaker.config();
    cfg.sample_rate = AUDIO_SAMPLE_RATE;  // matches our render rate: no resampling
    // M5Unified defaults this to 4 on the CoreS3 because it expects quiet source
    // material. Ours is already full-scale, and any multiplier above 1 would clip
    // the chorus into the amplifier.
    cfg.magnification = 1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = BLOCK_LEN;
    cfg.task_priority = AUDIO_TASK_PRIO;
    cfg.task_pinned_core = AUDIO_TASK_CORE;
    M5.Speaker.config(cfg);
    M5.Speaker.begin();
    M5.Speaker.setVolume(255);  // level is ours to set; do not attenuate twice

    xTaskCreatePinnedToCore(audioTask, "audioTask", 6144, nullptr,
                            AUDIO_TASK_PRIO, nullptr, AUDIO_TASK_CORE);
}

void audioSetRunning(bool on) {
    if (on == s_running) return;
    if (on) {
        waitAudioIdle();
        M5.Speaker.begin();
        s_running = true;
    } else {
        s_running = false;
        waitAudioIdle();
        syrinxPanic();
        // Drops the queue and powers the amplifier down through M5Unified's
        // enable callback, which is what keeps the CoreS3 quiet when paused.
        M5.Speaker.stop();
        M5.Speaker.end();
    }
}

void audioSetVolume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    s_volume = v;
}

float audioGetVolume() { return s_volume; }

float audioGetLoad() { return s_load; }
