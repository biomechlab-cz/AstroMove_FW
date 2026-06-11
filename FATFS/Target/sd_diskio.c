/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sd_diskio.c
  * @brief   SD Disk I/O driver
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Note: code generation based on sd_diskio_template_bspv1.c v2.1.4
   as "Use dma template" is disabled. */

/* USER CODE BEGIN firstSection */
/* can be used to modify / undefine following code or add new definitions */
/* USER CODE END firstSection*/

/* Includes ------------------------------------------------------------------*/
#include "ff_gen_drv.h"
#include "sd_diskio.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* use the default SD timout as defined in the platform BSP driver*/
#if defined(SDMMC_DATATIMEOUT)
#define SD_TIMEOUT SDMMC_DATATIMEOUT
#elif defined(SD_DATATIMEOUT)
#define SD_TIMEOUT SD_DATATIMEOUT
#else
#define SD_TIMEOUT 30 * 1000
#endif

#define SD_DEFAULT_BLOCK_SIZE 512

/*
 * Depending on the use case, the SD card initialization could be done at the
 * application level: if it is the case define the flag below to disable
 * the BSP_SD_Init() call in the SD_Initialize() and add a call to
 * BSP_SD_Init() elsewhere in the application.
 */
/* USER CODE BEGIN disableSDInit */
#define DISABLE_SD_INIT
/* USER CODE END disableSDInit */

/* Private variables ---------------------------------------------------------*/
/* Disk status */
static volatile DSTATUS Stat = STA_NOINIT;

/* Private function prototypes -----------------------------------------------*/
static DSTATUS SD_CheckStatus(BYTE lun);
DSTATUS SD_initialize (BYTE);
DSTATUS SD_status (BYTE);
DRESULT SD_read (BYTE, BYTE*, DWORD, UINT);
#if _USE_WRITE == 1
DRESULT SD_write (BYTE, const BYTE*, DWORD, UINT);
#endif /* _USE_WRITE == 1 */
#if _USE_IOCTL == 1
DRESULT SD_ioctl (BYTE, BYTE, void*);
#endif  /* _USE_IOCTL == 1 */

const Diskio_drvTypeDef  SD_Driver =
{
  SD_initialize,
  SD_status,
  SD_read,
#if  _USE_WRITE == 1
  SD_write,
#endif /* _USE_WRITE == 1 */

#if  _USE_IOCTL == 1
  SD_ioctl,
#endif /* _USE_IOCTL == 1 */
};

/* USER CODE BEGIN beforeFunctionSection */
/* Transient-failure recovery for SD_read/SD_write.
 *
 * Observed on this board: spurious DRDY/EXTI bursts starve the polling
 * SDMMC FIFO feed → TX underrun → HAL aborts the multi-block write
 * WITHOUT sending CMD12, leaving the card stuck in the receive state.
 * Any further status polling then spins forever. Recovery: abort the
 * transfer explicitly (CMD12), wait with a deadline, retry the same
 * sectors (idempotent — the card rejected the corrupted block by CRC). */
extern SD_HandleTypeDef hsd1;

#define SD_RETRY_COUNT      3
#define SD_READY_TIMEOUT_MS 1000

/* Debug counters (read over SWD) */
volatile uint32_t g_sd_retry_count = 0;  /* attempts that failed and were retried */
volatile uint32_t g_sd_fail_count  = 0;  /* operations that exhausted all retries */
volatile uint32_t g_sd_last_stage  = 0;  /* last failure: 1=BSP call failed, 2=ready-wait timed out */
volatile uint32_t g_sd_last_err    = 0;  /* hsd1.ErrorCode captured before the abort */

/* Wait until the card is back in transfer state. Returns 1 on success. */
static int sd_wait_ready(uint32_t timeout_ms)
{
  uint32_t t0 = HAL_GetTick();
  while (BSP_SD_GetCardState() != MSD_OK)
  {
    if (HAL_GetTick() - t0 > timeout_ms)
      return 0;
  }
  return 1;
}
/* USER CODE END beforeFunctionSection */

/* Private functions ---------------------------------------------------------*/

static DSTATUS SD_CheckStatus(BYTE lun)
{
  Stat = STA_NOINIT;

  if(BSP_SD_GetCardState() == MSD_OK)
  {
    Stat &= ~STA_NOINIT;
  }

  return Stat;
}

/**
  * @brief  Initializes a Drive
  * @param  lun : not used
  * @retval DSTATUS: Operation status
  */
DSTATUS SD_initialize(BYTE lun)
{
Stat = STA_NOINIT;

#if !defined(DISABLE_SD_INIT)

  if(BSP_SD_Init() == MSD_OK)
  {
    Stat = SD_CheckStatus(lun);
  }

#else
  Stat = SD_CheckStatus(lun);
#endif

  return Stat;
}

