#ifndef ACQUISITION_H
#define ACQUISITION_H

#include <stdint.h>

/* Init: SD 4-bit 480 kHz, mount, open log_NNNN.csv.
   Returns 1 on success, 0 on failure. */
uint8_t ACQ_Init(void);

/* Call from main loop. Writes 1000-sample CSV block when buffer is full. */
void ACQ_Process(void);

/* Called from EXTI0 ISR on ADS DRDY falling edge. */
void ACQ_DRDY_Callback(void);

/* Read ADS config registers and write them as a comment line to the open log file.
   Call after ACQ_Init() and before ADS1292_StartContinuous(). */
void ACQ_WriteDiagnostics(void);

/* Flush and close log file. */
void ACQ_Stop(void);

#endif /* ACQUISITION_H */
