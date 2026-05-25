#include "acquisition.h"
#include "ads1292.h"
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

/* ---- ADS 1-second buffer ---- */
#define ADS_BUF_LEN    1000
/* Worst-case LP line: "ecg ch1=-8388608i,loff=255i 4294967295\r\n" = 41 chars; 45 with margin */
#define LP_LINE_MAX    45

static int32_t  s_ch1[ADS_BUF_LEN];
static uint8_t  s_loff[ADS_BUF_LEN];
static uint32_t s_isr_snap[ADS_BUF_LEN];              /* g_isr_count at capture time */
static uint32_t s_tick_snap[ADS_BUF_LEN / 100];       /* HAL_GetTick() at each 100-sample boundary */
static char     s_wbuf[ADS_BUF_LEN * LP_LINE_MAX];    /* pre-formatted write buffer — single f_write */
static uint16_t s_count = 0;

/* ---- log file ---- */
static FATFS  s_fs;
static FIL    s_file;
static uint8_t s_file_open = 0;

static void find_log_name(char *buf, size_t sz)
{
    for (uint16_t n = 0; n <= 9999; n++) {
        snprintf(buf, sz, "log_%04u.lp", n);
        FILINFO fi;
        if (f_stat(buf, &fi) != FR_OK) return;
    }
}

/* ====================================================================
 * ISR path — DRDY falling edge sets g_drdy_flag; ACQ_Process reads it.
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

    UINT bw;
    char buf[192];
    int  len;

    /* Register readback */
    uint8_t id       = ADS1292_ReadReg(0x00);
    uint8_t config1  = ADS1292_ReadReg(0x01);
    uint8_t config2  = ADS1292_ReadReg(0x02);
    uint8_t loff     = ADS1292_ReadReg(0x03);
    uint8_t ch1set   = ADS1292_ReadReg(0x04);
    uint8_t ch2set   = ADS1292_ReadReg(0x05);
    uint8_t rldsens  = ADS1292_ReadReg(0x06);
    uint8_t loffsens = ADS1292_ReadReg(0x07);
    uint8_t resp1    = ADS1292_ReadReg(0x09);
    uint8_t resp2    = ADS1292_ReadReg(0x0A);

    len = snprintf(buf, sizeof(buf),
        "# ID=%02X CONFIG1=%02X CONFIG2=%02X LOFF=%02X"
        " CH1SET=%02X CH2SET=%02X RLDSENS=%02X LOFFSENS=%02X"
        " RESP1=%02X RESP2=%02X\r\n",
        id, config1, config2, loff,
        ch1set, ch2set, rldsens, loffsens,
        resp1, resp2);
    f_write(&s_file, buf, (UINT)len, &bw);
    f_sync(&s_file);  /* flush now so register data is on disk even if the next steps hang */

    /* DRDY check in SDATAC mode — START pin is tied high so ADS should be converting.
       If drdy=0 here, the ADS is not outputting DRDY despite being configured correctly. */
    uint32_t t0 = HAL_GetTick();
    uint8_t  drdy = 0;
    while (HAL_GetTick() - t0 < 100) {
        if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_0) == GPIO_PIN_RESET) {
            drdy = 1;
            break;
        }
    }

    /* Force-read one sample with RDATA command — works regardless of DRDY state.
       Status word bytes [0-2] should start with 0xC... (0b1100xxxx). */
    uint8_t rdata[9] = {0};
    ADS1292_ReadRDATA(rdata);

    len = snprintf(buf, sizeof(buf),
        "# DRDY_SDATAC=%u"
        " RDATA=%02X%02X%02X %02X%02X%02X %02X%02X%02X\r\n",
        drdy,
        rdata[0], rdata[1], rdata[2],
        rdata[3], rdata[4], rdata[5],
        rdata[6], rdata[7], rdata[8]);
    f_write(&s_file, buf, (UINT)len, &bw);
    f_sync(&s_file);
}

void ACQ_Process(void)
{
    if (s_count >= ADS_BUF_LEN) return;
    if (!g_drdy_flag) return;
    g_drdy_flag = 0;

    uint8_t raw[9] = {0};
    ADS1292_ReadRaw(raw);

    s_ch1[s_count]      = ((int32_t)((uint32_t)raw[3] << 24 |
                                     (uint32_t)raw[4] << 16 |
                                     (uint32_t)raw[5] << 8)) >> 8;
    s_loff[s_count]     = raw[0];
    s_isr_snap[s_count] = g_isr_count;
    if (s_count % 100 == 0)
        s_tick_snap[s_count / 100] = HAL_GetTick();
    s_count++;

    if (s_count >= ADS_BUF_LEN) {
        /* Format all lines into s_wbuf first, then one f_write — eliminates per-call FatFS overhead. */
        uint32_t pos = 0;
        for (uint16_t i = 0; i < ADS_BUF_LEN; i++) {
            uint32_t ts_ms = s_tick_snap[i / 100] + (i % 100);
            pos += (uint32_t)snprintf(s_wbuf + pos, sizeof(s_wbuf) - pos,
                                      "ecg ch1=%ldi,loff=%ui %lu\r\n",
                                      (long)s_ch1[i], (unsigned)s_loff[i],
                                      (unsigned long)ts_ms);
        }
        UINT bw;
        f_write(&s_file, s_wbuf, pos, &bw);
        f_sync(&s_file);

        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_10);
        HAL_Delay(50);
        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_10);

        s_count = 0;
    }
}

void ACQ_Stop(void)
{
    if (s_file_open) {
        f_close(&s_file);
        s_file_open = 0;
    }
    f_mount(NULL, SDPath, 0);
}
