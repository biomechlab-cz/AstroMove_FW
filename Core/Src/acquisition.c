#include "acquisition.h"
#include "ads1292.h"
#include "i2c_sensors.h"
#include "main.h"
#include "fatfs.h"
#include <stdio.h>
#include <string.h>

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
    MODIFY_REG(SDMMC1->CLKCR, SDMMC_CLKCR_CLKDIV, 50U);
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
 * Binary file format
 *
 * File header  — 32 bytes, written once at open:
 *   [0..3]   magic "AMS1"
 *   [4]      ADS ID register
 *   [5..13]  ADS registers: cfg1,cfg2,loff,ch1set,ch2set,rldsens,loffsens,resp1,resp2 (9 bytes)
 *   [14..19] RTC start time: sec,min,hr,date,mon,yr (BCD, from RV-3028-C7)
 *   [20..31] reserved 0x00
 *
 * Data block   — 5048 bytes, appended once per 1000-sample batch:
 *   [0..5]     RTC time at block flush: sec,min,hr,date,mon,yr (BCD)
 *   [6]        f_sync duration ms from previous sync (0 if no sync since last block)
 *   [7]        f_write duration ms from previous block (4× f_write combined, capped at 255)
 *   [8..47]    uint32_t tick_snap[10]  — HAL_GetTick() at each 100-sample boundary (LE)
 *   [48..4047] int32_t  ch1[1000]     — 24-bit ADC counts sign-extended to 32 bits (LE)
 *   [4048..5047] uint8_t loff[1000]   — raw ADS status byte 0 (lead-off flags in bits 6:4)
 *
 * f_sync every 10 blocks (~10 s) so data survives power loss without ACQ_Stop.
 *
 * Python:  python plot_ecg.py log_NNNN.bin
 * ==================================================================== */
#define ADS_BUF_LEN    1000
#define BLOCK_HDR_SIZE 32   /* file header, written once */
#define BLOCK_SIZE     5048 /* per-batch data block (8-byte RTC header + 5040 data) */

/* ---- sample buffers ---- */
static int32_t  s_stat[ADS_BUF_LEN];
static int32_t  s_ch1[ADS_BUF_LEN];
static int32_t  s_ch2[ADS_BUF_LEN];
static uint8_t  s_loff[ADS_BUF_LEN];
static uint32_t s_tick_snap[ADS_BUF_LEN / 100]; /* HAL_GetTick() at 100-sample boundaries */
static uint16_t s_count = 0;

/* ---- log file ---- */
static FATFS   s_fs;
static FIL     s_file;
static uint8_t s_file_open = 0;
static uint32_t s_fsync_last_ms  = 0;  /* duration of most recent f_sync call, ms */
static uint32_t s_fwrite_last_ms = 0;  /* duration of most recent 4× f_write calls, ms */

static void find_log_name(char *buf, size_t sz)
{
    for (uint16_t n = 0; n <= 9999; n++) {
        snprintf(buf, sz, "log_%04u.bin", n);
        FILINFO fi;
        if (f_stat(buf, &fi) != FR_OK) return;
    }
}

/* ====================================================================
 * ISR path
 * ==================================================================== */
volatile uint32_t g_isr_count = 0;
volatile uint8_t  g_drdy_flag = 0;

void ACQ_DRDY_Callback(void)
{
    g_isr_count++;
    g_drdy_flag = 1;
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
    if (!sd_init_4bit()) return 0;
    if (f_mount(&s_fs, SDPath, 1) != FR_OK) return 0;

    char name[16];
    find_log_name(name, sizeof(name));
    if (f_open(&s_file, name, FA_CREATE_NEW | FA_WRITE) != FR_OK) return 0;

    s_file_open = 1;
    return 1;
}

