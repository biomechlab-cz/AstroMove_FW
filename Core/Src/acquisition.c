#include <string.h>
#include "acquisition.h"
#include "ads1292.h"
#include "i2c_sensors.h"
#include "recording.h"
#include "led.h"
#include "main.h"
#include "fatfs.h"

/* ---- SD init (4-bit, ~470 kHz — see CLKDIV note in sd_init_4bit) ---- */
extern SD_HandleTypeDef hsd1;

static void sdmmc_gpio_very_high(void)
{
    GPIO_InitTypeDef g = {0};
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF12_SDMMC1;
    g.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_2;
    HAL_GPIO_Init(GPIOD, &g);
}

static int sd_wait_transfer(uint32_t ms)
{
    uint32_t t0 = HAL_GetTick();
    while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER) {
        if (HAL_GetTick() - t0 > ms) return 0;
        HAL_Delay(1);
    }
    return 1;
}

static uint8_t sd_init_4bit(void)
{
    hsd1.Init.ClockDiv = 10;
    hsd1.Init.BusWide  = SDMMC_BUS_WIDE_1B;
    if (HAL_SD_Init(&hsd1) != HAL_OK) return 0;
    sdmmc_gpio_very_high();
    if (!sd_wait_transfer(1000)) return 0;
    if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK) return 0;
    /* CLKDIV=100 → 48 MHz/(100+2) ≈ 470 kHz, ~235 KB/s in 4-bit mode.
       Slower than needed for the data rate (5 KB/s) on purpose: the FIFO
       is fed by polling and must survive the ~94 µs DRDY ISR. At CLKDIV=50
       the half-FIFO refill deadline can dip below the ISR duration, which
       failed every multi-block write with TX underrun. */
    MODIFY_REG(SDMMC1->CLKCR, SDMMC_CLKCR_CLKDIV, 100U);
    HAL_Delay(20);
    return sd_wait_transfer(500);
}

/* ====================================================================
 * Sample buffers — one chunk (1 s of EMG), handed to the recording
 * module which encrypts and stores them (see recording.c for the format)
 * ==================================================================== */
static int32_t  s_ch1[REC_CHUNK_SAMPLES];     /* 24-bit ADC counts sign-extended */
static uint16_t s_count = 0;

/* IMU slots — one per 10 EMG samples, so the IMU grid is derived from the
   ADS sample clock and every chunk holds exactly REC_CHUNK_IMU_SAMPLES.
   Slots filled while draining; after a blocking SD write the catch-up
   slots repeat the sensor's then-current reading (sample-and-hold). */
static REC_ImuSample s_imu[REC_CHUNK_IMU_SAMPLES];
static REC_ImuSample s_imu_hold;            /* last good reading, reused on I2C failure */
volatile uint32_t g_ism_fail_count = 0;
volatile uint32_t g_mag_fail_count = 0;
volatile uint32_t g_i2c_last_err = 0;       /* hi2c1.ErrorCode of the last failure */

extern I2C_HandleTypeDef hi2c1;

static FATFS s_fs;

#if REC_DIAG_ARTIFACT
/* ================= TEMPORARY: artifact / noise hunt =========================
 * Rotates one condition per batch (10 s) so ONE recording covers every
 * condition with identical electrodes/skin/environment. Disable via
 * REC_DIAG_ARTIFACT in recording.h.
 *
 * mode = batch_index % DIAG_MODE_COUNT  (derivable from the CSV; no format change)
 *   0 BASELINE   everything normal — reference
 *   1 NO_IMU     no I2C traffic at all (sample-and-hold) — DECISIVE test: if the
 *                1 Hz spike and/or the 100 Hz comb vanish here, I2C is the cause
 *   2 NO_LED     LED held off — tests LED coupling
 *   3 PROBE_I2C  normal + a bunched 8x IMU read at DIAG_PROBE_PHASE — positive
 *                test: does a bunched burst create a spike at that phase?
 *   4 PROBE_SD   normal + an SD write (f_sync) at DIAG_PROBE_PHASE — positive
 *                test for SD-write coupling
 *   5 ISM_ONLY   read only the ISM330 (skip the magnetometer)
 *   6 MAG_ONLY   read only the magnetometer (skip the ISM330)
 *   7 BASELINE2  reference repeat — guards against drift/electrode changes
 * ========================================================================= */
