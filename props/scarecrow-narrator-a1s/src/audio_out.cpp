#include "audio_out.h"
#include "pins_a1s.h"
#include <Arduino.h>
#include <math.h>
#include <driver/i2s.h>

static const i2s_port_t PORT = I2S_NUM_0;
static uint32_t s_rate   = 0;
static bool     s_running = false;

// scratch for mono->stereo and tone synth
static int16_t s_stereo[512];   // 256 frames

bool audio_out_begin(uint32_t sample_rate) {
    if (s_running) audio_out_end();

    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = sample_rate;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;      // stereo
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = 256;
    cfg.use_apll             = true;                            // accurate audio clock
    cfg.tx_desc_auto_clear   = true;                            // emit silence on underrun
    cfg.fixed_mclk           = 0;
    cfg.mclk_multiple        = I2S_MCLK_MULTIPLE_256;           // matches ES8388 reg 0x18 = 0x02
    cfg.bits_per_chan        = I2S_BITS_PER_CHAN_DEFAULT;

    if (i2s_driver_install(PORT, &cfg, 0, NULL) != ESP_OK) {
        Serial.println(F("[audio_out] i2s_driver_install failed"));
        return false;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = A1S_I2S_MCLK;    // GPIO0
    pins.bck_io_num   = A1S_I2S_BCLK;    // GPIO27
    pins.ws_io_num    = A1S_I2S_LRCK;    // GPIO25
    pins.data_out_num = A1S_I2S_DOUT;    // GPIO26
    pins.data_in_num  = I2S_PIN_NO_CHANGE;

    if (i2s_set_pin(PORT, &pins) != ESP_OK) {
        Serial.println(F("[audio_out] i2s_set_pin failed"));
        i2s_driver_uninstall(PORT);
        return false;
    }

    i2s_zero_dma_buffer(PORT);
    s_rate    = sample_rate;
    s_running = true;
    Serial.printf("[audio_out] I2S up: %lu Hz, 16-bit stereo, MCLK=GPIO%d BCLK=GPIO%d LRCK=GPIO%d DOUT=GPIO%d\n",
                  (unsigned long)sample_rate, A1S_I2S_MCLK, A1S_I2S_BCLK, A1S_I2S_LRCK, A1S_I2S_DOUT);
    return true;
}

void audio_out_end() {
    if (!s_running) return;
    i2s_zero_dma_buffer(PORT);
    i2s_driver_uninstall(PORT);
    s_running = false;
    s_rate    = 0;
}

void audio_out_set_rate(uint32_t sample_rate) {
    if (!s_running || sample_rate == s_rate) return;
    if (i2s_set_clk(PORT, sample_rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO) == ESP_OK) {
        s_rate = sample_rate;
        Serial.printf("[audio_out] rate -> %lu Hz\n", (unsigned long)sample_rate);
    } else {
        Serial.printf("[audio_out] rate change to %lu Hz FAILED\n", (unsigned long)sample_rate);
    }
}

uint32_t audio_out_rate() { return s_rate; }

size_t audio_out_write(const int16_t* interleaved_stereo, size_t frames) {
    if (!s_running || !frames) return 0;
    size_t written = 0;
    i2s_write(PORT, interleaved_stereo, frames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    return written / (2 * sizeof(int16_t));
}

size_t audio_out_write_mono(const int16_t* mono, size_t samples) {
    if (!s_running || !samples) return 0;
    size_t done = 0;
    const size_t CHUNK = 256;   // frames per pass (s_stereo holds 256)
    while (done < samples) {
        size_t n = samples - done;
        if (n > CHUNK) n = CHUNK;
        for (size_t i = 0; i < n; i++) {
            int16_t v = mono[done + i];
            s_stereo[2 * i]     = v;
            s_stereo[2 * i + 1] = v;
        }
        size_t w = 0;
        i2s_write(PORT, s_stereo, n * 2 * sizeof(int16_t), &w, portMAX_DELAY);
        done += n;
    }
    return done;
}

void audio_out_silence_ms(uint32_t ms) {
    if (!s_running) return;
    memset(s_stereo, 0, sizeof(s_stereo));
    uint32_t total_frames = (uint64_t)s_rate * ms / 1000;
    uint32_t done = 0;
    while (done < total_frames) {
        uint32_t n = total_frames - done;
        if (n > 256) n = 256;
        size_t w = 0;
        i2s_write(PORT, s_stereo, n * 2 * sizeof(int16_t), &w, portMAX_DELAY);
        done += n;
    }
}

void audio_out_flush() {
    if (s_running) i2s_zero_dma_buffer(PORT);
}

void audio_out_tone(uint32_t freq_hz, uint32_t ms, float amplitude) {
    if (!s_running) return;
    if (amplitude < 0) amplitude = 0;
    if (amplitude > 1) amplitude = 1;
    const int16_t peak = (int16_t)(amplitude * 30000.0f);
    const float w = 2.0f * (float)M_PI * (float)freq_hz / (float)s_rate;
    uint32_t total_frames = (uint64_t)s_rate * ms / 1000;
    uint32_t done = 0;
    float phase = 0;
    while (done < total_frames) {
        uint32_t n = total_frames - done;
        if (n > 256) n = 256;
        for (uint32_t i = 0; i < n; i++) {
            int16_t s = (int16_t)(peak * sinf(phase));
            phase += w;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            s_stereo[2 * i]     = s;
            s_stereo[2 * i + 1] = s;
        }
        size_t written = 0;
        i2s_write(PORT, s_stereo, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        done += n;
    }
}
