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
/*                                                                      */
/*  Creates DMA_A.TXT and DMA_B.TXT by directly writing FAT and        */
/*  directory structures via raw DMA — no f_write used at all.         */
/*                                                                      */
/*  Flow:                                                               */
/*   1. SD_DMA_Init + f_mount (read-only) to read filesystem geometry  */
/*   2. HAL_SD_ReadBlocks → read FAT sector 0, claim two free clusters */
/*   3. SD_DMA_Write → write back FAT1 (and FAT2 if present)          */
/*   4. HAL_SD_ReadBlocks → read root dir sector 0, inject dir entries */
/*   5. SD_DMA_Write → write back root dir sector                      */
/*   6. SD_DMA_Write → write 8 KB file data to each cluster            */
/*   7. SD_DMA_Read + memcmp → verify both files                       */
/*                                                                      */
/*  Blink codes:                                                        */
/*   1  SD_DMA_Init                                                     */
/*   2  f_mount  (format card: FAT32, >= 8 KB cluster size)            */
/*   3  HAL_SD_ReadBlocks FAT sector failed                             */
/*   4  No free clusters found in FAT sector 0 (card not empty?)       */
/*   5  SD_DMA_Write FAT sector failed                                  */
/*   6  HAL_SD_ReadBlocks root dir sector failed                        */
/*   7  No free directory slots in root dir sector 0 (>14 existing)    */
/*   8  SD_DMA_Write root dir sector failed                             */
/*   9  SD_DMA_Write file A data failed                                 */
/*  10  SD_DMA_Write file B data failed                                 */
/*  11  SD_DMA_Read file A failed                                       */
/*  12  memcmp file A failed                                            */
/*  13  cluster too small (reformat with >= 8 KB cluster size)         */
/*  14  SD_DMA_Read file B failed                                       */
/*  15  memcmp file B failed                                            */
/*  slow 500 ms blink = all pass                                        */
/* ------------------------------------------------------------------ */