enum {
    DIAG_BASELINE = 0, DIAG_NO_IMU, DIAG_NO_LED, DIAG_PROBE_I2C,
    DIAG_PROBE_SD, DIAG_ISM_ONLY, DIAG_MAG_ONLY, DIAG_BASELINE2,
    DIAG_MODE_COUNT
};
#define DIAG_PROBE_PHASE 500   /* mid-chunk: far from the phase-44 artifact */
#define DIAG_PROBE_I2C_READS 8 /* mimics the bunched catch-up burst after a write */

static uint32_t s_diag_chunk = 0;        /* chunks since session start */
volatile uint8_t g_diag_mode = 0;        /* current mode (readable over SWD) */

static uint8_t diag_mode(void)
{
    return (uint8_t)((s_diag_chunk / REC_CHUNKS_PER_BATCH) % DIAG_MODE_COUNT);
}

/* Injected stimulus at a known sample phase, for the positive tests. */
static void diag_probe(void)
{
    switch (diag_mode()) {
    case DIAG_PROBE_SD:
        REC_DiagSyncNow();
        break;
    case DIAG_PROBE_I2C:
        for (int i = 0; i < DIAG_PROBE_I2C_READS; i++) {
            ISM330_Data_t a; MMC5983_Data_t m;
            (void)ISM330_ReadSample(&a);
            (void)MMC5983_ReadSample(&m);
        }
        break;
    default:
        break;
    }
}
#endif /* REC_DIAG_ARTIFACT */

#if REC_DIAG_NOISE
/* ============ TEMPORARY: noise floor vs SD write rate x magnetometer ========
 * Full 4x2 factorial, one cell per batch (10 s); phase = batch_index % 8.
 * Crossing both factors also exposes any interaction between them.
 *
 *   write rate = phase >> 1        magnetometer read = even phase
 *     0  no write   (10 s buffered in RAM, card completely idle while sampling,
 *                    then flushed in one burst; samples captured during the
 *                    flush are DISCARDED, so every recorded sample of the batch
 *                    was taken with zero SD activity)
 *     1  1 Hz       (normal operation — one chunk write per second)
 *     2  2 Hz       (chunk write + 1 extra write event mid-chunk)
 *     3  10 Hz      (chunk write + 9 extra events — deliberate stress test)
 *
 *   phase: 0 W0/mag+  1 W0/mag-  2 W1/mag+  3 W1/mag-
 *          4 W2/mag+  5 W2/mag-  6 W10/mag+ 7 W10/mag-
 *
 * Analysis compares the broadband floor across cells with the write windows
 * excluded, so only a *continuous* effect of SD activity can show up.
 * ========================================================================== */
#define NZ_PHASES 8
#define NZ_BUF_CHUNKS REC_CHUNKS_PER_BATCH   /* one buffered burst == one batch */

/* extra write events per chunk, by write-rate index: 0 samples = none */
static const uint16_t NZ_SYNC_STEP[4] = { 0, 0, 500, 100 };

static int32_t       s_nzb_ch1[NZ_BUF_CHUNKS][REC_CHUNK_SAMPLES];
static REC_ImuSample s_nzb_imu[NZ_BUF_CHUNKS][REC_CHUNK_IMU_SAMPLES];
static struct { uint32_t dropped, sat, flat, drift, leadoff, diff; }
                     s_nzb_q[NZ_BUF_CHUNKS];
static uint32_t s_nzb_n     = 0;   /* chunks currently buffered */
static uint32_t s_nz_chunk  = 0;   /* chunks written so far in a writing phase */
static uint32_t s_nz_batch  = 0;   /* completed batches — drives the phase */
volatile uint8_t g_nz_phase = 0;   /* current phase (readable over SWD) */

