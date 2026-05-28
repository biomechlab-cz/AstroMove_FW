#include "sd_dma.h"
#include "main.h"
#include "cmsis_os.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>

extern SD_HandleTypeDef    hsd1;
extern DMA_HandleTypeDef   hdma_sdmmc1_tx;
extern DMA_HandleTypeDef   hdma_sdmmc1_rx;

/* ------------------------------------------------------------------ */
/*  LED blink helpers  (PB10)                                          */
/* ------------------------------------------------------------------ */
static void fail_blink(uint32_t n)
{
    for (;;) {
        for (uint32_t i = 0; i < n; i++) {
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_SET);
            osDelay(150);
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
            osDelay(150);
        }
        osDelay(1000);
    }
}

static void pass_blink(void)
{
    for (;;) {
        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_10);
        osDelay(500);
    }
}

/* ------------------------------------------------------------------ */
/*  GPIO: set SDMMC1 pins to VERY_HIGH + PULLUP after HAL_SD_Init      */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/*  SD_DMA_Init                                                         */
/* ------------------------------------------------------------------ */
HAL_StatusTypeDef SD_DMA_Init(void)
{
    hsd1.Init.ClockDiv = 10;
    hsd1.Init.BusWide  = SDMMC_BUS_WIDE_1B;
    if (HAL_SD_Init(&hsd1) != HAL_OK) return HAL_ERROR;

    sdmmc_gpio_very_high();

    if (!sd_wait_transfer(1000)) return HAL_TIMEOUT;
    if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK) return HAL_ERROR;

    MODIFY_REG(SDMMC1->CLKCR, SDMMC_CLKCR_CLKDIV, 50U);  /* 480 kHz */
    HAL_Delay(20);

    return sd_wait_transfer(500) ? HAL_OK : HAL_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/*  DMA completion wait                                                  */
/* ------------------------------------------------------------------ */
static HAL_StatusTypeDef wait_done(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while (HAL_SD_GetState(&hsd1) == HAL_SD_STATE_BUSY) {
        if (HAL_GetTick() - t0 > timeout_ms) return HAL_TIMEOUT;
        osDelay(1);
    }
    if (HAL_SD_GetState(&hsd1) != HAL_SD_STATE_READY) return HAL_ERROR;
    while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER) {
        if (HAL_GetTick() - t0 > timeout_ms) return HAL_TIMEOUT;
        osDelay(1);
    }
    return HAL_OK;
}

/* ------------------------------------------------------------------ */
/*  DMA channel reset (ST TECH040009 workaround)                        */
/* ------------------------------------------------------------------ */
static void dma_reset_for_write(void)
{
    HAL_DMA_Abort(&hdma_sdmmc1_tx);
    HAL_DMA_Abort(&hdma_sdmmc1_rx);
    HAL_DMA_DeInit(&hdma_sdmmc1_tx);
    HAL_DMA_DeInit(&hdma_sdmmc1_rx);
    HAL_DMA_Init(&hdma_sdmmc1_tx);
}

static void dma_reset_for_read(void)
{
    HAL_DMA_Abort(&hdma_sdmmc1_tx);
    HAL_DMA_Abort(&hdma_sdmmc1_rx);
    HAL_DMA_DeInit(&hdma_sdmmc1_tx);
    HAL_DMA_DeInit(&hdma_sdmmc1_rx);
    HAL_DMA_Init(&hdma_sdmmc1_rx);
}

HAL_StatusTypeDef SD_DMA_Write(uint32_t sector_addr, const uint8_t *buf, uint32_t count)
{
    if (!sd_wait_transfer(1000)) return HAL_TIMEOUT;
    SDMMC1->MASK = 0;
    SDMMC1->ICR  = 0x1FEFFFFFU;
    dma_reset_for_write();
    HAL_StatusTypeDef ret = HAL_SD_WriteBlocks_DMA(&hsd1, (uint8_t *)buf, sector_addr, count);
    if (ret != HAL_OK) return ret;
    return wait_done(5000);
}

HAL_StatusTypeDef SD_DMA_Read(uint32_t sector_addr, uint8_t *buf, uint32_t count)
{
    dma_reset_for_read();
    HAL_StatusTypeDef ret = HAL_SD_ReadBlocks_DMA(&hsd1, buf, sector_addr, count);
    if (ret != HAL_OK) return ret;
    return wait_done(5000);
}

