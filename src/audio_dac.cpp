// The M5Stack Fire backend: ESP32 I2S in built-in DAC mode. Not compiled for the
// CoreS3 — the ESP32-S3 has no DAC (see src/audio_spk.cpp).
#include "audio.h"
#include "syrinx.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <driver/dac.h>

#define AUDIO_DAC_PIN 25        // DAC1 -> the Fire's speaker amp
#define DMA_BUF_COUNT 8         // ~93 ms of slack: rides out an LCD redraw
#define DMA_BUF_LEN 256         // frames per DMA buffer (~11.6 ms at 22.05 kHz)
#define DAC_MID 0x8000          // unsigned mid-scale in the high byte

// Above loopTask (priority 1) so an LCD repaint can never delay a DMA refill.
// Deliberately on core 1 with loopTask rather than the empty core 0: the task
// watchdog watches IDLE0, so an overrun there would reboot the device, whereas
// on core 1 the worst case is a stuttering UI.
#define AUDIO_TASK_PRIO 3
#define AUDIO_TASK_CORE 1

static volatile bool s_running = false;
static volatile bool s_idleAck = true;  // audioTask confirms it has let go of s_dacBuf
static volatile float s_volume = 0.7f;
static volatile float s_load = 0.0f;

static float s_floatBuf[DMA_BUF_LEN];
static uint16_t s_dacBuf[DMA_BUF_LEN];

// 8-bit DAC dither/noise-shaping state. See quantize().
static float s_shapeErr = 0.0f;
static uint32_t s_rng = 0x1234567u;

static inline float urand() {  // xorshift32 -> uniform [-0.5, 0.5)
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng >> 8) * (1.0f / 16777216.0f) - 0.5f;
}

// The DAC is 8 bits, so how we get there matters more than usual. The first
// port did `(int)(s * 127.0f) + 128`, which truncates toward zero: that is a
// double-width step across zero, and the tail of every syllable decays through
// it as granular buzz. Instead: round, add half an LSB of TPDF dither so the
// error stops correlating with the signal, and feed the rounding error back
// first-order so the remaining noise is pushed up out of the bird band.
static inline uint16_t quantize(float s) {
    float want = s * 127.0f + 128.0f;
    float d = (urand() + urand()) * 0.5f;       // TPDF, +-0.5 LSB
    float w = want - s_shapeErr + d;            // NTF = 1 - z^-1
    int q = (int)lroundf(w);
    if (q < 0) q = 0;
    else if (q > 255) q = 255;
    float err = (float)q - w;                   // clamp keeps the loop from ringing
    s_shapeErr = err > 1.0f ? 1.0f : (err < -1.0f ? -1.0f : err);
    return (uint16_t)(q << 8);                  // DAC reads the high byte, unsigned
}

// Dither is only worth its noise where there is signal to decorrelate. Left
// running over digital silence it is just a hiss the speaker plays all day, and
// in chorus mode most of the wall clock *is* silence — 12 to 34 songs a minute,
// each under a second. So: a block that is exactly zero gets the flat mid-level
// code and nothing else, which is what the DAC held before dither existed.
//
// The test is on exact zeros rather than a threshold because syrinxRender
// memsets the buffer and only adds to it, so "no voice sounded" really is 0.0f
// and there is no gate to chatter.
static inline bool blockIsSilent(const float* buf, int n) {
    for (int i = 0; i < n; i++) {
        if (buf[i] != 0.0f) return false;
    }
    return true;
}

// The DAC takes the MSB of each 16-bit slot, unsigned — so 0x8000 is the
// mid-level. i2s_zero_dma_buffer writes 0x0000, which is full-negative and
// would pop the amp; always settle to mid-level instead.
static void primeMidLevel() {
    for (int i = 0; i < DMA_BUF_LEN; i++) s_dacBuf[i] = DAC_MID;
    for (int k = 0; k < DMA_BUF_COUNT; k++) {
        size_t written = 0;
        i2s_write(I2S_NUM_0, s_dacBuf, sizeof(s_dacBuf), &written, pdMS_TO_TICKS(100));
    }
}