static uint8_t nz_phase(void)     { return (uint8_t)(s_nz_batch % NZ_PHASES); }
static uint8_t nz_rate(void)      { return (uint8_t)(nz_phase() >> 1); }   /* 0..3 */
static uint8_t nz_mag_on(void)    { return (uint8_t)((nz_phase() & 1u) == 0u); }
static uint8_t nz_buffering(void) { return nz_rate() == 0; }
#endif /* REC_DIAG_NOISE */

static void imu_sample(REC_ImuSample *out)
{
    ISM330_Data_t a;
    MMC5983_Data_t m;
    uint8_t read_ism = 1, read_mag = 1;
#if REC_DIAG_NOISE
    if (!nz_mag_on()) read_mag = 0;   /* odd phases: silence the 100 Hz comb */
#endif
#if REC_DIAG_ARTIFACT
    uint8_t dm = diag_mode();
    if (dm == DIAG_NO_IMU) { *out = s_imu_hold; return; }  /* zero I2C traffic */
    read_ism = (dm != DIAG_MAG_ONLY);
    read_mag = (dm != DIAG_ISM_ONLY);
#endif
    if (read_ism) {
        if (ISM330_ReadSample(&a)) {
            s_imu_hold.ax = a.ax; s_imu_hold.ay = a.ay; s_imu_hold.az = a.az;
            s_imu_hold.gx = a.gx; s_imu_hold.gy = a.gy; s_imu_hold.gz = a.gz;
        } else {
            g_ism_fail_count++;
            g_i2c_last_err = hi2c1.ErrorCode;
        }
    }
    if (read_mag) {
        if (MMC5983_ReadSample(&m)) {
            s_imu_hold.mx = m.mx; s_imu_hold.my = m.my; s_imu_hold.mz = m.mz;
        } else {
            g_mag_fail_count++;
            g_i2c_last_err = hi2c1.ErrorCode;
        }
    }
    *out = s_imu_hold;
}

/* ====================================================================
 * ISR path — the sample is read from the ADS inside the DRDY interrupt
 * and pushed into a ring buffer, so the blocking encrypt + SD write in
 * the main loop cannot lose samples (the ring holds ~1 s).
 *
 * EXTI must be armed only after ADS1292_StartContinuous() so the ISR's
 * SPI reads never overlap main-thread SPI traffic (see main.c).
 * ==================================================================== */
#define RING_LEN  1024              /* power of two; ~1 s at 1 kSPS */
#define RING_MASK (RING_LEN - 1)

static volatile int32_t  s_ring_ch1[RING_LEN];
static volatile uint16_t s_ring_head = 0;  /* written by ISR only */
static volatile uint16_t s_ring_tail = 0;  /* written by main loop only */

/* DRDY at 1 kSPS pulses every ~1000 µs; edges closer than 800 µs to the
   last accepted one are noise (board 04 DRDY produces spurious edges). */
static uint32_t s_debounce_cycles;         /* 800 µs in DWT cycles */
static uint32_t s_last_edge_cycles;

volatile uint32_t g_isr_count = 0;
volatile uint32_t g_spurious_count = 0;    /* edges rejected by debounce */
volatile uint32_t g_dropped_count = 0;     /* samples lost to a full ring */
volatile uint32_t g_isr_max_cycles = 0;    /* worst-case ISR duration (debug) */
volatile uint32_t g_sample_index = 0;      /* free-running count of valid conversions —
                                              the EMG sample timeline used for multi-device
                                              sync latching (counts dropped samples too) */
static uint16_t   g_sync_seed = 0;         /* per-session entropy for the sync group-id (sync.c) */
static uint8_t    s_nonce_salt[8];         /* per-session AES-GCM nonce salt; gathered by
                                              ACQ_SeedNonce(), installed by ACQ_OpenSession()
                                              AFTER REC_Open() (session not open during SeedNonce) */

/* Live lead-off (status LED). Asserted after LEADOFF_DEBOUNCE consecutive
   samples with a CH1 electrode off, cleared on the first good sample.
   g_stat0_or/g_stat1_or are sticky ORs of the status bytes for verifying
   the lead-off bit positions on hardware over SWD. */
#define LEADOFF_DEBOUNCE 50                /* consecutive lead-off samples (~50 ms at 1 kSPS) */
volatile uint8_t  g_leadoff_active = 0;
volatile uint8_t  g_stat0_or = 0;
volatile uint8_t  g_stat1_or = 0;
static   uint16_t s_leadoff_run = 0;

