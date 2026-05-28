# SDMMC1 DMA Write — Working Solution

## Hardware

Board: STM32L462RETx, 16 MHz HSI, FreeRTOS CMSIS-RTOS v2, TIM6 as HAL timebase.  
SDMMC1 wired 4-bit to Hirose DM3AT micro SD slot.  
DMA2_Channel4: SDMMC1 TX (CSELR request 7, M2P).  
DMA2_Channel5: SDMMC1 RX (CSELR request 7, P2M).

---

## SD Init Sequence

Defined in `Core/Src/sd_dma.c` → `SD_DMA_Init()`:

1. 1-bit mode, ClockDiv=10 → `HAL_SD_Init()`
2. GPIO pins to VERY_HIGH + PULLUP (MspInit leaves them at MEDIUM — too slow for 4-bit with 47 kΩ board pull-ups)
3. Wait for TRANSFER card state
4. Switch to 4-bit: `HAL_SD_ConfigWideBusOperation(SDMMC_BUS_WIDE_4B)`
5. Set ClockDiv=50 → 480 kHz
6. Wait for TRANSFER card state

Do **not** skip step 2.

---

## Root Cause: DMA2_Channel4 TC Never Fires

`HAL_SD_WriteBlocks_DMA()` (non-L4+ path in `stm32l4xx_hal_sd.c`) calls
`__HAL_SD_DMA_ENABLE` (sets DMAEN=1) **before** `HAL_DMA_Start_IT`. The SDMMC
TXFIFOHE DMA request fires on the DMAEN 0→1 edge. Since Channel4 is not yet armed
at that point, the edge is missed and never re-asserted. After DTEN=1 (DPSM enabled),
the hardware spec forbids writing DMAEN again, so no recovery is possible — Channel4
stalls forever.

---

## Fix: ST TECH040009 Workaround

Abort + DeInit + Init both DMA channels before every SDMMC DMA operation. This
resets the DMA2 controller request-line state so Channel4 responds to the next
TXFIFOHE edge correctly.

```c
/* dma_reset_for_write — call before HAL_SD_WriteBlocks_DMA */
HAL_DMA_Abort(&hdma_sdmmc1_tx);
HAL_DMA_Abort(&hdma_sdmmc1_rx);
HAL_DMA_DeInit(&hdma_sdmmc1_tx);
HAL_DMA_DeInit(&hdma_sdmmc1_rx);
HAL_DMA_Init(&hdma_sdmmc1_tx);   /* re-arm TX only */

/* dma_reset_for_read — call before HAL_SD_ReadBlocks_DMA */
HAL_DMA_Abort(&hdma_sdmmc1_tx);
HAL_DMA_Abort(&hdma_sdmmc1_rx);
HAL_DMA_DeInit(&hdma_sdmmc1_tx);
HAL_DMA_DeInit(&hdma_sdmmc1_rx);
HAL_DMA_Init(&hdma_sdmmc1_rx);   /* re-arm RX only */
```

The HAL source (`stm32l4xx_hal_sd.c`) is **not modified**.

### Additional requirement: wait for card-ready before DMA write

After FatFS IT writes (`f_close` flushes FAT + directory entries), the SD card
is still internally programming. Issuing CMD24 (DMA write) before the card returns
to TRANSFER state causes `HAL_SD_WriteBlocks_DMA` to return `HAL_ERROR`.

`SD_DMA_Write` calls `sd_wait_transfer(1000)` at entry before clearing SDMMC state
or resetting DMA.

### SDMMC interrupt state

Clear stale interrupt enables and flags left by any preceding IT-mode operation
before arming DMA:

```c
SDMMC1->MASK = 0;
SDMMC1->ICR  = 0x1FEFFFFFU;
```

---

## FatFS Write Layer

FatFS uses **IT mode** (`HAL_SD_WriteBlocks_IT`) for writes, not DMA. The DMA
path's `BSP_SD_WriteCpltCallback` queue message was not reliably delivered in this
configuration. IT mode delivers completion via `SDMMC1_IRQHandler` →
`HAL_SD_IRQHandler` → `HAL_SD_TxCpltCallback` → `BSP_SD_WriteCpltCallback` →
`SDQueueID`, which works correctly.

FatFS **reads** continue to use DMA (`HAL_SD_ReadBlocks_DMA`) — no issue on the
read path.

IT write wrapper in `sd_diskio.c`:

```c
static uint8_t _bsp_sd_write_it(uint32_t *p, uint32_t addr, uint32_t n)
{
    return (HAL_SD_WriteBlocks_IT(&hsd1, (uint8_t *)p, addr, n) == HAL_OK)
           ? MSD_OK : MSD_ERROR;
}
#define BSP_SD_WriteBlocks_DMA _bsp_sd_write_it
```

---

## Confirmed Working: Pre-alloc + DMA Write + DMA Read

**Test: `SD_DMA_Test()` in `Core/Src/sd_dma.c`**

1. SD hardware init (4-bit, 480 kHz)
2. FatFS mount; create `STATS.TXT`; write 512 bytes of spaces via IT mode
3. Capture `fil.obj.sclust` (first cluster) while file is open; compute physical sector:
   `start_sector = fs.database + ((sclust - 2) * fs.csize)`
4. `f_close` + `f_mount(NULL)` — FatFS releases card
5. `SD_DMA_Write(start_sector, s_text, 1)` — DMA write readable text to the pre-allocated sector
6. `SD_DMA_Read(start_sector, s_rx, 1)` — DMA read back
7. `memcmp(s_text, s_rx, 512)` — byte compare

Result: **PASS** (LED slow 500 ms blink). `STATS.TXT` readable on PC:
```
AstroMowe DMA file test
Cluster: N  Sector: M
Status: PASS
```

Both TX and RX buffers are 4-byte aligned (`__attribute__((aligned(4)))`).

---

## Interrupt Configuration

All user peripheral IRQs at priority 5 (`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`).

| IRQ | Handler | Priority |
|-----|---------|----------|
| SDMMC1_IRQn | `SDMMC1_IRQHandler` → `HAL_SD_IRQHandler` | 5 |
| DMA2_Channel4_IRQn | `DMA2_Channel4_IRQHandler` → `HAL_DMA_IRQHandler` | 5 |
| DMA2_Channel5_IRQn | `DMA2_Channel5_IRQHandler` → `HAL_DMA_IRQHandler` | 5 |

`SDMMC1->MASK = 0` in `HAL_SD_MspInit()` USER CODE prevents spurious SDMMC interrupts
before the DMA channel is armed.

---

## File Reference

| File | Role |
|------|------|
| `Core/Src/sd_dma.c` | `SD_DMA_Init`, `SD_DMA_Write`, `SD_DMA_Read`, `SD_DMA_Test` |
| `Core/Inc/sd_dma.h` | Public API |
| `FATFS/Target/sd_diskio.c` | FatFS driver — IT writes, DMA reads |
| `Core/Src/stm32l4xx_hal_msp.c` | DMA channel init, IRQ priority config |
| `Core/Src/stm32l4xx_it.c` | IRQ handlers |
