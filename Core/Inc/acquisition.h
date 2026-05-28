#ifndef ACQUISITION_H
#define ACQUISITION_H

#include <stdint.h>

/* Call before osKernelInitialize. SD 4-bit 480 kHz, mount, open log_NNNN.bin.
   Returns 1 on success, 0 on failure. */
uint8_t ACQ_Init(void);

/* Call after osKernelInitialize, before osKernelStart.
   Creates DRDY semaphore and sample queue. */
void ACQ_CreateRtosObjects(void);

/* FreeRTOS task entry points — pass to osThreadNew. */
void AcqTask(void *arg);     /* high priority: samples ADS on every DRDY */
void SDWriteTask(void *arg); /* normal priority: accumulates and writes to SD */

/* Called from EXTI0 ISR (priority 6) on ADS DRDY falling edge. */
void ACQ_DRDY_Callback(void);

/* Flush and close log file. */
void ACQ_Stop(void);

/* Standalone SD DMA write test — inits card, writes test.txt, returns 1=pass 0=fail.
   Run from a FreeRTOS task (scheduler must be running for DMA queue to work). */
uint8_t ACQ_SD_WriteTest(void);

#endif /* ACQUISITION_H */
