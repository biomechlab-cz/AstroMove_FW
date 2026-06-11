#ifndef ACQUISITION_H
#define ACQUISITION_H

#include <stdint.h>

/* Init: SD 4-bit 480 kHz, mount, snapshot ADS registers, open the
   recording session (SNNNNNNN.EMX + .CSV — see recording.h).
   Call before ADS1292_StartContinuous(). Returns 1 on success. */
uint8_t ACQ_Init(void);

/* Call from main loop. Drains the ISR sample ring; every full 1-second
   chunk is encrypted and written by the recording module. */
void ACQ_Process(void);

/* Called from EXTI0 ISR on ADS DRDY falling edge. Debounces the edge,
   reads one sample over SPI and pushes it into the ring buffer. */
void ACQ_DRDY_Callback(void);

/* Finalize the open batch, close the session files and unmount. */
void ACQ_Stop(void);

/* Timing calibration: LED on, wait 10 s via HAL_GetTick, read RTC before/after,
   print result to USART1. Call after I2C_Sensors_Init(). */
void ACQ_TimingCalibration(void);

#endif /* ACQUISITION_H */
