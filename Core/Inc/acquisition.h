#ifndef ACQUISITION_H
#define ACQUISITION_H

#include <stdint.h>

/* Init: SD 4-bit ~470 kHz, mount, snapshot ADS registers, open the
   recording session (SNNNNNNN.EMX + .CSV — see recording.h).
   Call before ADS1292_StartContinuous(). Returns 1 on success. */
uint8_t ACQ_Init(void);

/* Seed the AES-GCM nonce salt from analog front-end noise (FORMAT.md §6).
   Call after ADS1292_StartContinuous() and before the DRDY EXTI is armed —
   reads the live RDATAC stream and installs the salt via REC_SetNonceSalt(). */
void ACQ_SeedNonce(void);

/* Call from main loop. Drains the ISR sample ring; every full 1-second
   chunk is encrypted and written by the recording module. */
void ACQ_Process(void);

/* Called from EXTI0 ISR on ADS DRDY falling edge. Debounces the edge,
   reads one sample over SPI and pushes it into the ring buffer. */
void ACQ_DRDY_Callback(void);

/* Finalize the open batch, close the session files and unmount. */
void ACQ_Stop(void);

#endif /* ACQUISITION_H */
