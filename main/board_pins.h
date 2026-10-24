#pragma once

/* ================= board pin map (single source of truth) =================
 *
 * Change a pin here, then rebuild + reflash. One place only - no menuconfig
 * and no sdkconfig sync, so regenerating the sdkconfig can never silently
 * move a pin back to an old value.
 *
 * ESP32-WROOM restrictions:
 *   - gpio 1 / 3      : uart0 (the serial console) - do not use
 *   - gpio 6-11       : spi flash - do not use
 *   - gpio 34-39      : input only
 *   - gpio 0/2/5/12/15: strapping pins (their level at reset selects the
 *                       boot mode - avoid them for outputs)
 *   - gpio 25/26      : internal dac pads (fine as plain digital outputs as
 *                       long as the dac driver is not used)
 *   - gpio 27         : adc2 (the project does not use the adc)
 */

/* ---- MAX98357A I2S (std mode, philips format, 16 bit) ----
 *
 * The map below is the one the hardware is CURRENTLY wired for and the one
 * that is VERIFIED TO PRODUCE SOUND. It is the boot default:
 *
 *     BCLK = GPIO27, LRC/WS = GPIO25, DIN = GPIO33
 *
 * The alternative map (the original 18/19/22 wiring) can be selected at
 * runtime with the "pins" command and is then remembered in nvs, so no
 * future reflash can silence a working amplifier again. */
#define PIN_I2S_BCK    27   /* -> module BCLK (bit clock)   */
#define PIN_I2S_WS     25   /* -> module LRC / WS (word sel) */
#define PIN_I2S_DATA   33   /* -> module DIN (data)         */

/* Alternative map - the original wiring, use "pins 1" to select it. */
#define PIN_I2S_BCK_ALT  18
#define PIN_I2S_WS_ALT   19
#define PIN_I2S_DATA_ALT 22

/* ---- status led (on while streaming, blinking while reconnecting) ---- */
#define PIN_STATUS_LED 26

/* ---- optional: drive the amplifier SD pin to test the module ----
 * -1 disables the feature. Wiring: gpio -> 22R -> SD pin of the module.
 *   sd 0 = pull low  (shutdown, the outputs must fall to ~0V)
 *   sd 1 = pull high (left channel only, 15 dB)
 *   sd z = float     (module default: (L+R)/2 mix, 9 dB) */
#define PIN_AMP_SD     23

/* ---- direct drive modes (diagnostic only, mutually exclusive with i2s) ---- */
#define PIN_PWM        32   /* pwm direct drive output       */
#define PIN_DAC_CHANNEL 0   /* 0 = gpio25, 1 = gpio26 (conflicts with the led) */
