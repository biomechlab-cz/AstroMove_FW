#include "sd_card.h"
#include "fatfs.h"
#include "stm32l4xx_hal.h"

extern SD_HandleTypeDef hsd1;

uint8_t SD_Card_Init(void)
{
    if (HAL_SD_Init(&hsd1) != HAL_OK)   return 0;
    if (f_mount(&SDFatFS, SDPath, 1) != FR_OK) return 0;
    return 1;
}

uint8_t SD_Card_Write(const char *filename, const uint8_t *data, uint32_t len)
{
    FIL f;
    UINT bw;
    if (f_open(&f, filename, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return 0;
    f_write(&f, data, len, &bw);
    f_close(&f);
    return (bw == len) ? 1 : 0;
}

uint8_t SD_Card_Append(const char *filename, const uint8_t *data, uint32_t len)
{
    FIL f;
    UINT bw;
    if (f_open(&f, filename, FA_OPEN_APPEND | FA_WRITE) != FR_OK) return 0;
    f_write(&f, data, len, &bw);
    f_close(&f);
    return (bw == len) ? 1 : 0;
}

void SD_Card_Deinit(void)
{
    f_mount(NULL, SDPath, 0);
}
