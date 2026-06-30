/* =====================================================================
 * sync.c — multi-device synchronization (see sync.h, SYNC_PROTOCOL.md,
 * FORMAT.md §10).
 *
 * Two things ride the shared open-drain PC6 net, distinguished by how long the
 * line is held LOW:
 *
 *   - Sample sync (~20 ms pulse): each device pulses once on arming; every
 *     device re-latches its EMG sample index on every short pulse, so the last
 *     pulse before unplug is the common reference. (Validated mechanism — the
 *     group-id work below does NOT touch it.)
 *
 *   - Group id (~60 ms framed packet): once the bus is quiet (all devices have
 *     armed), the leader — the device whose UID stagger was shortest, so it fires
 *     while the others are still staggering = listening — broadcasts a 16-bit
 *     session id as [long start][16 id bits][8 checksum bits], repeated a few
 *     times. Every device decodes it. The long start marker is never mistaken for
 *     a sample pulse, so it leaves the reference untouched.
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

#define SYNC_PULSE_LOW_MS     20   /* sample-sync announce pulse — short */
#define SYNC_PULSE_STAGGER_MS 512  /* max per-device pulse delay, computed by uid_stagger_ms()
                                      (FNV-1a hash of the full UID, so same-wafer boards that share
                                      the low UID bits still get distinct delays). Spreads the
                                      announce pulses so devices booting together off one power
                                      source don't collide, and elects the shortest-stagger device
                                      as the group-id leader. */
/* Group-id frame timing. The 60 ms start marker is well clear of the 20 ms sample
   pulse, so a receiver tells them apart by LOW duration (threshold 40 ms). */
#define GID_START_MS      60   /* leader drives LOW this long to mark a frame start */
#define GID_START_MIN_MS  40   /* RX: a LOW >= this is a frame start, else a sample pulse */
#define GID_BIT_MS         8   /* NRZ bit period (also the start->data gap) */
#define GID_DATA_BITS     24   /* 16-bit id + 8-bit checksum, MSB first */
#define GID_QUIET_MS     400   /* bus quiet this long after the last pulse = all armed */
#define GID_REPEATS        3   /* leader sends the frame this many times (late joiners / loss) */
#define GID_REPEAT_GAP_MS 30   /* high gap between repeated frames */

/* ---- low-level line helpers ---- */
static void drive_low(void) { HAL_GPIO_WritePin(SYNC_PORT, SYNC_A_PIN, GPIO_PIN_RESET); }
static void release_high(void) { HAL_GPIO_WritePin(SYNC_PORT, SYNC_A_PIN, GPIO_PIN_SET); }
static int  line_low(void) { return HAL_GPIO_ReadPin(SYNC_PORT, SYNC_A_PIN) == GPIO_PIN_RESET; }
static int  connector_present(void) { return HAL_GPIO_ReadPin(SYNC_PORT, SYNC_B_PIN) == GPIO_PIN_RESET; }

/* DWT-based busy wait — accurate across the DRDY ISR (the ISR's cycles are
   counted, so the wall-clock duration is honoured). DWT->CYCCNT is enabled in
   ACQ_Init(), which runs before SYNC_Run(). */
static void busy_ms(uint32_t ms)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t cyc = ms * (SystemCoreClock / 1000);
    while ((DWT->CYCCNT - start) < cyc) { }
}

static void set_rtc_zero(void)
{
    RTC_Time_t t = {0};
    RV3028_SetTime(&t);
}

static void sync_gpio_init(void)
{
    GPIO_InitTypeDef g = {0};

    /* PC7 Sync B — presence detect, input pull-up (LOW = cable shorts it to GND). */
    g.Pin  = SYNC_B_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(SYNC_PORT, &g);

    /* PC6 Sync A — shared signal line, open-drain + pull-up. Released (driven to
       1 = high-Z) idles high via the pull-up; any device writing 0 pulls the whole
       net low with no contention (wired-AND). The input register still reflects the
       actual line level, so we read it without reconfiguring. */
    release_high();
    g.Pin   = SYNC_A_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_OD;
    g.Pull  = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(SYNC_PORT, &g);
    release_high();
}

/* Sample-sync announce pulse: pull the line low briefly, latch the sample index
   at the edge, hold, release. */
static uint32_t emit_pulse(void)
{
    drive_low();
    uint32_t idx = ACQ_SampleIndex();   /* latch at the falling edge */
    busy_ms(SYNC_PULSE_LOW_MS);
    release_high();
    busy_ms(2);
    return idx;
}

/* Measure how long the line stays LOW from now (it is assumed low), in ms,
   bounded by timeout_ms. Returns when the line goes high or the timeout hits. */
static uint32_t low_duration_ms(uint32_t timeout_ms)
{
    uint32_t cpm = SystemCoreClock / 1000;
    uint32_t start = DWT->CYCCNT;
    while (line_low()) {
        if ((DWT->CYCCNT - start) > timeout_ms * cpm) break;
    }
    return (DWT->CYCCNT - start) / cpm;
}

/* Leader: transmit one group-id frame, reading the bus back on every '1' bit to
   detect a colliding second leader. Returns 1 if sent cleanly, 0 on collision. */
