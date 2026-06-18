#ifndef LED_H
#define LED_H

#include "stm32l4xx_hal.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Status LED — user feedback for device state, signal quality and faults.
 *
 * Hardware today: one single-colour LED on PB10 (active-high). The board will
 * later carry an RGB LED; this module already names a colour per state so the
 * scheme carries over. To move to RGB: wire the three channels, set
 * LED_HW_RGB to 1 and fill in led_write() + the channel pin macros below.
 *
 * The pattern engine is driven from SysTick (LED_Tick(), every 1 ms) so the
 * indication keeps animating even while the main loop blocks on an SD write.
 *
 * Single LED today: only the rhythm distinguishes the states. With RGB the
 * colour carries the meaning and the rhythm reinforces it.
 *
 *   state            rhythm                       (colour)  meaning
 *   ---------------  ---------------------------  --------  -----------------------
 *   BOOT             solid                        blue      powering up / busy
 *   READY            4 Hz blink (~1 s)            cyan      init OK, acquisition armed
 *   RECORDING        50 ms wink every 1 s         green     running, signal good
 *   LEADOFF          2 Hz even blink              yellow    electrode off / bad signal
 *   WARN             double-blink every 1.5 s     amber     samples dropped (logged)
 *   FAULT_STORAGE    triple-blink burst           red       SD / write failure (fatal)
 *   FAULT_INIT       ~6 Hz frantic blink          red       no card / mount failed (fatal)
 * ------------------------------------------------------------------------- */

#define LED_HW_RGB        0
#define LED_GPIO_PORT     GPIOB
#define LED_GPIO_PIN      GPIO_PIN_10

typedef enum {
    LED_BOOT = 0,        /* powering up: init peripherals / mount SD / open session */
    LED_READY,           /* init OK, acquisition armed (brief) */
    LED_RECORDING,       /* acquiring, electrodes good — calm heartbeat */
    LED_LEADOFF,         /* acquiring, electrode(s) off / bad signal */
    LED_WARN,            /* acquiring, recoverable issue logged (e.g. dropped samples) */
    LED_FAULT_STORAGE,   /* unrecoverable SD / recording error (fatal) */
    LED_FAULT_INIT,      /* init failed: no card, mount or session-open failure (fatal) */
    LED_STATE_COUNT
} LED_State;

/* Take ownership of the LED. Call once after MX_GPIO_Init(). */
void LED_Init(void);

/* Request a state. Ignored when it equals the current state (so the blink
   phase is preserved) and while a fatal state is latched — only another fatal
   state can replace it. Safe to call from the main loop at any rate. */
void LED_SetState(LED_State s);

/* Advance the pattern by one tick and drive the LED. Call every 1 ms from
   SysTick_Handler(). */
void LED_Tick(void);

#endif /* LED_H */