/* ------------------------------------------------------------------ */
/*  SD_DMA_Test  — FreeRTOS task                                        */
/*  Tests FatFS write path: f_mount → f_open → f_write → f_close.     */
/*  disk_write is routed through SD_DMA_Write (ST TECH040009 fix).     */
/*                                                                      */
/*  Blink codes:                                                        */
/*   1  SD_DMA_Init failed                                              */
/*   2  f_mount failed (format card FAT32, ≥8 KB clusters)            */
/*   3  f_open  failed                                                  */
/*   4  f_write failed                                                  */
/*   5  f_close failed                                                  */
/*   slow 500 ms blink = PASS                                           */
/* ------------------------------------------------------------------ */

#define FILE_SECTORS 16u
#define FILE_BYTES   (FILE_SECTORS * 512u)  /* 8 KB — kept for raw DMA ref below */

/* FAT32 directory entry — 32 bytes, packed */
typedef struct __attribute__((packed)) {
    uint8_t  name[11];      /* 0x00: 8.3 name, space-padded, upper-case    */
    uint8_t  attr;          /* 0x0B: 0x20 = ARCHIVE                         */
    uint8_t  nt;            /* 0x0C: reserved (NTRes), 0x00                 */
    uint8_t  crt_ts;        /* 0x0D: creation time, tenths of second        */
    uint16_t crt_time;      /* 0x0E */
    uint16_t crt_date;      /* 0x10 */
    uint16_t lst_acc_date;  /* 0x12: last-access date (required for 32 B!)  */
    uint16_t clus_hi;       /* 0x14: high 16 bits of first cluster          */
    uint16_t wrt_time;      /* 0x16 */
    uint16_t wrt_date;      /* 0x18 */
    uint16_t clus_lo;       /* 0x1A: low 16 bits of first cluster           */
    uint32_t size;          /* 0x1C: file size in bytes                     */
} FAT32DirEnt;

static FATFS   s_fs;
static uint8_t s_scratch[512]       __attribute__((aligned(4)));  /* 1-sector work buffer */
static uint8_t s_buf_a[FILE_BYTES]  __attribute__((aligned(4)));
static uint8_t s_buf_b[FILE_BYTES]  __attribute__((aligned(4)));
static uint8_t s_rx[FILE_BYTES]     __attribute__((aligned(4)));

void SD_DMA_Test(void *arg)
{
    (void)arg;
    osDelay(300);

    if (SD_DMA_Init() != HAL_OK)
        fail_blink(1);

    if (f_mount(&s_fs, "0:", 1) != FR_OK)
        fail_blink(2);

    {
        FIL     fil;
        UINT    bw;
        FRESULT fr;

        fr = f_open(&fil, "FATFS.TXT", FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK) fail_blink(3);

        static const char test_str[] =
            "AstroMowe FatFS write test\r\n"
            "disk_write via SD_DMA_Write: OK\r\n";
        fr = f_write(&fil, test_str, sizeof(test_str) - 1, &bw);
        if (fr != FR_OK || bw != sizeof(test_str) - 1) fail_blink(4);

        fr = f_close(&fil);
        if (fr != FR_OK) fail_blink(5);
    }

    f_mount(NULL, "0:", 0);
    pass_blink();
}

