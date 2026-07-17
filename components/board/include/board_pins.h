/**
 * @file board_pins.h
 * @brief ESP32-S3 与 INMP441 / MAX98357 / ST7789 默认接线（可按硬件修改）
 */
#pragma once

#include "driver/gpio.h"
#include "hal/i2s_types.h"

/* I2S0 — 麦克风 INMP441（L/R 接 GND → 芯片输出左声道，一般配 I2S_STD_SLOT_LEFT） */
#define BOARD_I2S_MIC_PORT       I2S_NUM_0
#define BOARD_I2S_MIC_BCLK       GPIO_NUM_5
#define BOARD_I2S_MIC_WS         GPIO_NUM_4
#define BOARD_I2S_MIC_DIN        GPIO_NUM_6
/* 听写返回 w 仍为空时，可改为 I2S_STD_SLOT_RIGHT（WS 接法/_module 差异） */
#define BOARD_I2S_MIC_SLOT_MASK  I2S_STD_SLOT_LEFT
/* 上传 ASR 前 PCM 软件放大：分子/分母。
 * 1/1 太弱（RMS 经常 <1800 → VAD 判定全静音）；3/2 太强（近距离削顶）。
 * 5/4 = 1.25x 是折中：正常对话 peak 约 5000~20000，不会削顶也够讯飞识别。 */
#define BOARD_MIC_PCM_GAIN_NUM   5
#define BOARD_MIC_PCM_GAIN_DEN   4

/* I2S1 — MAX98357 功放 */
#define BOARD_I2S_SPK_PORT       I2S_NUM_1
#define BOARD_I2S_SPK_BCLK       GPIO_NUM_15
#define BOARD_I2S_SPK_WS         GPIO_NUM_16
#define BOARD_I2S_SPK_DOUT       GPIO_NUM_7

/* SPI — ST7789 240x240 */
#define BOARD_LCD_SPI_HOST       SPI2_HOST
#define BOARD_LCD_PIN_MOSI       GPIO_NUM_11
#define BOARD_LCD_PIN_SCLK       GPIO_NUM_12
#define BOARD_LCD_PIN_CS         GPIO_NUM_10
#define BOARD_LCD_PIN_DC         GPIO_NUM_9
#define BOARD_LCD_PIN_RST        GPIO_NUM_8
#define BOARD_LCD_PIN_BL         GPIO_NUM_13

#define BOARD_AUDIO_SAMPLE_RATE  16000
#define BOARD_AUDIO_BITS         16
