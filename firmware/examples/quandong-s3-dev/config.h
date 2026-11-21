#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

/* ==========================================================================
 * LCDWIKI 2.8inch ESP32-S3 Display  E32C28P(带触屏) / E32N28P(无触屏)
 * 商家在固件里把板型名写成 "quandong-s3-dev"，实际型号为 ES3C28P。
 * 引脚来源：www.lcdwiki.com 官方 IO 资源分配表 + 硬件原理图。
 *
 * 硬件：
 *   ESP32-S3 N16R8（16MB Flash / 8MB Octal PSRAM）
 *   2.8" IPS 240x320  ILI9341V (SPI)  +  FT6336U 电容触摸 (I2C 0x38)
 *   ES8311 音频编解码 (I2C 0x18) + FM8002E 功放 + LMA2718B381 MEMS 麦克风
 *   WS2812 RGB 灯 (IO42) / MicroSD (SDIO) / TP4054 充电 / ME6217 稳压
 * ========================================================================== */

/* ---------------- 音频 ---------------- */
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

/* 单麦 + ES8311 自带 ADC，没有独立回采通道，不能开硬件 AEC */
#define AUDIO_INPUT_REFERENCE    false

/* I2S 方向按 ES8311 网络名接线，不要照 wiki 表格字面理解：
 *   IO6 = ES8311 ASDOUT (net I2S_DO) → 麦克风数据 → ESP32 的 DIN
 *   IO8 = ES8311 DSDIN  (net I2S_DI) → 喇叭数据   → ESP32 的 DOUT
 * wiki 表里写的 output/input 是站在 codec 视角，照抄会把收发放反，
 * 表现为喇叭无声 + 麦克风全是 0。 */
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_4   /* IO4  I2S 主时钟 */
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_5   /* IO5  I2S 位时钟 */
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_7   /* IO7  I2S 左右声道选择 */
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_8   /* IO8  ESP → ES8311 DSDIN（喇叭） */
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6   /* IO6  ES8311 ASDOUT → ESP（麦克风） */

/*
 * 注意：IO1 是功放使能，官方资料写明「低电平使能，高电平关闭」，
 * 与常见的「高电平开功放」相反，所以 pa_inverted 必须为 true，
 * 否则喇叭会一直静音，或者一直在响。
 */
#define AUDIO_CODEC_PA_PIN          GPIO_NUM_1
#define AUDIO_CODEC_PA_INVERTED     true

/* I2C 总线：ES8311(0x18)、FT6336U(0x38) 与外部 IIC 扩展座共用 IO16/IO15 */
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_16
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_15
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR

/* 触摸屏（预留，当前固件未启用触摸交互） */
#define TOUCH_I2C_ADDR      0x38
#define TOUCH_RST_PIN       GPIO_NUM_18
#define TOUCH_INT_PIN       GPIO_NUM_17

/* ---------------- 显示 ---------------- */
/* ILI9341 SPI：LCDWIKI 默认竖屏 240x320 */
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320
/* 这块屏的 FPC 走线与 ILI9341 出厂扫描方向相反，MX 位必须置 1，
 * 否则画面左右镜像（文字反着写、进度条从右往左跑）。 */
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#define DISPLAY_SPI_SCK_PIN   GPIO_NUM_12  /* IO12 SPI 时钟 */
#define DISPLAY_SPI_MOSI_PIN  GPIO_NUM_11  /* IO11 SPI 写数据 */
#define DISPLAY_SPI_MISO_PIN  GPIO_NUM_13  /* IO13 SPI 读数据 */
#define DISPLAY_SPI_CS_PIN    GPIO_NUM_10  /* IO10 片选，低有效 */
#define DISPLAY_DC_PIN        GPIO_NUM_46  /* IO46 命令/数据选择，高=数据 */
/* 屏的 RESET 与 ESP32-S3 的 CHIP_PU 共用一个按键，不是独立 GPIO */
#define DISPLAY_RST_PIN       GPIO_NUM_NC

#define DISPLAY_BACKLIGHT_PIN           GPIO_NUM_45  /* IO45 背光，高电平点亮 */
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

/* 屏本身的颜色反相 / BGR 排列（LCDWIKI 出厂初始化就是这样设的）
 * 如果烧完发现颜色是负片，把 INVERT 改成 false；
 * 如果红蓝互换，把 BGR 改成 false。 */
#define DISPLAY_INVERT_COLOR  true
#define DISPLAY_BGR_ORDER     true

/* ---------------- 按键与指示灯 ---------------- */
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

/* IO42 是带控制 IC 的 WS2812 三色灯，不能用普通 GPIO 电平驱动，
 * 固件暂不接管，避免上电乱闪。 */
#define BUILTIN_LED_GPIO        GPIO_NUM_NC
#define RGB_LED_GPIO            GPIO_NUM_42

#endif // _BOARD_CONFIG_H_
