#include "audio.h"
#include "syrinx.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <driver/dac.h>

#define AUDIO_DAC_PIN 25        // DAC1 -> NS4168 speaker amp
#define DMA_BUF_COUNT 4
#define DMA_BUF_LEN 256         // frames per DMA buffer (~11.6 ms at 22.05 kHz)

static volatile bool s_running = false;
static volatile float s_volume = 0.7f;

static float s_floatBuf[DMA_BUF_LEN];
static uint16_t s_dacBuf[DMA_BUF_LEN];

// The DAC takes the MSB of each 16-bit slot, unsigned — so 0x8000 is the
// mid-level. i2s_zero_dma_buffer writes 0x0000, which is full-negative and
// would pop the amp; always settle to mid-level instead.
static void primeMidLevel() {
    for (int i = 0; i < DMA_BUF_LEN; i++) s_dacBuf[i] = 0x8000;
    for (int k = 0; k < DMA_BUF_COUNT; k++) {
        size_t written = 0;
        i2s_write(I2S_NUM_0, s_dacBuf, sizeof(s_dacBuf), &written, pdMS_TO_TICKS(100));
    }
}

static void audioTask(void*) {
    while (true) {
        if (!s_running) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        syrinxRender(s_floatBuf, DMA_BUF_LEN);
        float vol = s_volume;
        for (int i = 0; i < DMA_BUF_LEN; i++) {
            float s = s_floatBuf[i] * vol;
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            // built-in DAC reads the high byte of each 16-bit slot, unsigned
            s_dacBuf[i] = (uint16_t)(((int)(s * 127.0f) + 128) << 8);
        }
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
    cfg.intr_alloc_flags = 0;
    cfg.dma_buf_count = DMA_BUF_COUNT;
    cfg.dma_buf_len = DMA_BUF_LEN;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
    i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);  // DAC1 = GPIO25

    // idle silent until the first chorus note: no hum on boot
    i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
    dac_output_disable(DAC_CHANNEL_1);
    pinMode(AUDIO_DAC_PIN, INPUT);

    xTaskCreatePinnedToCore(audioTask, "audioTask", 4096, nullptr, 1, nullptr, 1);
}

void audioSetRunning(bool on) {
    if (on == s_running) return;
    s_running = on;
    if (on) {
        pinMode(AUDIO_DAC_PIN, OUTPUT);
        dac_output_enable(DAC_CHANNEL_1);
        i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);
        primeMidLevel();  // settle at mid-level before the first real sample
    } else {
        syrinxPanic();
        primeMidLevel();
        delay(30);  // let the settled level reach the amp
        i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
        dac_output_disable(DAC_CHANNEL_1);
        pinMode(AUDIO_DAC_PIN, INPUT);  // high-Z: no idle hiss
    }
}

void audioSetVolume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    s_volume = v;
}

float audioGetVolume() { return s_volume; }
