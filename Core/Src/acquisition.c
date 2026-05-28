#include "acquisition.h"
#include "ads1292.h"
#include "i2c_sensors.h"
#include "main.h"
#include "fatfs.h"
#include "cmsis_os.h"
#include <stdio.h>
#include <string.h>

/* ---- SD init (validated 4-bit 480 kHz) ---- */
extern SD_HandleTypeDef hsd1;
extern SPI_HandleTypeDef hspi1;

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
    MODIFY_REG(SDMMC1->CLKCR, SDMMC_CLKCR_CLKDIV, 100U);
    HAL_Delay(20);
    return sd_wait_transfer(500);
}

/* ====================================================================
 * Binary file format
 *
 * File header  — 32 bytes, written once at open:
 *   [0..3]   magic "AMS1"
 *   [4]      ADS ID register
 *   [5..13]  ADS registers: cfg1,cfg2,loff,ch1set,ch2set,rldsens,loffsens,resp1,resp2
 *   [14..19] RTC start time: sec,min,hr,date,mon,yr (BCD, from RV-3028-C7)
 *   [20..31] reserved 0x00
 *
 * Data block   — 5048 bytes, appended once per 1000-sample batch:
 *   [0..5]       RTC time at block flush: sec,min,hr,date,mon,yr (BCD)
 *   [6..7]       reserved 0x00
 *   [8..47]      uint32_t tick_snap[10]  — HAL_GetTick() at 100-sample boundaries (LE)
 *   [48..4047]   int32_t  ch1[1000]     — 24-bit ADC counts sign-extended to 32 bits (LE)
 *   [4048..5047] uint8_t  loff[1000]    — raw ADS status byte 0
 *
 * f_sync every 10 blocks (~10 s); 3-blink LED signals safe-to-remove.
 *
 * Python:  python plot_ecg.py log_NNNN.bin
 * ==================================================================== */
#define ADS_BUF_LEN    1000
#define BLOCK_HDR_SIZE 32
#define BLOCK_SIZE     5048

/* ---- RTOS objects ---- */
static osSemaphoreId_t    s_drdy_sem;
static osMessageQueueId_t s_queue;

typedef struct {
    int32_t  ch1;
    uint32_t tick;
    uint8_t  loff;
    uint8_t  _pad[3];
} Sample_t;

/* ---- log file ---- */
static FATFS   s_fs;
static FIL     s_file;
static uint8_t s_file_open = 0;

static void find_log_name(char *buf, size_t sz)
{
    for (uint16_t n = 0; n <= 9999; n++) {
        snprintf(buf, sz, "log_%04u.bin", n);
        FILINFO fi;
        if (f_stat(buf, &fi) != FR_OK) return;
    }
}

/* ====================================================================
 * ISR path  (EXTI0, priority 6 — within FreeRTOS API window)
 * ==================================================================== */
volatile uint32_t g_isr_count = 0;

void ACQ_DRDY_Callback(void)
{
    g_isr_count++;
    osSemaphoreRelease(s_drdy_sem);
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

void ACQ_CreateRtosObjects(void)
{
    s_drdy_sem = osSemaphoreNew(1, 0, NULL);            /* binary, initially 0 */
    s_queue    = osMessageQueueNew(200, sizeof(Sample_t), NULL);
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
    RV3028_ReadTime(&t);
    hdr[14] = t.sec; hdr[15] = t.min; hdr[16] = t.hr;
    hdr[17] = t.date; hdr[18] = t.mon; hdr[19] = t.yr;

    UINT bw;
    f_write(&s_file, hdr, sizeof(hdr), &bw);
    f_sync(&s_file);
}

/* ====================================================================
 * AcqTask — high priority
 * Handles startup sequence, then samples on every DRDY semaphore.
 * ==================================================================== */
void AcqTask(void *arg)
{
    /* SD init runs here — scheduler is fully active, so FatFS mutex/queue creation works */
    if (!ACQ_Init()) {
        for (;;) {
            HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_10);
            osDelay(100);
        }
    }

    /* Init ADS here (not in main.c) — keeps SPI init and reads in the same context */
    ADS1292_Init(&hspi1);

    /* Diagnostics header requires ADS in SDATAC (already set by ADS1292_Init above) */
    ACQ_WriteDiagnostics();

    /* 1-second startup flash using osDelay so SDWriteTask can run if needed */
    for (int i = 0; i < 10; i++) {
        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_10);
        osDelay(100);
    }
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);

    /* Enable DRDY interrupt now that semaphore is ready, then start streaming */
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);
    ADS1292_StartContinuous();

    for (;;) {
        osSemaphoreAcquire(s_drdy_sem, osWaitForever);

        uint8_t raw[9] = {0};
        ADS1292_ReadRaw(raw);

        Sample_t s;
        s.ch1  = ((int32_t)((uint32_t)raw[3] << 24 |
                             (uint32_t)raw[4] << 16 |
                             (uint32_t)raw[5] << 8)) >> 8;
        s.loff = raw[0];
        s.tick = HAL_GetTick();
        osMessageQueuePut(s_queue, &s, 0, 0); /* non-blocking; queue has 100-sample headroom */
    }
}

