#include "acquisition.h"
#include "ads1292.h"
#include "i2c_sensors.h"
#include "sd_dma.h"
#include "main.h"
#include "fatfs.h"
#include "cmsis_os.h"
#include <stdio.h>
#include <string.h>

extern SPI_HandleTypeDef hspi1;

/* ====================================================================
 * Text file format (CSV, for development / debugging)
 *
 * Header line (written once):
 *   tick_ms,ecg,loff
 *
 * One line per ADS sample (1 kSPS):
 *   <tick_ms>,<ecg_signed_decimal>,<loff_hex>\r\n
 *   Example: 1234567,-23456,00\r\n   (~25 chars per line)
 *
 * f_sync every 10 × 1000-sample blocks (~10 s).
 * LED: single toggle per 1000-sample block; 3 rapid blinks on sync.
 * ==================================================================== */
#define ADS_BUF_LEN 1000

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
static char    s_log_name[16];

static void find_log_name(char *buf, size_t sz)
{
    for (uint16_t n = 0; n <= 9999; n++) {
        snprintf(buf, sz, "log_%04u.txt", n);
        FILINFO fi;
        if (f_stat(buf, &fi) != FR_OK) return;
    }
}

/* Open the next available log file and write the CSV column header.
 * Called from SDWriteTask for each new 1000-sample block. */
static uint8_t open_next_log(void)
{
    find_log_name(s_log_name, sizeof(s_log_name));
    if (f_open(&s_file, s_log_name, FA_CREATE_NEW | FA_WRITE) != FR_OK) return 0;
    s_file_open = 1;
    return 1;
}

/* Write a minimal CSV column header — used by SDWriteTask when opening
 * subsequent files (ADS register read not safe from SDWriteTask context). */
static void write_csv_header(void)
{
    UINT bw;
    static const char hdr[] = "tick_ms,ecg,loff\r\n";
    f_write(&s_file, hdr, sizeof(hdr) - 1, &bw);
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
    if (SD_DMA_Init() != HAL_OK) return 0;
    if (f_mount(&s_fs, SDPath, 1) != FR_OK) return 0;

    if (!open_next_log()) return 0;
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

    char     buf[256];
    UINT     bw;
    RTC_Time_t t = {0};
    RV3028_ReadTime(&t);

    int n = snprintf(buf, sizeof(buf),
        "# AstroMowe ECG  ID=%02X CFG1=%02X CFG2=%02X LOFF=%02X"
        " CH1=%02X CH2=%02X RESP1=%02X RESP2=%02X\r\n"
        "# RTC start: %02X-%02X-%02X %02X:%02X:%02X\r\n"
        "tick_ms,ecg,loff\r\n",
        ADS1292_ReadReg(0x00), ADS1292_ReadReg(0x01),
        ADS1292_ReadReg(0x02), ADS1292_ReadReg(0x03),
        ADS1292_ReadReg(0x04), ADS1292_ReadReg(0x05),
        ADS1292_ReadReg(0x09), ADS1292_ReadReg(0x0A),
        t.yr, t.mon, t.date, t.hr, t.min, t.sec);
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1; /* guard truncation */
    f_write(&s_file, buf, (UINT)n, &bw);
    /* no f_sync here — let the data flush naturally via the write buffer */
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
 * Drains the sample queue and writes one CSV line per sample.
 * FatFS buffers internally (fil->buf[512]) so disk_write is called
 * roughly every 20 samples (~512 / 25 bytes/line).
 * AcqTask keeps sampling while f_write runs here.
 * ==================================================================== */
void SDWriteTask(void *arg)
{
    uint32_t count = 0;

    for (;;) {
        Sample_t s;
        osMessageQueueGet(s_queue, &s, NULL, osWaitForever);

        char  line[32];
        UINT  bw;
        int   len = snprintf(line, sizeof(line), "%lu,%ld,%02X\r\n",
                             (unsigned long)s.tick, (long)s.ch1, (unsigned)s.loff);
        FRESULT fr = f_write(&s_file, line, (UINT)len, &bw);
        if (fr != FR_OK || bw != (UINT)len) {
            for (;;) { HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_10); osDelay(50); }
        }

        if (++count >= ADS_BUF_LEN) {
            count = 0;
            HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_10);

            FRESULT fr_sync = f_sync(&s_file);
            if (fr_sync != FR_OK) {
                /* f_sync failed — 300 ms blink distinct from f_write 50 ms */
                for (;;) { HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_10); osDelay(300); }
            }
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
    if (SD_DMA_Init() != HAL_OK) return 1;
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
