#ifndef ACQUISITION_H
#define ACQUISITION_H

#include <stdint.h>

/* Init: DWT counter, SD 4-bit ~470 kHz, mount, snapshot ADS registers.
   Does NOT open the session — call ACQ_OpenSession() after the sync sequence.
   Call before ADS1292_StartContinuous(). Returns 1 on success. */
uint8_t ACQ_Init(void);

/* Create the recording session files and write the header (see recording.h),
   after the sync sequence. synced = 1 if a multi-device sync pulse was latched;
   sync_lead_samples = EMG samples from the shared sync pulse to the first recorded
   sample (subtract across devices to align — FORMAT.md §10); group_id = shared
   16-bit session id agreed on the sync bus (0 if none) so the Inspector can pair
   cards. Returns 1 on success. */
uint8_t ACQ_OpenSession(uint8_t synced, uint32_t sync_lead_samples, uint16_t group_id);

/* Drop everything sampled so far (the sync-wait samples) and clear the drop
   counter so recording starts clean. Returns the sample index at the reset —
   the absolute index of (about) the first recorded sample. */
uint32_t ACQ_ResetRing(void);

/* Free-running count of valid EMG conversions since power-up — the sample
   timeline the sync sequence latches on (see g_sample_index in acquisition.c). */
uint32_t ACQ_SampleIndex(void);

/* Per-session 16-bit entropy seed for the sync group-id (from ADS analog noise). */
uint16_t ACQ_SyncSeed(void);

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
