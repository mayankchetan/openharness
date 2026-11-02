// What every pad the firmware uses does while the chip sleeps (battery sleep plan section 1.2). ONE table, so a new pin cannot
// be forgotten: test/test_sleep_pins.py fails when a BSP_ pin in pins_dial.h has no row here.
//
// WHY IT MATTERS. With tickless idle on, ESP-IDF isolates every GPIO in light sleep (input, output and pulls off:
// CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND selects PM_SLP_DISABLE_GPIO). An output the panel or the codec depends on would
// float: the AMOLED is powered directly and its chip select, reset and the audio amplifier enable must not. A pad marked KEEP
// gets gpio_sleep_sel_dis(), which keeps its awake configuration (level and drive) through sleep. power_sleep.c does that for
// every KEEP row once, at init.
//
// Light sleep actions
//   KEEP       an output that must hold its level: gpio_sleep_sel_dis().
//   ISOLATE    IDF's default: the pad is switched off for the sleep. Fine for inputs read only when awake and for pads whose
//              peripheral is stopped (I2S clocks, I2C bus: board pull-ups assumed, plan UNK).
//   WAKE_LOW   the wake source: gpio_wakeup_enable(LOW_LEVEL) by touch_habitat.c (it also does the KEEP part).
// Deep sleep actions (data only, for the later deep-sleep stage; nothing in the firmware reads these columns yet)
//   HOLD_HIGH / HOLD_LOW   latch the level across the sleep (rtc_gpio_hold_en for RTC pads, gpio_hold_en + gpio_deep_sleep_hold_en)
//   HOLD_IDLE              hold at the level the panel driver idles at; read the driver before using it
//   WAKE_LOW               EXT1 wake, any-low
//   ISOLATE                nothing to hold
#pragma once

#include "board_pins.h"

typedef enum { SLEEP_LIGHT_KEEP = 0, SLEEP_LIGHT_ISOLATE, SLEEP_LIGHT_WAKE_LOW } sleep_light_t;
typedef enum { SLEEP_DEEP_HOLD_HIGH = 0, SLEEP_DEEP_HOLD_LOW, SLEEP_DEEP_HOLD_IDLE, SLEEP_DEEP_WAKE_LOW, SLEEP_DEEP_ISOLATE } sleep_deep_t;
typedef enum { SLEEP_DIR_IN = 0, SLEEP_DIR_OUT, SLEEP_DIR_OD } sleep_dir_t;

// Pins that depend on the board (board()->lcd_rst, ->touch_rst): the row says which, power_sleep.c looks it up.
// The values below are the CST9217 dial's (board_table.c), the only dial the battery sleep runs on.
#define SLEEP_PIN_LCD_RST    (-1)
#define SLEEP_PIN_TOUCH_RST  (-2)
#define SLEEP_CST9217_LCD_RST   39
#define SLEEP_CST9217_TOUCH_RST 40

typedef struct {
    const char   *name;
    int           gpio;     // a BSP_ macro, or SLEEP_PIN_LCD_RST / SLEEP_PIN_TOUCH_RST
    sleep_dir_t   dir;
    sleep_light_t light;
    sleep_deep_t  deep;
} sleep_pin_t;

static const sleep_pin_t SLEEP_PINS[] = {
    // name                 gpio                 dir             light                    deep
    {"BOOT button",         BSP_BOOT_BUTTON,     SLEEP_DIR_IN,   SLEEP_LIGHT_ISOLATE,     SLEEP_DEEP_ISOLATE},    // read by the 250 ms poll; a wake pin only after a bench test (strapping pin)
    {"Touch INT",           BSP_TOUCH_INT,       SLEEP_DIR_IN,   SLEEP_LIGHT_WAKE_LOW,    SLEEP_DEEP_WAKE_LOW},
    {"LCD CS",              BSP_LCD_QSPI_CS,     SLEEP_DIR_OUT,  SLEEP_LIGHT_KEEP,        SLEEP_DEEP_HOLD_HIGH},
    {"LCD SCLK",            BSP_LCD_QSPI_SCLK,   SLEEP_DIR_OUT,  SLEEP_LIGHT_KEEP,        SLEEP_DEEP_HOLD_IDLE},
    {"LCD D0",              BSP_LCD_QSPI_D0,     SLEEP_DIR_OUT,  SLEEP_LIGHT_KEEP,        SLEEP_DEEP_HOLD_IDLE},
    {"LCD D1",              BSP_LCD_QSPI_D1,     SLEEP_DIR_OUT,  SLEEP_LIGHT_KEEP,        SLEEP_DEEP_HOLD_IDLE},
    {"LCD D2",              BSP_LCD_QSPI_D2,     SLEEP_DIR_OUT,  SLEEP_LIGHT_KEEP,        SLEEP_DEEP_HOLD_IDLE},
    {"LCD D3",              BSP_LCD_QSPI_D3,     SLEEP_DIR_OUT,  SLEEP_LIGHT_KEEP,        SLEEP_DEEP_HOLD_IDLE},
    {"LCD reset",           SLEEP_PIN_LCD_RST,   SLEEP_DIR_OUT,  SLEEP_LIGHT_KEEP,        SLEEP_DEEP_HOLD_HIGH},
    {"Touch reset",         SLEEP_PIN_TOUCH_RST, SLEEP_DIR_OUT,  SLEEP_LIGHT_KEEP,        SLEEP_DEEP_HOLD_HIGH},
    {"Speaker amp enable",  BSP_PA_IO,           SLEEP_DIR_OUT,  SLEEP_LIGHT_KEEP,        SLEEP_DEEP_HOLD_LOW},    // low in S2: the codec library drops it after every beep, and audio_capture_idle(true) drives it low (S2_AMP_OFF); KEEP holds that level through the sleep
    {"I2S MCLK",            BSP_I2S_MCLK,        SLEEP_DIR_OUT,  SLEEP_LIGHT_ISOLATE,     SLEEP_DEEP_ISOLATE},    // clocks stopped while the chip sleeps (audio_capture_idle)
    {"I2S BCLK",            BSP_I2S_BCLK,        SLEEP_DIR_OUT,  SLEEP_LIGHT_ISOLATE,     SLEEP_DEEP_ISOLATE},
    {"I2S WS",              BSP_I2S_WS,          SLEEP_DIR_OUT,  SLEEP_LIGHT_ISOLATE,     SLEEP_DEEP_ISOLATE},
    {"I2S data to codec",   BSP_I2S_DIN,         SLEEP_DIR_OUT,  SLEEP_LIGHT_ISOLATE,     SLEEP_DEEP_ISOLATE},
    {"I2S data from codec", BSP_I2S_DOUT,        SLEEP_DIR_IN,   SLEEP_LIGHT_ISOLATE,     SLEEP_DEEP_ISOLATE},
    {"I2C SDA",             BSP_I2C_SDA,         SLEEP_DIR_OD,   SLEEP_LIGHT_ISOLATE,     SLEEP_DEEP_ISOLATE},    // board pull-ups keep the bus high
    {"I2C SCL",             BSP_I2C_SCL,         SLEEP_DIR_OD,   SLEEP_LIGHT_ISOLATE,     SLEEP_DEEP_ISOLATE},
};
#define SLEEP_PIN_COUNT ((int)(sizeof SLEEP_PINS / sizeof SLEEP_PINS[0]))