/* Live signal-saturation: a railed input flags a bad/high-impedance contact that
   lead-off can miss (the PGA saturates differentially before the lead-off
   comparator's absolute threshold trips — observed with the textile-electrode
   bond). Counted and debounced like lead-off; also lights the lead-off LED. */
#define SAT_THRESHOLD ((1 << 23) - 8)      /* |ch1| within 8 LSB of ±full-scale */
#define SAT_DEBOUNCE  50                   /* consecutive saturated samples (~50 ms) */
volatile uint8_t  g_saturated_active = 0;
volatile uint32_t g_saturated_count = 0;   /* CH1 saturated samples since last chunk (→ CSV) */
static   uint16_t s_sat_run = 0;

/* Recoverable-warning hold for the LED (e.g. dropped samples): WARN is shown
   for ~3 s after the event so a brief overrun is still visible. */
#define QUALITY_HOLD_MS 3000U
static uint32_t s_warn_deadline = 0;
static uint8_t  s_warn_on = 0;
static uint32_t s_bad_signal_deadline = 0;
static uint8_t  s_bad_signal_on = 0;

/* Chunk-level signal quality checks. These intentionally run outside the ISR
   and use conservative thresholds: flag clearly unusable signal first, then
   tune from CSVs recorded on real subjects/electrodes. */
#define QUALITY_FLATLINE_P2P_COUNTS          32
#define QUALITY_FLATLINE_DIFF_SUM_COUNTS     128
#define QUALITY_FLATLINE_REPEAT_RUN          950
#define QUALITY_BASELINE_EDGE_SAMPLES        100
#define QUALITY_BASELINE_DRIFT_COUNTS        100000
/* Signal-based lead-off (the ADS hardware comparators are off — see ads1292.c).
   A connected body couples mains + baseline, so a 1 s chunk's sum-of-abs-diffs is
   in the millions; a disconnected electrode is far quieter. Measured: disconnected
   ~450k–495k, connected >2.9M (rest ~5M). Below this threshold (but not flatline)
   → electrode likely off. Calibrated in a mains environment — RE-TUNE for battery,
   where the connected level drops (use samples/_calib_leadoff.py on new sessions). */
#define QUALITY_LEADOFF_DIFF_SUM_COUNTS      1000000
/* The other lead-off signature: a floating electrode left dangling/coupled doesn't go
   quiet — it picks up huge interference and swings wildly. Across hardware test
   sessions the bands separated cleanly: connected EMG ≤ ~7.5M, wild float ≥ ~16M, with
   an empty gap between. Anything above this upper bound is treated as floating bad
   contact. Same mains-environment calibration caveat — on battery both the connected
   level and the float pickup drop, so RE-TUNE (samples/_leadoff_perchunk.py). If strong
   voluntary EMG ever false-trips this, raise it (observed wild floats were ≥ 16M). */
#define QUALITY_LEADOFF_HI_DIFF_SUM_COUNTS   12000000

typedef struct {
    uint32_t flags;
    uint32_t flatline_chunks;
    uint32_t baseline_drift_chunks;
    uint32_t leadoff_chunks;
    uint32_t diff_abs_sum;   /* this chunk's sum-of-abs-diffs (continuous level metric) */
    uint8_t  bad_contact;
    uint8_t  noisy;
} ACQ_SignalQuality;

static int64_t abs_i64(int64_t v)
{
    return (v < 0) ? -v : v;
}