/**
  * @brief  Gets Disk Status
  * @param  lun : not used
  * @retval DSTATUS: Operation status
  */
DSTATUS SD_status(BYTE lun)
{
  return SD_CheckStatus(lun);
}

/* USER CODE BEGIN beforeReadSection */
/* can be used to modify previous code / undefine following code / add new code */
/* USER CODE END beforeReadSection */
/**
  * @brief  Reads Sector(s)
  * @param  lun : not used
  * @param  *buff: Data buffer to store read data
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to read (1..128)
  * @retval DRESULT: Operation result
  */

DRESULT SD_read(BYTE lun, BYTE *buff, DWORD sector, UINT count)
{
  DRESULT res = RES_ERROR;

  /* Retry transient failures — see note above sd_wait_ready() */
  for (int attempt = 0; attempt < SD_RETRY_COUNT && res != RES_OK; attempt++)
  {
    if(BSP_SD_ReadBlocks((uint32_t*)buff,
                         (uint32_t) (sector),
                         count, SD_TIMEOUT) == MSD_OK
       && sd_wait_ready(SD_READY_TIMEOUT_MS))
    {
      res = RES_OK;
    }
    else
    {
      g_sd_retry_count++;
      HAL_SD_Abort(&hsd1);
      sd_wait_ready(SD_READY_TIMEOUT_MS);
    }
  }

  if (res != RES_OK)
    g_sd_fail_count++;
  return res;
}

/* USER CODE BEGIN beforeWriteSection */
/* can be used to modify previous code / undefine following code / add new code */
/* USER CODE END beforeWriteSection */
/**
  * @brief  Writes Sector(s)
  * @param  lun : not used
  * @param  *buff: Data to be written
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to write (1..128)
  * @retval DRESULT: Operation result
  */
#if _USE_WRITE == 1

DRESULT SD_write(BYTE lun, const BYTE *buff, DWORD sector, UINT count)
{
  DRESULT res = RES_ERROR;

  /* Retry transient failures — see note above sd_wait_ready() */
  for (int attempt = 0; attempt < SD_RETRY_COUNT && res != RES_OK; attempt++)
  {
    uint8_t write_ok = (BSP_SD_WriteBlocks((uint32_t*)buff,
                                           (uint32_t)(sector),
                                           count, SD_TIMEOUT) == MSD_OK);
    if(write_ok && sd_wait_ready(SD_READY_TIMEOUT_MS))
    {
      res = RES_OK;
    }
    else
    {
      g_sd_last_stage = write_ok ? 2 : 1;
      g_sd_last_err   = hsd1.ErrorCode;
      g_sd_retry_count++;
      HAL_SD_Abort(&hsd1);
      sd_wait_ready(SD_READY_TIMEOUT_MS);
    }
  }

  if (res != RES_OK)
    g_sd_fail_count++;
  return res;
}
#endif /* _USE_WRITE == 1 */

/* USER CODE BEGIN beforeIoctlSection */
/* can be used to modify previous code / undefine following code / add new code */
/* USER CODE END beforeIoctlSection */
/**
  * @brief  I/O control operation
  * @param  lun : not used
  * @param  cmd: Control code
  * @param  *buff: Buffer to send/receive control data
  * @retval DRESULT: Operation result
  */
#if _USE_IOCTL == 1
DRESULT SD_ioctl(BYTE lun, BYTE cmd, void *buff)
{
  DRESULT res = RES_ERROR;
  BSP_SD_CardInfo CardInfo;

  if (Stat & STA_NOINIT) return RES_NOTRDY;

  switch (cmd)
  {
  /* Make sure that no pending write process */
  case CTRL_SYNC :
    res = RES_OK;
    break;

  /* Get number of sectors on the disk (DWORD) */
  case GET_SECTOR_COUNT :
    BSP_SD_GetCardInfo(&CardInfo);
    *(DWORD*)buff = CardInfo.LogBlockNbr;
    res = RES_OK;
    break;

  /* Get R/W sector size (WORD) */
  case GET_SECTOR_SIZE :
    BSP_SD_GetCardInfo(&CardInfo);
    *(WORD*)buff = CardInfo.LogBlockSize;
    res = RES_OK;
    break;

  /* Get erase block size in unit of sector (DWORD) */
  case GET_BLOCK_SIZE :
    BSP_SD_GetCardInfo(&CardInfo);
    *(DWORD*)buff = CardInfo.LogBlockSize / SD_DEFAULT_BLOCK_SIZE;
    res = RES_OK;
    break;

  default:
    res = RES_PARERR;
  }

  return res;
}
#endif /* _USE_IOCTL == 1 */

/* USER CODE BEGIN afterIoctlSection */
/* can be used to modify previous code / undefine following code / add new code */
/* USER CODE END afterIoctlSection */

/* USER CODE BEGIN lastSection */
/* can be used to modify / undefine previous code or add new code */
/* USER CODE END lastSection */
