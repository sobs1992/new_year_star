#pragma once

#include "driver/gpio.h"

/* What is wired to which pin of the Seeed XIAO ESP32-C5 (Dn: the pin names on its edge).
 * The web page names D2/D3 for the radar too (index.html). */

/* MAX98357 I2S amplifier; its SD pin floats, so it plays (L+R)/2 */
#define BOARD_I2S_BCLK      GPIO_NUM_9   /* D9 */
#define BOARD_I2S_LRC       GPIO_NUM_10  /* D10 */
#define BOARD_I2S_DIN       GPIO_NUM_8   /* D8 */

/* WS2812 strip data */
#define BOARD_LED_STRIP     GPIO_NUM_12  /* D7, free with the console on USB only (sdkconfig.defaults) */

/* Seeed 24GHz mmWave for XIAO radar, UART */
#define BOARD_RADAR_RX      GPIO_NUM_25  /* D2 <- radar TX */
#define BOARD_RADAR_TX      GPIO_NUM_7   /* D3 -> radar RX */

/* On the XIAO itself */
#define BOARD_STATUS_LED    GPIO_NUM_27  /* yellow user LED, on when low */
#define BOARD_BOOT_BUTTON   GPIO_NUM_28  /* BOOT, to GND when pressed */
