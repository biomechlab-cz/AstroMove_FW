/* =====================================================================
 * Status LED driver — see led.h for the state → indication scheme.
 *
 * Each state maps to a rhythm "pulses of on_ms separated by gap_ms, then
 * off for the rest of period_ms". A single 1 ms SysTick advances the phase
 * and drives the pin, so the rhythm survives blocking SD writes in the
 * main loop. A colour is carried per state for the future RGB LED.
 * ===================================================================== */
#include "led.h"

typedef struct {
    uint16_t period_ms;   /* full pattern period */
    uint16_t on_ms;       /* duration of each pulse */
    uint16_t gap_ms;      /* off-time between pulses within a burst */
    uint8_t  pulses;      /* number of pulses per period */
    uint8_t  r, g, b;     /* colour for the future RGB LED (per channel on/off) */
} led_pattern_t;

static const led_pattern_t PATTERNS[LED_STATE_COUNT] = {
    [LED_BOOT]          = {  500, 500,   0, 1, 0, 0, 1 },  /* solid (blue)            */
    [LED_READY]         = {  250, 125,   0, 1, 0, 1, 1 },  /* 4 Hz blink (cyan)       */
    [LED_RECORDING]     = { 1000,  50,   0, 1, 0, 1, 0 },  /* wink / s (green)        */
    [LED_LEADOFF]       = {  500, 250,   0, 1, 1, 1, 0 },  /* 2 Hz blink (yellow)     */
    [LED_WARN]          = { 1500,  90, 160, 2, 1, 1, 0 },  /* double-blink (amber)    */
    [LED_FAULT_STORAGE] = { 1200, 110, 160, 3, 1, 0, 0 },  /* triple burst (red)      */
    [LED_FAULT_INIT]    = {  160,  80,   0, 1, 1, 0, 0 },  /* ~6 Hz frantic (red)     */
    [LED_SYNC]          = {  500, 500,   0, 1, 1, 0, 1 },  /* SOLID (magenta) — mono: not a blink, so it's unmistakable vs the winking RECORDING; "hold, do not unplug" */
};

static volatile LED_State s_state    = LED_BOOT;
static volatile uint16_t  s_phase_ms = 0;
static uint8_t            s_inited   = 0;

static int led_is_fault(LED_State s)
{
    return (s == LED_FAULT_STORAGE || s == LED_FAULT_INIT);
}

#if LED_HW_RGB
/* TODO: drive the three RGB channels once the LED is wired (define the pin
   macros in led.h and set LED_HW_RGB to 1). */
static void led_write(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
#else
static void led_write_mono(int on)
{
    HAL_GPIO_WritePin(LED_GPIO_PORT, LED_GPIO_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
#endif

void LED_Init(void)
{
    s_state    = LED_BOOT;
    s_phase_ms = 0;
    s_inited   = 1;
}

void LED_SetState(LED_State s)
{
    if (s >= LED_STATE_COUNT) return;
    if (led_is_fault(s_state) && !led_is_fault(s)) return; /* fatal latches */
    if (s == s_state) return;
    __disable_irq();
    s_state    = s;
    s_phase_ms = 0;
    __enable_irq();
}

static int pattern_on(const led_pattern_t *p, uint16_t phase)
{
    for (uint8_t k = 0; k < p->pulses; k++) {
        uint16_t start = (uint16_t)(k * (p->on_ms + p->gap_ms));
        if (phase >= start && phase < (uint16_t)(start + p->on_ms))
            return 1;
    }
    return 0;
}

void LED_Tick(void)
{
    if (!s_inited) return;
    const led_pattern_t *p = &PATTERNS[s_state];
    if (++s_phase_ms >= p->period_ms)
        s_phase_ms = 0;
    int on = pattern_on(p, s_phase_ms);
#if LED_HW_RGB
    led_write(on ? p->r : 0, on ? p->g : 0, on ? p->b : 0);
#else
    led_write_mono(on);
#endif
}