// primeMidLevel() runs on loopTask but shares s_dacBuf and the I2S port with
// audioTask; both writing at once corrupts the buffer, which is exactly the pop
// the priming exists to avoid. Park the render task first.
static void waitAudioIdle() {
    for (int i = 0; i < 60 && !s_idleAck; i++) delay(5);
}

static void audioTask(void*) {
    const float blockUs = (float)DMA_BUF_LEN * 1e6f / (float)AUDIO_SAMPLE_RATE;
    while (true) {
        if (!s_running) {
            s_idleAck = true;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        s_idleAck = false;
        uint32_t t0 = micros();
        syrinxRender(s_floatBuf, DMA_BUF_LEN);
        float vol = s_volume;
        if (blockIsSilent(s_floatBuf, DMA_BUF_LEN)) {
            for (int i = 0; i < DMA_BUF_LEN; i++) s_dacBuf[i] = DAC_MID;
            s_shapeErr = 0.0f;  // start the next note from a settled shaper
        } else {
            for (int i = 0; i < DMA_BUF_LEN; i++) {
                float s = s_floatBuf[i] * vol;
                if (s > 1.0f) s = 1.0f;
                else if (s < -1.0f) s = -1.0f;
                s_dacBuf[i] = quantize(s);
            }
        }
        float used = (float)(micros() - t0) / blockUs;
        s_load = s_load * 0.9f + used * 0.1f;

        size_t written = 0;
        // blocks until the DMA has room: this is the sample clock
        i2s_write(I2S_NUM_0, s_dacBuf, sizeof(s_dacBuf), &written, portMAX_DELAY);
    }
}

void audioInit() {
    syrinxInit(AUDIO_SAMPLE_RATE);

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
    cfg.sample_rate = AUDIO_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = DMA_BUF_COUNT;
    cfg.dma_buf_len = DMA_BUF_LEN;
    cfg.use_apll = false;
    // Must stay false. With auto-clear a late refill hands the DAC 0x0000 —
    // which is not silence but the full negative rail, i.e. a hard click. Left
    // false the DMA repeats the previous buffer instead: far less audible, and
    // the only reason a missed block is a glitch rather than a bang.
    cfg.tx_desc_auto_clear = false;
    i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
    // Built-in DAC mode drives no external pins; passing NULL is what routes the
    // I2S output into the DAC instead of the GPIO matrix.
    i2s_set_pin(I2S_NUM_0, nullptr);

    // idle silent until the first chorus note: no hum on boot
    i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
    dac_output_disable(DAC_CHANNEL_1);

    xTaskCreatePinnedToCore(audioTask, "audioTask", 6144, nullptr,
                            AUDIO_TASK_PRIO, nullptr, AUDIO_TASK_CORE);
}

void audioSetRunning(bool on) {
    if (on == s_running) return;
    // GPIO25 is either an analog DAC output or a digital GPIO, never both. The
    // first version of this called pinMode(OUTPUT) and dac_output_enable() on the
    // same pin, which points the GPIO matrix and the RTC analog path at one pad
    // and leaves which one wins to the order of the calls. i2s_set_dac_mode()
    // configures the pad on its own; nothing else should touch it.
    if (on) {
        waitAudioIdle();
        i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);  // DAC1 = GPIO25
        s_shapeErr = 0.0f;
        primeMidLevel();  // settle at mid-level before the first real sample
        s_running = true;
    } else {
        s_running = false;
        waitAudioIdle();
        syrinxPanic();
        primeMidLevel();
        delay(30);  // let the settled level reach the amp
        i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
        dac_output_disable(DAC_CHANNEL_1);
    }
}

void audioSetVolume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    s_volume = v;
}

float audioGetVolume() { return s_volume; }

float audioGetLoad() { return s_load; }