/* ------------------------------------------------------------------ */
/*  Raw-DMA file creation (DMA_A.TXT + DMA_B.TXT written by           */
/*  manipulating FAT/dir sectors directly, bypassing FatFS).           */
/*  Kept for reference; not compiled in normal builds.                 */
/* ------------------------------------------------------------------ */
#if 0
static void sd_raw_dma_test(void)
{
    if (SD_DMA_Init() != HAL_OK) fail_blink(1);
    if (f_mount(&s_fs, "0:", 1) != FR_OK) fail_blink(2);
    if ((uint32_t)s_fs.csize < FILE_SECTORS) fail_blink(13);

    uint32_t fatbase  = s_fs.fatbase;
    uint32_t fsize    = s_fs.fsize;
    uint8_t  n_fats   = s_fs.n_fats;
    uint32_t dirbase  = s_fs.dirbase;
    uint32_t database = s_fs.database;
    uint16_t csize    = s_fs.csize;
    f_mount(NULL, "0:", 0);

    if (HAL_SD_ReadBlocks(&hsd1, s_scratch, fatbase, 1, 5000) != HAL_OK) fail_blink(3);

    uint32_t *fat = (uint32_t *)s_scratch;
    uint32_t cl_a = 0, cl_b = 0;
    for (uint32_t i = dirbase + 1; i < 128 && cl_b == 0; i++) {
        if ((fat[i] & 0x0FFFFFFFU) == 0) {
            if (cl_a == 0) { cl_a = i; fat[i] = 0x0FFFFFFFU; }
            else           { cl_b = i; fat[i] = 0x0FFFFFFFU; }
        }
    }
    if (cl_a == 0 || cl_b == 0) fail_blink(4);

    if (SD_DMA_Write(fatbase, s_scratch, 1) != HAL_OK) fail_blink(5);
    if (n_fats == 2 && SD_DMA_Write(fatbase + fsize, s_scratch, 1) != HAL_OK) fail_blink(5);

    uint32_t root_lba = database + ((dirbase - 2U) * csize);
    if (HAL_SD_ReadBlocks(&hsd1, s_scratch, root_lba, 1, 5000) != HAL_OK) fail_blink(6);

    FAT32DirEnt *dir = (FAT32DirEnt *)s_scratch;
    int slot_a = -1, slot_b = -1;
    for (int i = 0; i < 16 && slot_b < 0; i++) {
        uint8_t first = dir[i].name[0];
        if (first == 0x00 || first == 0xE5) {
            if (slot_a < 0) slot_a = i; else slot_b = i;
        }
    }
    if (slot_a < 0 || slot_b < 0) fail_blink(7);

    const uint16_t fat_date = 0x5CBCu;
    memset(&dir[slot_a], 0, 32);
    memcpy(dir[slot_a].name, "DMA_A   TXT", 11);
    dir[slot_a].attr = 0x20; dir[slot_a].crt_date = fat_date;
    dir[slot_a].lst_acc_date = fat_date; dir[slot_a].clus_hi = (uint16_t)(cl_a >> 16);
    dir[slot_a].wrt_date = fat_date; dir[slot_a].clus_lo = (uint16_t)(cl_a & 0xFFFFU);
    dir[slot_a].size = FILE_BYTES;

    memset(&dir[slot_b], 0, 32);
    memcpy(dir[slot_b].name, "DMA_B   TXT", 11);
    dir[slot_b].attr = 0x20; dir[slot_b].crt_date = fat_date;
    dir[slot_b].lst_acc_date = fat_date; dir[slot_b].clus_hi = (uint16_t)(cl_b >> 16);
    dir[slot_b].wrt_date = fat_date; dir[slot_b].clus_lo = (uint16_t)(cl_b & 0xFFFFU);
    dir[slot_b].size = FILE_BYTES;

    if (SD_DMA_Write(root_lba, s_scratch, 1) != HAL_OK) fail_blink(8);

    uint32_t sector_a = database + ((cl_a - 2U) * csize);
    uint32_t sector_b = database + ((cl_b - 2U) * csize);

    memset(s_buf_a, 'A', FILE_BYTES);
    memset(s_buf_b, 'B', FILE_BYTES);
    snprintf((char *)s_buf_a, 128, "DMA_A cl=%lu sec=%lu\r\n", (unsigned long)cl_a, (unsigned long)sector_a);
    snprintf((char *)s_buf_b, 128, "DMA_B cl=%lu sec=%lu\r\n", (unsigned long)cl_b, (unsigned long)sector_b);

    if (SD_DMA_Write(sector_a, s_buf_a, FILE_SECTORS) != HAL_OK) fail_blink(9);
    if (SD_DMA_Write(sector_b, s_buf_b, FILE_SECTORS) != HAL_OK) fail_blink(10);

    memset(s_rx, 0xCC, FILE_BYTES);
    if (SD_DMA_Read(sector_a, s_rx, FILE_SECTORS) != HAL_OK) fail_blink(11);
    if (memcmp(s_buf_a, s_rx, FILE_BYTES) != 0) fail_blink(12);

    memset(s_rx, 0xCC, FILE_BYTES);
    if (SD_DMA_Read(sector_b, s_rx, FILE_SECTORS) != HAL_OK) fail_blink(14);
    if (memcmp(s_buf_b, s_rx, FILE_BYTES) != 0) fail_blink(15);

    pass_blink();
}
#endif