static int send_gid_frame(uint16_t id)
{
    uint8_t  cks = (uint8_t)((id >> 8) + (id & 0xFF));
    uint32_t frame = ((uint32_t)id << 8) | cks;   /* 24 bits, MSB first */

    drive_low();    busy_ms(GID_START_MS);         /* start marker */
    release_high(); busy_ms(GID_BIT_MS);           /* start->data gap (one bit of high) */

    for (int i = GID_DATA_BITS - 1; i >= 0; i--) {
        if ((frame >> i) & 1u) {
            release_high();
            busy_ms(GID_BIT_MS / 2);
            if (line_low()) { release_high(); return 0; }  /* another driver -> collision */
            busy_ms(GID_BIT_MS - GID_BIT_MS / 2);
        } else {
            drive_low();
            busy_ms(GID_BIT_MS);
        }
    }
    release_high();
    busy_ms(GID_BIT_MS);
    return 1;
}

/* Follower: decode the 24 frame bits. Call right after low_duration_ms() reported
   a frame-start (the start LOW just ended, line is high); rise_cyc = DWT then.
   Samples each bit at its mid-point. Returns 1 and *out on good checksum. */
static int decode_gid_frame(uint32_t rise_cyc, uint16_t *out)
{
    uint32_t cpm = SystemCoreClock / 1000;
    uint32_t frame = 0;
    for (int i = 0; i < GID_DATA_BITS; i++) {
        /* gap = 1 bit, then bit i spans [rise + (i+1)*BIT, rise + (i+2)*BIT];
           sample its mid-point. */
        uint32_t target = rise_cyc + ((uint32_t)(i + 1) * GID_BIT_MS + GID_BIT_MS / 2) * cpm;
        while ((int32_t)(DWT->CYCCNT - target) < 0) { }
        frame = (frame << 1) | (line_low() ? 0u : 1u);   /* high = 1, low = 0 */
    }
    uint16_t id  = (uint16_t)(frame >> 8);
    uint8_t  cks = (uint8_t)(frame & 0xFF);
    if (cks != (uint8_t)((id >> 8) + (id & 0xFF))) return 0;
    *out = id;
    return 1;
}

/* Per-device pulse-stagger delay in ms (0 .. SYNC_PULSE_STAGGER_MS-1), derived from the
   full 96-bit unique id via FNV-1a. Using all 12 UID bytes — not HAL_GetUIDw0() % N —
   matters because devices from the same wafer/lot share the low UID bits: with the old
   w0 % 512, two such boards got the SAME stagger, tied for shortest, both self-elected as
   group-id leader, and their broadcasts collided (id collapses to 0). Hashing the whole
   UID gives them distinct staggers, hence distinct leader priority. */
static uint32_t uid_stagger_ms(void)
{
    const uint32_t w[3] = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };
    uint32_t h = 2166136261u;            /* FNV-1a 32-bit offset basis */
    for (int i = 0; i < 3; i++)
        for (int s = 0; s < 32; s += 8)  /* low byte first */
            h = (h ^ ((w[i] >> s) & 0xFFu)) * 16777619u;
    return h % SYNC_PULSE_STAGGER_MS;
}

SYNC_Result SYNC_Run(void)
{
    SYNC_Result r = { .synced = 0, .pulse_index = 0, .group_id = 0 };

    sync_gpio_init();
    if (!connector_present())
        return r;   /* no cable → single-device operation, skip sync */

    LED_SetState(LED_SYNC);

    uint16_t my_id = ACQ_SyncSeed();   /* our candidate group id, used only if we lead */

    /* Stagger, listening. If we hear the line go low during the stagger, another
       device pulsed first → we are a follower. If the stagger elapses with the
       line quiet, we are the leader (shortest stagger). */
    uint8_t  is_leader = 1;
    uint32_t cpm = SystemCoreClock / 1000;
    uint32_t s_start = DWT->CYCCNT;
    uint32_t s_cyc = uid_stagger_ms() * cpm;
    while ((DWT->CYCCNT - s_start) < s_cyc) {
        if (line_low()) { is_leader = 0; break; }
    }

    /* Announce ourselves (sample-sync), latching our own pulse as initial reference. */
    r.pulse_index = emit_pulse();
    r.synced      = 1;
    set_rtc_zero();

    uint8_t  frames_left = is_leader ? GID_REPEATS : 0;
    uint32_t last_edge_ms = HAL_GetTick();
    int      prev_low = line_low();

    while (connector_present()) {
        int low = line_low();
        if (low && !prev_low) {
            uint32_t idx = ACQ_SampleIndex();       /* latch at the falling edge */
            uint32_t dur = low_duration_ms(GID_START_MS + 40);
            uint32_t rise_cyc = DWT->CYCCNT;
            last_edge_ms = HAL_GetTick();
            if (dur < GID_START_MIN_MS) {            /* short → sample-sync pulse */
                r.pulse_index = idx;
                set_rtc_zero();
            } else {                                 /* long → group-id frame */
                uint16_t id;
                if (decode_gid_frame(rise_cyc, &id)) r.group_id = id;
            }
            prev_low = line_low();
            continue;
        }
        prev_low = low;

        /* Leader: once the bus has been quiet long enough that all peers have
           armed, broadcast the id a few times back-to-back, then go silent. The
           frames don't move the sample reference (they're long-start, ignored
           above). On a collision (a second leader) drop the id → group_id stays
           0 and the Inspector falls back to heuristic pairing. */
        if (frames_left && (uint32_t)(HAL_GetTick() - last_edge_ms) > GID_QUIET_MS) {
            int ok = 1;
            for (int k = 0; k < GID_REPEATS; k++) {
                if (!send_gid_frame(my_id)) { ok = 0; break; }
                busy_ms(GID_REPEAT_GAP_MS);
            }
            r.group_id = ok ? my_id : 0;
            frames_left = 0;
            last_edge_ms = HAL_GetTick();
            prev_low = line_low();
        }
    }

    return r;   /* cable unplugged — start recording from the synced reference */
}
