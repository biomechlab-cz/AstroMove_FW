#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>

/* Multi-device synchronization over the shared sync bus (see SYNC_PROTOCOL.md
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
    uint16_t group_id;      /* shared session id agreed on the bus (0 if none) — pairs cards */
} SYNC_Result;

/* Run the sync sequence. Preconditions: the ADS is sampling with the DRDY ISR
   armed (so ACQ_SampleIndex() advances), the I2C RTC is reachable, and
   ACQ_SeedNonce() has run (so ACQ_SyncSeed() holds this session's entropy).

   If the cable is absent (PC7 HIGH) returns immediately with synced=0.

   Otherwise two things ride the shared line, distinguished by LOW duration:
     - Sample sync: each device emits one short (~20 ms) pulse on arming; every
       device re-latches its sample index on every short pulse, so the last pulse
       before unplug is the common reference (unchanged, validated mechanism).
     - Group id: once the bus goes quiet (all armed), the leader (the device whose
       UID stagger was shortest) broadcasts a 16-bit session id as a framed packet
       (~60 ms start marker + 16 id bits + 8 checksum bits), repeated a few times.
       Every device decodes it -> result.group_id. The long start marker is not
       mistaken for a sync pulse, so it never disturbs the reference.
   Blocks until this device is unplugged (PC7 HIGH). Drives LED_SYNC throughout. */
SYNC_Result SYNC_Run(void);

#endif /* SYNC_H */