static ACQ_SignalQuality analyze_signal_quality(const int32_t ch1[REC_CHUNK_SAMPLES])
{
    ACQ_SignalQuality q = {0};
    int32_t min_v = ch1[0];
    int32_t max_v = ch1[0];
    int32_t prev = ch1[0];
    uint16_t repeat_run = 1;
    uint16_t max_repeat_run = 1;
    int64_t diff_abs_sum = 0;
    int64_t first_edge_sum = 0;
    int64_t last_edge_sum = 0;

    for (uint16_t i = 0; i < REC_CHUNK_SAMPLES; i++) {
        int32_t x = ch1[i];
        if (x < min_v) min_v = x;
        if (x > max_v) max_v = x;

        if (i < QUALITY_BASELINE_EDGE_SAMPLES)
            first_edge_sum += x;
        if (i >= (REC_CHUNK_SAMPLES - QUALITY_BASELINE_EDGE_SAMPLES))
            last_edge_sum += x;

        if (i > 0) {
            diff_abs_sum += abs_i64((int64_t)x - prev);
            if (x == prev) {
                if (repeat_run < REC_CHUNK_SAMPLES)
                    repeat_run++;
            } else {
                repeat_run = 1;
            }
            if (repeat_run > max_repeat_run)
                max_repeat_run = repeat_run;
            prev = x;
        }
    }

    int64_t peak_to_peak = (int64_t)max_v - min_v;
    if (peak_to_peak <= QUALITY_FLATLINE_P2P_COUNTS ||
        diff_abs_sum <= QUALITY_FLATLINE_DIFF_SUM_COUNTS ||
        max_repeat_run >= QUALITY_FLATLINE_REPEAT_RUN) {
        q.flags |= REC_SIGQ_FLATLINE;
        q.flatline_chunks = 1;
        q.bad_contact = 1;
    } else if (diff_abs_sum < QUALITY_LEADOFF_DIFF_SUM_COUNTS ||
               diff_abs_sum > QUALITY_LEADOFF_HI_DIFF_SUM_COUNTS) {
        /* Not flat, but outside the connected-EMG band → lead-off. Two signatures:
           too quiet (open electrode settled) or too wild (floating electrode picking
           up interference). Connected EMG sits between the two thresholds. */
        q.flags |= REC_SIGQ_LEADOFF;
        q.leadoff_chunks = 1;
        q.bad_contact = 1;
    }

    int64_t baseline_delta =
        (last_edge_sum - first_edge_sum) / QUALITY_BASELINE_EDGE_SAMPLES;
    if (!(q.flags & (REC_SIGQ_FLATLINE | REC_SIGQ_LEADOFF)) &&
        abs_i64(baseline_delta) >= QUALITY_BASELINE_DRIFT_COUNTS) {
        q.flags |= REC_SIGQ_BASELINE_DRIFT;
        q.baseline_drift_chunks = 1;
        q.noisy = 1;
    }

    /* Report the raw level so thresholds stay re-tunable from the control CSV
       alone (no EMX decode). Clamp the int64 accumulator into uint32. */
    q.diff_abs_sum = (diff_abs_sum > 0xFFFFFFFF) ? 0xFFFFFFFFu
                                                 : (uint32_t)diff_abs_sum;
    return q;
}

void ACQ_DRDY_Callback(void)
{
    uint32_t t_entry = DWT->CYCCNT;
    g_isr_count++;

    uint32_t now = DWT->CYCCNT;
    if (now - s_last_edge_cycles < s_debounce_cycles) {
        g_spurious_count++;
        return;
    }
    s_last_edge_cycles = now;

    uint8_t raw[9];
    ADS1292_ReadRawFast(raw); /* direct-register read, sends NOPs on MOSI */

    /* Normalized lead-off status (ADS1292_NORMALIZE_STATUS) drives only the live
       LED (HW comparators are off, so it is inert today) — it is NOT stored.
       Done before the ring check so a dropped sample still updates signal quality. */
    uint8_t status = ADS1292_NORMALIZE_STATUS(raw[0], raw[1]);
    g_stat0_or |= raw[0];   /* sticky raw ORs kept for SWD bring-up/debug */
    g_stat1_or |= raw[1];

    /* CH1 (IN1P/IN1N) carries the EMG. Debounce a sustained HW lead-off run for
       the LED (signal-based lead-off is detected per-chunk in the main loop). */
    if (status & ADS1292_STATUS_CH1_LEADOFF) {
        if (s_leadoff_run < LEADOFF_DEBOUNCE) s_leadoff_run++;
    } else {
        s_leadoff_run = 0;
    }
    g_leadoff_active = (s_leadoff_run >= LEADOFF_DEBOUNCE);

    /* CH1 sample (24-bit sign-extended), reused for the saturation check below. */
    int32_t ch1 = ((int32_t)((uint32_t)raw[3] << 24 |
                             (uint32_t)raw[4] << 16 |
                             (uint32_t)raw[5] << 8)) >> 8;
    if (ch1 >= SAT_THRESHOLD || ch1 <= -SAT_THRESHOLD) {
        g_saturated_count++;
        if (s_sat_run < SAT_DEBOUNCE) s_sat_run++;
    } else {
        s_sat_run = 0;
    }
    g_saturated_active = (s_sat_run >= SAT_DEBOUNCE);

    /* Advance the EMG sample timeline before the ring check, so a dropped
       sample still ticks the clock used for multi-device sync alignment. */
    g_sample_index++;

    uint16_t next = (uint16_t)((s_ring_head + 1) & RING_MASK);
    if (next == s_ring_tail) {  /* ring full — main loop stalled > ~1 s */
        g_dropped_count++;
        return;
    }
    s_ring_ch1[s_ring_head] = ch1;
    s_ring_head = next;

    uint32_t elapsed = DWT->CYCCNT - t_entry;
    if (elapsed > g_isr_max_cycles)
        g_isr_max_cycles = elapsed;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_0)
        ACQ_DRDY_Callback();
}

