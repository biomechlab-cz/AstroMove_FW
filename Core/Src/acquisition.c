#include "acquisition.h"
#include "ads1292.h"
#include "i2c_sensors.h"
#include "recording.h"
#include "main.h"
#include "fatfs.h"

/* ---- SD init (validated 4-bit 480 kHz) ---- */
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
 * Timing calibration
 * ==================================================================== */
// void ACQ_TimingCalibration(void)
// {
//     RTC_Time_t t0 = {0}, t1 = {0};

//     HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_SET);

//     RV3028_ReadTime(&t0);
//     uint32_t tick0 = HAL_GetTick();

//     HAL_Delay(10000);

//     RV3028_ReadTime(&t1);
//     uint32_t tick1 = HAL_GetTick();

//     HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);

//     /* BCD to decimal */
//     uint32_t s0 = (t0.hr  >> 4) * 36000 + (t0.hr  & 0x0F) * 3600
//                 + (t0.min >> 4) *   600 + (t0.min & 0x0F) *   60
//                 + (t0.sec >> 4) *    10 + (t0.sec & 0x0F);
//     uint32_t s1 = (t1.hr  >> 4) * 36000 + (t1.hr  & 0x0F) * 3600
//                 + (t1.min >> 4) *   600 + (t1.min & 0x0F) *   60
//                 + (t1.sec >> 4) *    10 + (t1.sec & 0x0F);

//     uint32_t delta_tick = tick1 - tick0;           /* ms, should be ~10000 */
//     int32_t  delta_rtc  = (int32_t)(s1 - s0);     /* seconds per RTC */

//     char buf[128];
//     int len = snprintf(buf, sizeof(buf),
//         "TIMING: tick=%lu ms  RTC=%ld s  diff=%ld ms\r\n",
//         delta_tick, delta_rtc, (long)delta_tick - delta_rtc * 1000);
// }

/* ====================================================================
 * Sample buffers — one chunk (1 s of EMG), handed to the recording
 * module which encrypts and stores them (see recording.c for the format)
 * ==================================================================== */
static int32_t  s_ch1[REC_CHUNK_SAMPLES];   /* 24-bit ADC counts sign-extended */
static uint8_t  s_loff[REC_CHUNK_SAMPLES];  /* raw ADS status byte 0 (lead-off in bits 6:4) */
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

static void imu_sample(REC_ImuSample *out)
{
    ISM330_Data_t a;
    MMC5983_Data_t m;
    if (ISM330_ReadSample(&a)) {
        s_imu_hold.ax = a.ax; s_imu_hold.ay = a.ay; s_imu_hold.az = a.az;
        s_imu_hold.gx = a.gx; s_imu_hold.gy = a.gy; s_imu_hold.gz = a.gz;
    } else {
        g_ism_fail_count++;
        g_i2c_last_err = hi2c1.ErrorCode;
    }
    if (MMC5983_ReadSample(&m)) {
        s_imu_hold.mx = m.mx; s_imu_hold.my = m.my; s_imu_hold.mz = m.mz;
    } else {
        g_mag_fail_count++;
        g_i2c_last_err = hi2c1.ErrorCode;
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
static volatile uint8_t  s_ring_loff[RING_LEN];
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

    uint16_t next = (uint16_t)((s_ring_head + 1) & RING_MASK);
    if (next == s_ring_tail) {  /* ring full — main loop stalled > ~1 s */
        g_dropped_count++;
        return;
    }
    s_ring_ch1[s_ring_head]  = ((int32_t)((uint32_t)raw[3] << 24 |
                                          (uint32_t)raw[4] << 16 |
                                          (uint32_t)raw[5] << 8)) >> 8;
    s_ring_loff[s_ring_head] = raw[0];
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
uint8_t ACQ_Init(void)
{
    /* DWT cycle counter — used by the ISR for DRDY edge debouncing */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    s_debounce_cycles = SystemCoreClock / 1250; /* 800 µs */

    if (!sd_init_4bit()) return 0;
    if (f_mount(&s_fs, SDPath, 1) != FR_OK) return 0;

    /* ADS register snapshot for the file header — chip must still be in
       SDATAC mode (before ADS1292_StartContinuous) */
    static const uint8_t reg_addr[10] = {
        ADS1292_REG_ID,      ADS1292_REG_CONFIG1,  ADS1292_REG_CONFIG2,
        ADS1292_REG_LOFF,    ADS1292_REG_CH1SET,   ADS1292_REG_CH2SET,
        ADS1292_REG_RLDSENS, ADS1292_REG_LOFFSENS, ADS1292_REG_RESP1,
        ADS1292_REG_RESP2,
    };
    uint8_t ads_regs[10];
    for (uint8_t i = 0; i < 10; i++)
        ads_regs[i] = ADS1292_ReadReg(reg_addr[i]);

    return REC_Open(ads_regs);
}

static void blink_error(uint8_t count)
{
    while (1) {
        HAL_Delay(1000);
        for (uint8_t i = 0; i < count; i++) {
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_SET);
            HAL_Delay(80);
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
            HAL_Delay(80);
        }
    }
}

void ACQ_Process(void)
{
    while (s_ring_tail != s_ring_head) {
        if (s_count % (REC_CHUNK_SAMPLES / REC_CHUNK_IMU_SAMPLES) == 0)
            imu_sample(&s_imu[s_count / (REC_CHUNK_SAMPLES / REC_CHUNK_IMU_SAMPLES)]);

        s_ch1[s_count]  = s_ring_ch1[s_ring_tail];
        s_loff[s_count] = s_ring_loff[s_ring_tail];
        s_ring_tail = (uint16_t)((s_ring_tail + 1) & RING_MASK);

        if (++s_count >= REC_CHUNK_SAMPLES) {
            s_count = 0;

            __disable_irq();
            uint32_t dropped = g_dropped_count;
            g_dropped_count = 0;
            __enable_irq();

            if (!REC_WriteChunk(s_ch1, s_loff, s_imu, dropped))
                blink_error(2);

            HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_10);
        }
    }
}

void ACQ_Stop(void)
{
    REC_Close();
    f_mount(NULL, SDPath, 0);
}
