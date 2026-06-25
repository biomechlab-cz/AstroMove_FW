#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>

/* Multi-device synchronization over the shared sync bus (see sync_protocol.md
   and FORMAT.md §10).

   Lines:
     PC6  Sync A  — shared signal net, open-drain + pull-up (wired-AND so any
                    number of devices can pull it low without contention).
     PC7  Sync B  — presence: shorted to GND through the connector, so LOW = the
                    sync cable is plugged in, HIGH = absent.

   The reference event is a LOW edge on the shared PC6 net: every device on the
   bus sees it within wire-propagation time, regardless of power-on skew or when
   each is later unplugged — so it is the only genuinely simultaneous event
   across the group (an individual connector unplug is not). */

typedef struct {
    uint8_t  synced;        /* 1 if a sync pulse was latched while the cable was present */
    uint32_t pulse_index;   /* ACQ_SampleIndex() latched at the last pulse (valid if synced) */
} SYNC_Result;

/* Run the sync sequence. Preconditions: the ADS is sampling with the DRDY ISR
   armed (so ACQ_SampleIndex() advances) and the I2C RTC is reachable.

   If the cable is absent (PC7 HIGH) returns immediately with synced=0.
   Otherwise: emit one pulse on PC6, then — until this device is unplugged
   (PC7 HIGH) — re-latch the sample index and zero the RTC on every LOW edge
   seen on the shared line. Each device pulses once on arming, so the last edge
   before unplug is common to every device still on the bus; that latched index
   is the returned reference. Blocks until unplug. Drives LED_SYNC throughout. */
SYNC_Result SYNC_Run(void);

#endif /* SYNC_H */