/* ====================================================================
 * Public API
 * ==================================================================== */
void ACQ_SeedNonce(void)
{
    /* Per-session AES-GCM nonce salt from analog front-end noise (FORMAT.md §6).
       Called after ADS1292_StartContinuous() and before the DRDY EXTI is armed:
       the ADS is streaming in RDATAC, so the main thread can read frames with
       ADS1292_ReadRawFast (transmits 0x00 on MOSI — mandatory, see CLAUDE.md).
       The LSBs of ~128 conversions are mixed (FNV-1a 64-bit) with the free-
       running CPU cycle counter and the 96-bit device UID, then installed. */
    const uint64_t prime = 0x100000001b3ULL;
    uint64_t h = 0xcbf29ce484222325ULL;        /* FNV-1a 64 offset basis */
    h = (h ^ HAL_GetUIDw0()) * prime;          /* cross-device uniqueness */
    h = (h ^ HAL_GetUIDw1()) * prime;
    h = (h ^ HAL_GetUIDw2()) * prime;

    for (int i = 0; i < 128; i++) {
        uint8_t raw[9];
        ADS1292_ReadRawFast(raw);
        h = (h ^ raw[5]) * prime;              /* ch1 LSB — analog thermal noise */
        h = (h ^ raw[8]) * prime;              /* ch2 LSB — analog thermal noise */
        h = (h ^ DWT->CYCCNT) * prime;         /* sampling jitter */
        HAL_Delay(1);                          /* ~one conversion period for a fresh sample */
    }

    /* Stash the salt — it is installed into the header by ACQ_OpenSession() after
       REC_Open() opens the session. We CANNOT call REC_SetNonceSalt() here: this
       runs before the sync sequence, so the session is not open yet and the salt
       would be silently dropped (which previously left the nonce = device UID). */
    for (int i = 0; i < 8; i++)
        s_nonce_salt[i] = (uint8_t)(h >> (8 * i));

    /* Per-session 16-bit seed for the multi-device sync group-id (sync.c). Same
       analog entropy, so it differs every session; forced nonzero so 0 can mean
       "no group id". */
    g_sync_seed = (uint16_t)((s_nonce_salt[2] << 8) | s_nonce_salt[3]);
    if (g_sync_seed == 0) g_sync_seed = 0xA5A5;
}
/* ADS register snapshot taken in ACQ_Init() (SDATAC mode), held until the
   session is opened by ACQ_OpenSession() after the sync sequence has run. */
static uint8_t s_ads_regs[10];

