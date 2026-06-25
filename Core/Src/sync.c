/* =====================================================================
 * sync.c — multi-device synchronization (see sync.h, sync_protocol.md,
 * FORMAT.md §10).
 *
 * The shared PC6 net is the synchronization reference: one LOW edge is seen by
 * every device on the bus simultaneously. Each device pulses once on arming and
 * re-latches its EMG sample index on every edge it sees, holding the last one
 * until it is unplugged — so all devices still on the bus converge on the same
 * (last) pulse. The latched index is later stored in the file header; subtract
 * it across devices to align their streams to the common instant.
 *
 * Only PC6/PC7 are (re)configured here, by pin, so the grouped PC0 (DRDY EXTI)
 * and PC1 (ADS CS) configured in MX_GPIO_Init are left untouched.
 * ===================================================================== */
#include "sync.h"
#include "main.h"
#include "led.h"
#include "acquisition.h"
#include "i2c_sensors.h"

#define SYNC_PORT   GPIOC
#define SYNC_A_PIN  GPIO_PIN_6   /* Sync A — shared open-drain signal line */
#define SYNC_B_PIN  GPIO_PIN_7   /* Sync B — presence (LOW = cable present) */

#define SYNC_PULSE_LOW_MS     20   /* hold the line low long enough for peers to poll it */
#define SYNC_PULSE_STAGGER_MS 512  /* max per-device pulse delay (derived from the UID). Devices
                                      powered from one shared source boot simultaneously; without a
                                      spread their pulses coincide on the wired-AND line and no device
                                      observes a distinct peer edge. Staggering guarantees a clean,
                                      separated last edge that every earlier-pulsing device latches. */

static void sync_gpio_init(void)
{
    GPIO_InitTypeDef g = {0};

    /* PC7 Sync B — presence detect, input pull-up (LOW = cable shorts it to GND). */
    g.Pin  = SYNC_B_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(SYNC_PORT, &g);

    /* PC6 Sync A — shared signal line, open-drain + pull-up. Released (driven
       to 1 = high-Z) idles high via the pull-up; any device writing 0 pulls the
       whole net low with no contention (wired-AND). The pin's input register
       still reflects the actual line level, so we read it without reconfiguring. */
    HAL_GPIO_WritePin(SYNC_PORT, SYNC_A_PIN, GPIO_PIN_SET);  /* release before enabling */
    g.Pin   = SYNC_A_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_OD;
    g.Pull  = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(SYNC_PORT, &g);
    HAL_GPIO_WritePin(SYNC_PORT, SYNC_A_PIN, GPIO_PIN_SET);  /* ensure released */
}

static int connector_present(void)
{
    return HAL_GPIO_ReadPin(SYNC_PORT, SYNC_B_PIN) == GPIO_PIN_RESET;
}

static int line_low(void)
{
    return HAL_GPIO_ReadPin(SYNC_PORT, SYNC_A_PIN) == GPIO_PIN_RESET;
}

static void set_rtc_zero(void)
{
    RTC_Time_t t = {0};
    RV3028_SetTime(&t);
}

/* Pull the shared line low, latch the sample index at that edge, hold, release. */
static uint32_t emit_pulse(void)
{
    HAL_GPIO_WritePin(SYNC_PORT, SYNC_A_PIN, GPIO_PIN_RESET);  /* drive low */
    uint32_t idx = ACQ_SampleIndex();                          /* latch at the edge */
    HAL_Delay(SYNC_PULSE_LOW_MS);
    HAL_GPIO_WritePin(SYNC_PORT, SYNC_A_PIN, GPIO_PIN_SET);    /* release (high-Z) */
    HAL_Delay(2);
    return idx;
}

SYNC_Result SYNC_Run(void)
{
    SYNC_Result r = { .synced = 0, .pulse_index = 0 };

    sync_gpio_init();

    if (!connector_present())
        return r;   /* no cable → single-device operation, skip sync */

    LED_SetState(LED_SYNC);

    /* Stagger our pulse by a per-device delay (from the UID) so devices booting
       together off one power source don't pulse at the same instant — see the
       SYNC_PULSE_STAGGER_MS note. The last pulse becomes the common shared edge. */
    HAL_Delay(HAL_GetUIDw0() % SYNC_PULSE_STAGGER_MS);

    /* Announce ourselves, latching our own pulse as the initial reference. */
    r.pulse_index = emit_pulse();
    r.synced      = 1;
    set_rtc_zero();

    /* Until we are unplugged, re-latch on every LOW edge on the shared line
       (a pulse from any device) and re-zero the RTC. The last edge before the
       unplug is the reference common to every device still on the bus. */
    int prev_low = line_low();
    while (connector_present()) {
        int low = line_low();
        if (low && !prev_low) {            /* falling edge = a peer's pulse */
            r.pulse_index = ACQ_SampleIndex();
            set_rtc_zero();
        }
        prev_low = low;
    }

    return r;   /* cable unplugged — start recording from the synced reference */
}
