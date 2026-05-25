#include "sd_4bit_test.h"
#include "main.h"
#include "fatfs.h"
#include <string.h>
#include <stdio.h>

extern SD_HandleTypeDef hsd1;

static uint8_t wr_buf[64 * 1024];

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

static int sd_wait_transfer(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER) {
        if (HAL_GetTick() - t0 > timeout_ms) return 0;
        HAL_Delay(1);
    }
    return 1;
}

static void sdmmc_full_reset(void)
{
    HAL_SD_DeInit(&hsd1);
    __HAL_RCC_SDMMC1_FORCE_RESET();
    HAL_Delay(10);
    __HAL_RCC_SDMMC1_RELEASE_RESET();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_11, GPIO_PIN_RESET);
    HAL_Delay(500);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_11, GPIO_PIN_SET);
    HAL_Delay(500);
}

/* Try 4-bit at a given ClockDiv. Returns write KB/s, or 0 on failure. */
static uint32_t try_4bit(uint8_t clkdiv, char *status, size_t status_sz)
{
    FATFS fs; FIL f; UINT written; FRESULT fr;
    uint32_t kbs = 0;

    /* fresh init in 1-bit */
    hsd1.Init.ClockDiv = 10;
    hsd1.Init.BusWide  = SDMMC_BUS_WIDE_1B;
    if (HAL_SD_Init(&hsd1) != HAL_OK) {
        snprintf(status, status_sz, "init-fail");
        return 0;
    }
    sdmmc_gpio_very_high();
    if (!sd_wait_transfer(1000)) {
        HAL_SD_DeInit(&hsd1);
        snprintf(status, status_sz, "card-not-ready");
        return 0;
    }

    /* switch to 4-bit */
    if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK) {
        HAL_SD_DeInit(&hsd1);
        snprintf(status, status_sz, "acmd6-fail");
        return 0;
    }

    /* set target clock speed */
    MODIFY_REG(SDMMC1->CLKCR, SDMMC_CLKCR_CLKDIV, (uint32_t)clkdiv);
    HAL_Delay(20);

    if (!sd_wait_transfer(500)) {
        sdmmc_full_reset();
        snprintf(status, status_sz, "state-fail");
        return 0;
    }

    fr = f_mount(&fs, SDPath, 1);
    if (fr != FR_OK) {
        sdmmc_full_reset();
        snprintf(status, status_sz, "mount-fail(%d)", (int)fr);
        return 0;
    }

    memset(wr_buf, 0xA5, sizeof(wr_buf));
    fr = f_open(&f, "test64k.bin", FA_CREATE_ALWAYS | FA_WRITE);
    if (fr == FR_OK) {
        uint32_t t0 = HAL_GetTick();
        f_write(&f, wr_buf, sizeof(wr_buf), &written);
        f_sync(&f);
        uint32_t elapsed = HAL_GetTick() - t0;
        f_close(&f);
        if (elapsed > 0)
            kbs = (64UL * 1000UL) / elapsed;
    }

    f_mount(NULL, SDPath, 0);
    sdmmc_full_reset();

    snprintf(status, status_sz, "OK");
    return kbs;
}

void SD_Test4Bit(void)
{
    /* ClockDiv → SDMMC_CK:  50→480kHz  10→2.4MHz  4→6MHz  2→12MHz  1→24MHz */
    static const struct { uint8_t div; const char *label; } speeds[] = {
        { 50, "480kHz" },
        { 10, "2.4MHz" },
        {  4, "6MHz"   },
        {  2, "12MHz"  },
        {  1, "24MHz"  },
    };
    const int N = sizeof(speeds) / sizeof(speeds[0]);

    char result[512];
    char status[32];
    int  pos = 0;

    pos += snprintf(result + pos, sizeof(result) - pos,
        "4-bit clock sweep (4-bit mode, VERY_HIGH GPIO)\r\n"
        "----------------------------------------\r\n");

    for (int i = 0; i < N; i++) {
        uint32_t kbs = try_4bit(speeds[i].div, status, sizeof(status));
        pos += snprintf(result + pos, sizeof(result) - pos,
            "%s (div=%d): %s  %lu KB/s\r\n",
            speeds[i].label, speeds[i].div, status, kbs);
        HAL_Delay(200);
    }

    /* Write result — need a working init for the file write */
    hsd1.Init.ClockDiv = 10;
    hsd1.Init.BusWide  = SDMMC_BUS_WIDE_1B;
    if (HAL_SD_Init(&hsd1) != HAL_OK) return;
    sdmmc_gpio_very_high();
    if (!sd_wait_transfer(1000)) return;

    FATFS fs; FIL f; UINT bw;
    if (f_mount(&fs, SDPath, 1) != FR_OK) return;
    if (f_open(&f, "result.txt", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        f_write(&f, result, strlen(result), &bw);
        f_close(&f);
    }
    f_mount(NULL, SDPath, 0);
}