uint8_t ACQ_Init(void)
{
    /* DWT cycle counter — used by the ISR for DRDY edge debouncing */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    s_debounce_cycles = SystemCoreClock / 1250; /* 800 µs */

    if (!sd_init_4bit()) return 0;
    if (f_mount(&s_fs, SDPath, 1) != FR_OK) return 0;

    /* ADS register snapshot for the file header — chip must still be in SDATAC
       mode (before ADS1292_StartContinuous). Stored until ACQ_OpenSession(),
       which runs after the sync sequence so the header reflects the synced RTC. */
    static const uint8_t reg_addr[10] = {
        ADS1292_REG_ID,      ADS1292_REG_CONFIG1,  ADS1292_REG_CONFIG2,
        ADS1292_REG_LOFF,    ADS1292_REG_CH1SET,   ADS1292_REG_CH2SET,
        ADS1292_REG_RLDSENS, ADS1292_REG_LOFFSENS, ADS1292_REG_RESP1,
        ADS1292_REG_RESP2,
    };
    for (uint8_t i = 0; i < 10; i++)
        s_ads_regs[i] = ADS1292_ReadReg(reg_addr[i]);

    return 1;
}

/* Open the recording session (creates the files and writes the header). Called
   after the sync sequence, so the header carries the synced RTC start time plus
   the sync result. synced = 1 if a multi-device sync pulse was latched this
   power-up; sync_lead_samples = EMG samples from the shared sync pulse to the
   first recorded sample (subtract across devices to align streams — FORMAT.md §10). */
uint8_t ACQ_OpenSession(uint8_t synced, uint32_t sync_lead_samples, uint16_t group_id)
{
    if (!REC_Open(s_ads_regs, synced, sync_lead_samples, group_id))
        return 0;
    /* Now that the session is open, install the entropy salt gathered by
       ACQ_SeedNonce() — this rewrites the header nonce_prefix from the UID
       placeholder to the per-session salt (FORMAT.md §6). */
    REC_SetNonceSalt(s_nonce_salt);
    return 1;
}

/* Discard everything sampled so far (e.g. during the sync wait) and clear the
   drop counter so recording starts clean. Returns the sample index at the reset
   point — the absolute index of (about) the first sample that will be recorded. */
uint32_t ACQ_ResetRing(void)
{
    __disable_irq();
    uint32_t idx = g_sample_index;
    s_ring_head = 0;
    s_ring_tail = 0;
    s_count     = 0;
    g_dropped_count = 0;
    __enable_irq();
    return idx;
}

/* Current value of the free-running EMG sample timeline (see g_sample_index). */
uint32_t ACQ_SampleIndex(void)
{
    return g_sample_index;
}

/* Per-session 16-bit entropy seed for the multi-device sync group-id (set by
   ACQ_SeedNonce from the ADS analog noise; nonzero). */
uint16_t ACQ_SyncSeed(void)
{
    return g_sync_seed;
}

#if REC_DIAG_NOISE
/* Route a completed chunk per the current phase. Same contract as
   REC_WriteChunk: returns 0 on an unrecoverable storage failure. */
static uint8_t nz_handle_chunk(uint32_t dropped, uint32_t sat,
                               const ACQ_SignalQuality *q)
{
    if (nz_buffering()) {
        uint32_t i = s_nzb_n;
        memcpy(s_nzb_ch1[i], s_ch1, sizeof(s_ch1));
        memcpy(s_nzb_imu[i], s_imu, sizeof(s_imu));
        s_nzb_q[i].dropped = dropped;              s_nzb_q[i].sat   = sat;
        s_nzb_q[i].flat    = q->flatline_chunks;   s_nzb_q[i].drift = q->baseline_drift_chunks;
        s_nzb_q[i].leadoff = q->leadoff_chunks;    s_nzb_q[i].diff  = q->diff_abs_sum;
        if (++s_nzb_n < NZ_BUF_CHUNKS)
            return 1;                    /* still buffering — the card stays idle */

        for (i = 0; i < NZ_BUF_CHUNKS; i++)          /* flush the burst at once */
            if (!REC_WriteChunk(s_nzb_ch1[i], s_nzb_imu[i], s_nzb_q[i].dropped,
                                s_nzb_q[i].sat, s_nzb_q[i].flat, s_nzb_q[i].drift,
                                s_nzb_q[i].leadoff, s_nzb_q[i].diff))
                return 0;
        s_nzb_n = 0;
        s_nz_batch++;
        ACQ_ResetRing();   /* discard everything captured during the flush */
        return 1;
    }

    if (!REC_WriteChunk(s_ch1, s_imu, dropped, sat, q->flatline_chunks,
                        q->baseline_drift_chunks, q->leadoff_chunks, q->diff_abs_sum))
        return 0;
    if (++s_nz_chunk >= NZ_BUF_CHUNKS) { s_nz_chunk = 0; s_nz_batch++; }
    return 1;
}
#endif

