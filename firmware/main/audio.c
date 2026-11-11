// SPDX-License-Identifier: MIT
// ES8311 codec: microphone voice detection and the chirp synthesizer.
//
// One task reads the microphone in 20 ms chunks and, while a chirp is
// playing, writes the same amount to the speaker, which keeps both in step.
// The microphone is ignored while Muse is making a sound so it never reacts
// to itself.

#include "audio.h"

#include <math.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "agent.h"
#include "muse.h"

static const char *TAG = "audio";

#define SAMPLE_RATE 16000
#define CHUNK 320              // 20 ms
#define SELF_MUTE_MS 350       // ignore the mic this long after the last chirp sample
#define VOICE_MIN_RMS 120.0f
#define VOICE_MIN_CHUNKS 5     // 100 ms of sound, so taps and clicks don't count
#define VOICE_HANGOVER_MS 700
#define LEARN_CHUNKS 25        // learn the room noise for 0.5 s after start-up
#define PREROLL_CHUNKS 20      // 400 ms of audio from before the talk gesture was recognised

#define SPEAKER_VOLUME CONFIG_MUSE_SPEAKER_VOLUME
#define MIC_GAIN_DB ((float)CONFIG_MUSE_MIC_GAIN_DB)

#ifdef CONFIG_MUSE_SOUND_ENABLE
#define SOUND_ENABLED 1
#else
#define SOUND_ENABLED 0
#endif

#ifdef CONFIG_MUSE_VOICE_ENABLE
#define VOICE_ENABLED 1
#define VOICE_RATIO (CONFIG_MUSE_VOICE_RATIO_X10 / 10.0f)
#else
#define VOICE_ENABLED 0
#define VOICE_RATIO 1000.0f
#endif

static esp_codec_dev_handle_t s_codec;
static QueueHandle_t s_queue;
static volatile int s_volume = SPEAKER_VOLUME;  // what was asked for; the audio task applies it

// ---------------------------------------------------------------- synth
static const muse_note_t *s_seq;
static int s_note, s_note_pos;
static float s_phase;

static void synth_fill(int16_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        float v = 0;
        if (s_seq) {
            const muse_note_t *nt = &s_seq[s_note];
            if (nt->ms == 0) {
                s_seq = NULL;
            } else {
                const int total = nt->ms * (SAMPLE_RATE / 1000);
                if (nt->amp) {
                    const float t = (float)s_note_pos / total;
                    float f = nt->f0 + (nt->f1 - nt->f0) * t * t * (3 - 2 * t);
                    if (nt->vib_hz) {
                        f += nt->vib_depth * sinf(2 * (float)M_PI * nt->vib_hz * s_note_pos / SAMPLE_RATE);
                    }
                    s_phase += 2 * (float)M_PI * f / SAMPLE_RATE;
                    if (s_phase > 2 * (float)M_PI) {
                        s_phase -= 2 * (float)M_PI;
                    }
                    // 6 ms attack, 30 ms release, a little 2nd and 3rd harmonic for warmth
                    const float env = fminf(1.0f, s_note_pos / 96.0f) * fminf(1.0f, (total - s_note_pos) / 480.0f);
                    v = (sinf(s_phase) + 0.28f * sinf(2 * s_phase) + 0.12f * sinf(3 * s_phase)) * env *
                        (nt->amp / 100.0f) * 9000.0f;
                }
                if (++s_note_pos >= total) {
                    s_note++;
                    s_note_pos = 0;
                }
            }
        }
        out[i] = (int16_t)v;
    }
}