/* ====================================================================
 * SDWriteTask — normal priority
 * Drains the queue, accumulates 1000-sample blocks, writes to SD.
 * AcqTask keeps sampling while f_write runs here.
 * ==================================================================== */
void SDWriteTask(void *arg)
{
    static int32_t  ch1[ADS_BUF_LEN];
    static uint8_t  loff_buf[ADS_BUF_LEN];
    static uint32_t tick_snap[ADS_BUF_LEN / 100];
    uint16_t count       = 0;
    uint8_t  block_count = 0;

    for (;;) {
        Sample_t s;
        osMessageQueueGet(s_queue, &s, NULL, osWaitForever);

        ch1[count]      = s.ch1;
        loff_buf[count] = s.loff;
        if (count % 100 == 0)
            tick_snap[count / 100] = s.tick;
        count++;

        if (count >= ADS_BUF_LEN) {
            uint8_t blk_hdr[8] = {0};
            RTC_Time_t t = {0};
            RV3028_ReadTime(&t);
            blk_hdr[0] = t.sec; blk_hdr[1] = t.min; blk_hdr[2] = t.hr;
            blk_hdr[3] = t.date; blk_hdr[4] = t.mon; blk_hdr[5] = t.yr;

            UINT bw;
            FRESULT fr;
            fr  = f_write(&s_file, blk_hdr,   sizeof(blk_hdr),   &bw);
            fr |= f_write(&s_file, tick_snap,  sizeof(tick_snap),  &bw);
            fr |= f_write(&s_file, ch1,        sizeof(ch1),        &bw);
            fr |= f_write(&s_file, loff_buf,   sizeof(loff_buf),   &bw);
            if (fr != FR_OK) {
                /* SD write failed — rapid blink to signal error */
                for (;;) {
                    HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_10);
                    osDelay(50);
                }
            }

            if (++block_count >= 10) {
                f_sync(&s_file);
                block_count = 0;
                /* 3 rapid blinks = "synced, safe to remove SD card"
                   osDelay yields CPU — AcqTask keeps sampling during blink */
                for (int i = 0; i < 6; i++) {
                    HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_10);
                    osDelay(50);
                }
            } else {
                HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_10);
            }
            count = 0;
        }
    }
}

void ACQ_Stop(void)
{
    if (s_file_open) {
        f_sync(&s_file);
        f_close(&s_file);
        s_file_open = 0;
    }
    f_mount(NULL, SDPath, 0);
}

/* ====================================================================
 * ACQ_SD_WriteTest
 * Standalone DMA write test — run from a FreeRTOS task (scheduler must
 * be running so the sd_diskio DMA completion queue works).
 * Writes 10 lines of ASCII text to test.txt on the SD card.
 * Returns 1 on full success, 0 on any failure.
 * ==================================================================== */
/* ACQ_SD_WriteTest — full DMA+FatFS test.
 * Returns 0 = pass, or error stage (1-6) blinkable in StartDefaultTask:
 *   1 = sd_init_4bit failed
 *   2 = f_mount failed
 *   3 = f_open failed
 *   4 = f_write failed (FR_DISK_ERR means DMA write broken)
 *   5 = (reserved)
 *   6 = (reserved) */
uint8_t ACQ_SD_WriteTest(void)
{
    if (!sd_init_4bit()) return 1;
    if (f_mount(&s_fs, SDPath, 1) != FR_OK) return 2;

    static FIL test_f;
    UINT bw;
    FRESULT fr;

    fr = f_open(&test_f, "test.txt", FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) { f_mount(NULL, SDPath, 0); return 3; }

    static const char line[] = "AstroMowe SD DMA write test OK\r\n";
    for (int i = 0; i < 10; i++) {
        fr = f_write(&test_f, line, sizeof(line) - 1, &bw);
        if (fr != FR_OK || bw != sizeof(line) - 1) { f_close(&test_f); f_mount(NULL, SDPath, 0); return 4; }
    }

    f_sync(&test_f);
    f_close(&test_f);
    f_mount(NULL, SDPath, 0);
    return 0;
}