void ACQ_WriteDiagnostics(void)
{
    if (!s_file_open) return;

    uint8_t hdr[BLOCK_HDR_SIZE] = {0};
    hdr[0] = 'A'; hdr[1] = 'M'; hdr[2] = 'S'; hdr[3] = '1';
    hdr[4]  = ADS1292_ReadReg(0x00); /* ID */
    hdr[5]  = ADS1292_ReadReg(0x01); /* CONFIG1 */
    hdr[6]  = ADS1292_ReadReg(0x02); /* CONFIG2 */
    hdr[7]  = ADS1292_ReadReg(0x03); /* LOFF */
    hdr[8]  = ADS1292_ReadReg(0x04); /* CH1SET */
    hdr[9]  = ADS1292_ReadReg(0x05); /* CH2SET */
    hdr[10] = ADS1292_ReadReg(0x06); /* RLDSENS */
    hdr[11] = ADS1292_ReadReg(0x07); /* LOFFSENS */
    hdr[12] = ADS1292_ReadReg(0x09); /* RESP1 */
    hdr[13] = ADS1292_ReadReg(0x0A); /* RESP2 */
    RTC_Time_t t = {0};
    RV3028_ReadTime(&t); /* BCD; stays 0x00 on I2C failure */
    hdr[14] = t.sec; hdr[15] = t.min; hdr[16] = t.hr;
    hdr[17] = t.date; hdr[18] = t.mon; hdr[19] = t.yr;
    /* [20..31] remain 0x00 */

    UINT bw;
    f_write(&s_file, hdr, sizeof(hdr), &bw);
    f_sync(&s_file); /* sync once after header */
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
    if (!g_drdy_flag) return;
    g_drdy_flag = 0;

    uint8_t raw[9] = {0};
    ADS1292_ReadRaw(raw);

    // s_stat[s_count]  = ((int32_t)((uint32_t)raw[0] << 24 |
    //                               (uint32_t)raw[1] << 16 |
    //                               (uint32_t)raw[2] << 8)) >> 8;
    s_ch1[s_count]  = ((int32_t)((uint32_t)raw[3] << 24 |
                                  (uint32_t)raw[4] << 16 |
                                  (uint32_t)raw[5] << 8)) >> 8;
    // s_ch2[s_count]  = ((int32_t)((uint32_t)raw[6] << 24 |
    //                               (uint32_t)raw[7] << 16 |
    //                               (uint32_t)raw[8] << 8)) >> 8;
    s_loff[s_count] = raw[0];
    if (s_count % 100 == 0)
        s_tick_snap[s_count / 100] = HAL_GetTick();
    s_count++;

    if (s_count >= ADS_BUF_LEN) {
        /* 8-byte RTC block header, then three raw array writes.
           Total: 8 + 40 + 4000 + 1000 = 5048 bytes (~13 ms at 395 KB/s). */
        uint8_t blk_hdr[8] = {0};
        RTC_Time_t t = {0};
        RV3028_ReadTime(&t);
        blk_hdr[0] = t.sec; blk_hdr[1] = t.min; blk_hdr[2] = t.hr;
        blk_hdr[3] = t.date; blk_hdr[4] = t.mon; blk_hdr[5] = t.yr;
        blk_hdr[6] = (uint8_t)(s_fsync_last_ms  > 255 ? 255 : s_fsync_last_ms);  /* prev f_sync ms */
        blk_hdr[7] = (uint8_t)(s_fwrite_last_ms > 255 ? 255 : s_fwrite_last_ms); /* prev f_write ms */
        s_fsync_last_ms  = 0;
        s_fwrite_last_ms = 0;

        UINT bw;
        FRESULT fr;
        uint32_t tw0 = HAL_GetTick();
        fr  = f_write(&s_file, blk_hdr,     sizeof(blk_hdr),     &bw);
        fr |= f_write(&s_file, s_tick_snap, sizeof(s_tick_snap),  &bw);
        fr |= f_write(&s_file, s_ch1,       sizeof(s_ch1),        &bw);
        fr |= f_write(&s_file, s_loff,      sizeof(s_loff),       &bw);
        s_fwrite_last_ms = HAL_GetTick() - tw0;
        if (fr != FR_OK) blink_error(2);

        static uint8_t s_block_count = 0;
        if (++s_block_count >= 10) {
            uint32_t ts0 = HAL_GetTick();
            fr = f_sync(&s_file);
            s_fsync_last_ms = HAL_GetTick() - ts0;
            s_block_count = 0;
            if (fr != FR_OK) blink_error(3);
        }
        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_10);
        s_count = 0;
    }
}

void ACQ_Stop(void)
{
    if (s_file_open) {
        f_sync(&s_file); /* flush FatFS write-back cache and update directory entry */
        f_close(&s_file);
        s_file_open = 0;
    }
    f_mount(NULL, SDPath, 0);
}