// ---------------------------------------------------------------- codec
static bool codec_open(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        return false;
    }
    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &tx, &rx) != ESP_OK) {
        return false;
    }
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK || i2s_channel_init_std_mode(rx, &std_cfg) != ESP_OK) {
        return false;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {.port = CONFIG_BSP_I2S_NUM, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c_cfg = {.port = BSP_I2C_NUM, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!data_if || !gpio_if || !ctrl_if) {
        return false;
    }
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) {
        return false;
    }
    esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = codec_if, .data_if = data_if};
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (!s_codec) {
        return false;
    }
    esp_codec_dev_sample_info_t fs = {.sample_rate = SAMPLE_RATE, .channel = 1, .bits_per_sample = 16};
    if (esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) {
        return false;
    }
    esp_codec_dev_set_in_gain(s_codec, MIC_GAIN_DB);
    esp_codec_dev_set_out_mute(s_codec, false);
    esp_codec_dev_set_out_vol(s_codec, SPEAKER_VOLUME);
    return true;
}

// ---------------------------------------------------------------- task
static float chunk_rms(const int16_t *s, int n)
{
    float mean = 0;
    for (int i = 0; i < n; i++) {
        mean += s[i];
    }
    mean /= n;
    float acc = 0;
    for (int i = 0; i < n; i++) {
        const float d = s[i] - mean;
        acc += d * d;
    }
    return sqrtf(acc / n);
}

// Loudness of Muse's own reply, 0..1, for the mouth.
static float speech_level(const int16_t *pcm, int n)
{
    const float rms = chunk_rms(pcm, n);
    if (rms < 1.0f) {
        return 0.0f;
    }
    const float dbfs = 20.0f * log10f(rms / 32768.0f);
    return muse_clampf((dbfs + 42.0f) / 26.0f, 0.0f, 1.0f);
}