#define FILE_SECTORS 16u
#define FILE_BYTES   (FILE_SECTORS * 512u)  /* 8 KB */

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

    /* -------------------------------------------------------------- */
    /*  Step 1 — SD hardware init                                      */
    /* -------------------------------------------------------------- */
    if (SD_DMA_Init() != HAL_OK)
        fail_blink(1);

    /* -------------------------------------------------------------- */
    /*  Step 2 — f_mount (read-only: reads BPB to populate s_fs)      */
    /*  disk_write is never called by f_mount alone.                   */
    /* -------------------------------------------------------------- */
    if (f_mount(&s_fs, "0:", 1) != FR_OK)
        fail_blink(2);

    if ((uint32_t)s_fs.csize < FILE_SECTORS)
        fail_blink(13);

    /* Save geometry; unmount so FatFS is out of the way */
    uint32_t fatbase  = s_fs.fatbase;
    uint32_t fsize    = s_fs.fsize;   /* sectors per FAT */
    uint8_t  n_fats   = s_fs.n_fats;
    uint32_t dirbase  = s_fs.dirbase; /* root dir start cluster (FAT32) */
    uint32_t database = s_fs.database;
    uint16_t csize    = s_fs.csize;
    f_mount(NULL, "0:", 0);

    /* -------------------------------------------------------------- */
    /*  Step 3 — Read FAT sector 0 (covers clusters 0..127)           */
    /*  Cluster 0 and 1 are reserved; cluster dirbase = root dir.      */
    /*  We look for free entries (value 0x00000000) starting from      */
    /*  dirbase+1 — the first cluster available for file data.         */
    /* -------------------------------------------------------------- */
    if (HAL_SD_ReadBlocks(&hsd1, s_scratch, fatbase, 1, 5000) != HAL_OK)
        fail_blink(3);

    /* -------------------------------------------------------------- */
    /*  Step 4 — Claim two free clusters                               */
    /* -------------------------------------------------------------- */
    uint32_t *fat = (uint32_t *)s_scratch;
    uint32_t cl_a = 0, cl_b = 0;
    for (uint32_t i = dirbase + 1; i < 128 && cl_b == 0; i++) {
        if ((fat[i] & 0x0FFFFFFFU) == 0) {          /* 0 = free */
            if      (cl_a == 0) { cl_a = i; fat[i] = 0x0FFFFFFFU; }  /* EOC */
            else                { cl_b = i; fat[i] = 0x0FFFFFFFU; }
        }
    }
    if (cl_a == 0 || cl_b == 0) fail_blink(4);

    /* -------------------------------------------------------------- */
    /*  Step 5 — DMA-write updated FAT1 (and FAT2 if present)         */
    /* -------------------------------------------------------------- */
    if (SD_DMA_Write(fatbase, s_scratch, 1) != HAL_OK)
        fail_blink(5);
    if (n_fats == 2) {
        if (SD_DMA_Write(fatbase + fsize, s_scratch, 1) != HAL_OK)
            fail_blink(5);
    }

    /* -------------------------------------------------------------- */
    /*  Step 6 — Read root directory sector 0                          */
    /*  Root dir LBA = database + (dirbase - 2) * csize               */
    /* -------------------------------------------------------------- */
    uint32_t root_lba = database + ((dirbase - 2U) * csize);
    if (HAL_SD_ReadBlocks(&hsd1, s_scratch, root_lba, 1, 5000) != HAL_OK)
        fail_blink(6);

    /* -------------------------------------------------------------- */
    /*  Step 7 — Find two empty directory slots                        */
    /*  First byte 0x00 = end-of-directory; 0xE5 = deleted entry.     */
    /* -------------------------------------------------------------- */
    FAT32DirEnt *dir = (FAT32DirEnt *)s_scratch;
    int slot_a = -1, slot_b = -1;
    for (int i = 0; i < 16 && slot_b < 0; i++) {
        uint8_t first = dir[i].name[0];
        if (first == 0x00 || first == 0xE5) {
            if (slot_a < 0) slot_a = i;
            else            slot_b = i;
        }
    }
    if (slot_a < 0 || slot_b < 0) fail_blink(7);

    /* Build directory entries.
     * FAT date: bits[15:9]=year-1980, bits[8:5]=month, bits[4:0]=day.
     * 2026-05-28 → year=46, month=5, day=28 → 0x5CBC */
    const uint16_t fat_date = 0x5CBCu;

    memset(&dir[slot_a], 0, 32);
    memcpy(dir[slot_a].name, "DMA_A   TXT", 11);
    dir[slot_a].attr         = 0x20;                        /* ARCHIVE */
    dir[slot_a].crt_date     = fat_date;
    dir[slot_a].lst_acc_date = fat_date;
    dir[slot_a].clus_hi      = (uint16_t)(cl_a >> 16);
    dir[slot_a].wrt_date     = fat_date;
    dir[slot_a].clus_lo      = (uint16_t)(cl_a & 0xFFFFU);
    dir[slot_a].size         = FILE_BYTES;

    memset(&dir[slot_b], 0, 32);
    memcpy(dir[slot_b].name, "DMA_B   TXT", 11);
    dir[slot_b].attr         = 0x20;
    dir[slot_b].crt_date     = fat_date;
    dir[slot_b].lst_acc_date = fat_date;
    dir[slot_b].clus_hi      = (uint16_t)(cl_b >> 16);
    dir[slot_b].wrt_date     = fat_date;
    dir[slot_b].clus_lo      = (uint16_t)(cl_b & 0xFFFFU);
    dir[slot_b].size         = FILE_BYTES;

    /* -------------------------------------------------------------- */
    /*  Step 8 — DMA-write updated root directory sector               */
    /* -------------------------------------------------------------- */
    if (SD_DMA_Write(root_lba, s_scratch, 1) != HAL_OK)
        fail_blink(8);

    /* Physical LBA of each file's first sector */
    uint32_t sector_a = database + ((cl_a - 2U) * csize);
    uint32_t sector_b = database + ((cl_b - 2U) * csize);

    /* -------------------------------------------------------------- */
    /*  Build file content                                              */
    /* -------------------------------------------------------------- */
    memset(s_buf_a, 'A', FILE_BYTES);
    {
        int n = snprintf((char *)s_buf_a, 128,
                         "AstroMowe DMA test -- DMA_A.TXT\r\n"
                         "Cluster: %lu  Sector: %lu\r\n"
                         "Size: %u bytes (%u sectors)\r\n"
                         "Status: PASS\r\n",
                         (unsigned long)cl_a, (unsigned long)sector_a,
                         (unsigned)FILE_BYTES, (unsigned)FILE_SECTORS);
        if (n > 0 && (uint32_t)n < FILE_BYTES) s_buf_a[n] = 'A';
    }

    memset(s_buf_b, 'B', FILE_BYTES);
    {
        int n = snprintf((char *)s_buf_b, 128,
                         "AstroMowe DMA test -- DMA_B.TXT\r\n"
                         "Cluster: %lu  Sector: %lu\r\n"
                         "Size: %u bytes (%u sectors)\r\n"
                         "Status: PASS\r\n",
                         (unsigned long)cl_b, (unsigned long)sector_b,
                         (unsigned)FILE_BYTES, (unsigned)FILE_SECTORS);
        if (n > 0 && (uint32_t)n < FILE_BYTES) s_buf_b[n] = 'B';
    }

    /* -------------------------------------------------------------- */
    /*  Step 9/10 — DMA-write file data                                */
    /* -------------------------------------------------------------- */
    if (SD_DMA_Write(sector_a, s_buf_a, FILE_SECTORS) != HAL_OK)
        fail_blink(9);
    if (SD_DMA_Write(sector_b, s_buf_b, FILE_SECTORS) != HAL_OK)
        fail_blink(10);

    /* -------------------------------------------------------------- */
    /*  Step 11/12 — DMA-read file A and verify                        */
    /* -------------------------------------------------------------- */
    memset(s_rx, 0xCC, FILE_BYTES);
    if (SD_DMA_Read(sector_a, s_rx, FILE_SECTORS) != HAL_OK)
        fail_blink(11);
    if (memcmp(s_buf_a, s_rx, FILE_BYTES) != 0)
        fail_blink(12);

    /* -------------------------------------------------------------- */
    /*  Step 14/15 — DMA-read file B and verify                        */
    /* -------------------------------------------------------------- */
    memset(s_rx, 0xCC, FILE_BYTES);
    if (SD_DMA_Read(sector_b, s_rx, FILE_SECTORS) != HAL_OK)
        fail_blink(14);
    if (memcmp(s_buf_b, s_rx, FILE_BYTES) != 0)
        fail_blink(15);

    pass_blink();
}
