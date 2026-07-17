#include "audio_i2s.h"
#include "board_pins.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_i2s";
enum {
    CAPTURE_RESUME_DRAIN_SAMPLES = 256,
    CAPTURE_RESUME_DRAIN_ROUNDS = 6,
};

static i2s_chan_handle_t s_rx = NULL;
static i2s_chan_handle_t s_tx = NULL;
static bool s_capture_on = true;
static bool s_tx_enabled = false;

static void drain_rx_after_resume(void)
{
    if (!s_rx) {
        return;
    }

    int16_t scratch[CAPTURE_RESUME_DRAIN_SAMPLES];
    for (int i = 0; i < CAPTURE_RESUME_DRAIN_ROUNDS; i++) {
        size_t read = 0;
        esp_err_t err = i2s_channel_read(s_rx, scratch, sizeof(scratch), &read, 20);
        if (err == ESP_ERR_TIMEOUT) {
            break;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "drain rx after resume failed: %s", esp_err_to_name(err));
            break;
        }
    }
}

static esp_err_t init_rx(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_MIC_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "i2s mic new");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_MIC_BCLK,
            .ws = BOARD_I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = BOARD_I2S_MIC_DIN,
        },
    };
    std_cfg.slot_cfg.slot_mask = BOARD_I2S_MIC_SLOT_MASK;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "mic std init");
    return i2s_channel_enable(s_rx);
}

static esp_err_t init_tx(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_SPK_PORT, I2S_ROLE_MASTER);
    /* 关键：DMA 缺数据时必须自动清零输出，否则硬件会循环重放上一帧 PCM，
     * 在流式 TTS 句间出现"最后一个字连续叠音"的现象 */
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s spk new");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_SPK_BCLK,
            .ws = BOARD_I2S_SPK_WS,
            .dout = BOARD_I2S_SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "spk std init");
    esp_err_t err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) {
        s_tx_enabled = true;
    }
    return err;
}

esp_err_t audio_i2s_init(void)
{
    esp_err_t err = init_rx();
    if (err != ESP_OK) {
        return err;
    }
    err = init_tx();
    if (err != ESP_OK) {
        audio_i2s_deinit();
        return err;
    }
    ESP_LOGI(TAG, "I2S mic(I2S%d) + spk(I2S%d) OK", BOARD_I2S_MIC_PORT, BOARD_I2S_SPK_PORT);
    return ESP_OK;
}

void audio_i2s_deinit(void)
{
    if (s_rx) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    if (s_tx) {
        if (s_tx_enabled) {
            i2s_channel_disable(s_tx);
        }
        i2s_del_channel(s_tx);
        s_tx = NULL;
        s_tx_enabled = false;
    }
}

void audio_i2s_set_capture_enabled(bool enabled)
{
    if (!s_rx || s_capture_on == enabled) {
        return;
    }

    esp_err_t err = enabled ? i2s_channel_enable(s_rx) : i2s_channel_disable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s mic capture failed: %s", enabled ? "enable" : "disable", esp_err_to_name(err));
        return;
    }

    s_capture_on = enabled;
    if (enabled) {
        drain_rx_after_resume();
    }
}

esp_err_t audio_i2s_mic_read(int16_t *samples_out, size_t samples_count)
{
    if (!s_capture_on || !s_rx || !samples_out) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t bytes = samples_count * sizeof(int16_t);
    size_t read = 0;
    uint8_t *p = (uint8_t *)samples_out;
    while (read < bytes) {
        size_t n = 0;
        esp_err_t err = i2s_channel_read(s_rx, p + read, bytes - read, &n, portMAX_DELAY);
        if (err != ESP_OK) {
            return err;
        }
        read += n;
    }
    return ESP_OK;
}

esp_err_t audio_i2s_spk_write(const int16_t *samples, size_t samples_count)
{
    if (!s_tx || !samples) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_tx_enabled) {
        esp_err_t e = i2s_channel_enable(s_tx);
        if (e != ESP_OK) {
            return e;
        }
        s_tx_enabled = true;
    }
    size_t bytes = samples_count * sizeof(int16_t);
    size_t written = 0;
    const uint8_t *p = (const uint8_t *)samples;
    while (written < bytes) {
        size_t n = 0;
        esp_err_t err = i2s_channel_write(s_tx, p + written, bytes - written, &n, portMAX_DELAY);
        if (err != ESP_OK) {
            return err;
        }
        written += n;
    }
    return ESP_OK;
}

void audio_i2s_spk_stop(void)
{
    if (!s_tx || !s_tx_enabled) {
        return;
    }
    esp_err_t err = i2s_channel_disable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disable spk channel failed: %s", esp_err_to_name(err));
        return;
    }
    s_tx_enabled = false;
}