static void audio_task(void *arg)
{
    (void)arg;
    static int16_t mic[CHUNK], spk[CHUNK];
    int16_t (*preroll)[CHUNK] = heap_caps_calloc(PREROLL_CHUNKS, sizeof(*preroll), MALLOC_CAP_SPIRAM);
    int preroll_next = 0;
    bool was_listening = false;
    float noise = 0, env = 0, mic_env = 0, room_db = 0;
    int voiced_run = 0, chunks = 0;
    int64_t last_voice = 0, mute_until = 0;
#ifdef CONFIG_MUSE_DEBUG_LOG
    int64_t next_log = muse_now_ms() + 3000;
#endif
    int volume = SPEAKER_VOLUME;
    for (;;) {
        if (volume != s_volume) {
            volume = s_volume;
            esp_codec_dev_set_out_vol(s_codec, volume);
        }
        if (esp_codec_dev_read(s_codec, mic, sizeof(mic)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const int64_t now = muse_now_ms();

        // How noisy the room is (for the AI's muse_status), when Muse itself is quiet.
        if (now >= mute_until) {
            const float db = 20.0f * log10f(chunk_rms(mic, CHUNK) + 1.0f);
            room_db = room_db <= 0 ? db : room_db + (db - room_db) * 0.02f;
            g_muse.noise_db = (int)room_db;
        }

        // Voice chat: while the screen is held, stream the microphone,
        // starting with the moments before the hold was recognised.
        const bool listening = agent_listening();
        if (listening) {
            if (!was_listening && preroll) {
                for (int i = 0; i < PREROLL_CHUNKS; i++) {
                    agent_push_mic(preroll[(preroll_next + i) % PREROLL_CHUNKS], CHUNK);
                }
            }
            agent_push_mic(mic, CHUNK);
            // How loud you are, so Muse can lean in as you speak.
            const float loud = muse_clampf((20.0f * log10f(chunk_rms(mic, CHUNK) + 1.0f) - 52.0f) / 26.0f, 0.0f, 1.0f);
            mic_env += (loud - mic_env) * (loud > mic_env ? 0.4f : 0.08f);
        } else {
            mic_env *= 0.8f;
            if (preroll) {
                memcpy(preroll[preroll_next], mic, sizeof(mic));
                preroll_next = (preroll_next + 1) % PREROLL_CHUNKS;
            }
        }
        g_muse.mic_level = mic_env;
        was_listening = listening;

        // Speaker: Muse's reply first, chirps otherwise.
        const int got = agent_pull_speech(spk, CHUNK);
        if (got > 0) {
            if (got < CHUNK) {
                memset(spk + got, 0, (CHUNK - got) * sizeof(int16_t));
            }
            esp_codec_dev_write(s_codec, spk, sizeof(spk));
            mute_until = now + SELF_MUTE_MS;
            const float level = speech_level(spk, CHUNK);
            env = level > env ? env + (level - env) * 0.7f : env * 0.6f + level * 0.4f;
            g_muse.mouth_level = env;
            continue;
        }
        int id;
        if (!s_seq && xQueueReceive(s_queue, &id, 0) == pdTRUE) {
            s_seq = muse_sounds[id];
            s_note = s_note_pos = 0;
        }
        if (s_seq) {
            synth_fill(spk, CHUNK);
            esp_codec_dev_write(s_codec, spk, sizeof(spk));
            mute_until = now + SELF_MUTE_MS;
        }

        // Without voice chat, Muse just reacts to voices around it.
        if (!VOICE_ENABLED || g_muse.agent_state != MUSE_AGENT_OFF) {
            env *= 0.6f;
            g_muse.mouth_level = env;
            g_muse.voice_active = false;
            continue;
        }
        const float rms = chunk_rms(mic, CHUNK);
        chunks++;
        if (now < mute_until) {
            voiced_run = 0;
            env *= 0.7f;
            g_muse.mouth_level = env;
            g_muse.voice_active = false;
            continue;
        }

        // Track the room noise: quickly while it's quiet, very slowly while
        // someone is talking.
        if (chunks < LEARN_CHUNKS || noise <= 0) {
            noise = noise <= 0 ? rms : noise + (rms - noise) * 0.2f;
        }
        const bool voiced = chunks >= LEARN_CHUNKS && rms > noise * VOICE_RATIO && rms > VOICE_MIN_RMS;
        noise += (rms - noise) * (voiced ? 0.0005f : 0.05f);
        noise = fmaxf(noise, 15.0f);

        voiced_run = voiced ? voiced_run + 1 : 0;
        if (voiced_run >= VOICE_MIN_CHUNKS) {
            last_voice = now;
        }
        g_muse.voice_active = last_voice != 0 && (now - last_voice) < VOICE_HANGOVER_MS;

        float level = 0;
        if (voiced && g_muse.voice_active) {
            level = muse_clampf((20.0f * log10f(rms / noise) - 8.0f) / 18.0f, 0.0f, 1.0f);
        }
        env = level > env ? env + (level - env) * 0.6f : env * 0.75f;
        g_muse.mouth_level = env;

#ifdef CONFIG_MUSE_DEBUG_LOG
        if (now >= next_log) {
            ESP_LOGI(TAG, "mic rms %.0f, room noise %.0f, voice %d", rms, noise, g_muse.voice_active);
            next_log = now + 5000;
        }
#endif
    }
}

bool audio_start(void)
{
    if (!codec_open()) {
        ESP_LOGW(TAG, "ES8311 codec not available, sound and voice are off");
        return false;
    }
    s_queue = xQueueCreate(4, sizeof(int));
    xTaskCreatePinnedToCore(audio_task, "muse_audio", 6 * 1024, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "ES8311 ready at %d Hz (sound %s, voice %s)", SAMPLE_RATE, SOUND_ENABLED ? "on" : "off",
             VOICE_ENABLED ? "on" : "off");
    return true;
}

void audio_set_volume(int percent)
{
    s_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
}

int audio_volume(void)
{
    return s_volume;
}

void audio_play(muse_sound_t sound)
{
    if (SOUND_ENABLED && s_queue && sound < MUSE_SOUND_COUNT) {
        const int id = sound;
        xQueueSend(s_queue, &id, 0);
    }
}