void ACQ_Process(void)
{
    /* Status LED — pick the highest-priority condition currently active.
       Fatal states latch inside the LED module; WARN outranks LEADOFF. */
    uint32_t tick = HAL_GetTick();
    if (s_warn_on && (int32_t)(tick - s_warn_deadline) >= 0)
        s_warn_on = 0;
    if (s_bad_signal_on && (int32_t)(tick - s_bad_signal_deadline) >= 0)
        s_bad_signal_on = 0;
    LED_State led = LED_RECORDING;
    if (g_leadoff_active || g_saturated_active || s_bad_signal_on)
        led = LED_LEADOFF;  /* hard bad contact / unusable signal */
    if (s_warn_on)        led = LED_WARN;
    LED_SetState(led);
#if REC_DIAG_ARTIFACT
    g_diag_mode = diag_mode();
    LED_Mute(g_diag_mode == DIAG_NO_LED);
#endif
#if REC_DIAG_NOISE
    g_nz_phase = nz_phase();
#endif

    while (s_ring_tail != s_ring_head) {
        if (s_count % (REC_CHUNK_SAMPLES / REC_CHUNK_IMU_SAMPLES) == 0)
            imu_sample(&s_imu[s_count / (REC_CHUNK_SAMPLES / REC_CHUNK_IMU_SAMPLES)]);
#if REC_DIAG_ARTIFACT
        /* Inject the mode's stimulus at a known sample phase. The samples the ISR
           captures during it land at phase DIAG_PROBE_PHASE.. so a coupling shows
           up as a fresh peak there. */
        if (s_count == DIAG_PROBE_PHASE)
            diag_probe();
#endif
#if REC_DIAG_NOISE
        /* Extra write events inside the chunk, raising the write rate above 1 Hz. */
        {
            uint16_t step = NZ_SYNC_STEP[nz_rate()];
            if (step && s_count && (s_count % step) == 0)
                REC_DiagSyncNow();
        }
#endif

        s_ch1[s_count] = s_ring_ch1[s_ring_tail];
        s_ring_tail = (uint16_t)((s_ring_tail + 1) & RING_MASK);

        if (++s_count >= REC_CHUNK_SAMPLES) {
            s_count = 0;

            __disable_irq();
            uint32_t dropped = g_dropped_count;
            uint32_t saturated = g_saturated_count;
            g_dropped_count = 0;
            g_saturated_count = 0;
            __enable_irq();

            ACQ_SignalQuality quality = analyze_signal_quality(s_ch1);

#if REC_DIAG_NOISE
            if (!nz_handle_chunk(dropped, saturated, &quality)) {
#else
            if (!REC_WriteChunk(s_ch1, s_imu, dropped, saturated,
                                quality.flatline_chunks,
                                quality.baseline_drift_chunks,
                                quality.leadoff_chunks,
                                quality.diff_abs_sum)) {
#endif
                LED_SetState(LED_FAULT_STORAGE);  /* SysTick keeps the pattern alive */
                while (1) { }
            }

            tick = HAL_GetTick();
            if (quality.bad_contact) {
                s_bad_signal_deadline = tick + QUALITY_HOLD_MS;
                s_bad_signal_on = 1;
            }
            if (dropped || quality.noisy) {
                s_warn_deadline = tick + QUALITY_HOLD_MS;
                s_warn_on = 1;
            }
#if REC_DIAG_ARTIFACT
            s_diag_chunk++;   /* advance the experiment clock (mode changes per batch) */
#endif
        }
    }
}

void ACQ_Stop(void)
{
    REC_Close();
    f_mount(NULL, SDPath, 0);
}
