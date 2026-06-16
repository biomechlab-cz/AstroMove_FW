# Sync Protocol

## Hardware Lines

| Pin | Role        | Description                                      |
|-----|-------------|--------------------------------------------------|
| PC6 | Sync A      | Signal line — used to send the sync pulse        |
| PC7 | Sync B      | Presence detection — shorted to GND on connector |


The cable provided shorts red and green cables, the cable is made with right angle right from the connector,
the cables should run away from the board. so that the red wire is next to the STLINK connector.


**PC7 detail:** The pin is pulled high internally. When the mating connector is plugged in, Sync B is shorted to GND, so PC7 reads LOW. When the connector is absent, PC7 floats HIGH via the pull-up.

---

## Initialization Sequence

```
Device powers on
       │
       ▼
  Read PC7
       │
  PC7 == LOW? ──── No ──► Connector absent → skip sync, start measuring
       │
      Yes
       │ (connector detected)
       ▼
  Send pulse on PC6
  Set common clock = 0
  Reconfigure PC6 as input (listen)
       │
       ▼
  ┌─── Listen on PC6, monitor PC7 ───┐
  │                                  │
  │  PC7 goes HIGH?                  │
  │  (connector removed)             │
  │       │ Yes                      │
  │       ▼                          │
  │  Start measuring ◄───────────────┘
  │
  │  Signal received on PC6?
  │  (other device sent its pulse)
  │       │ Yes
  │       ▼
  │  Set common clock = 0
  │  Start measuring
  │
  └─── (loop while PC7 == LOW and no signal)
```

---

## State Description

### 1. Connector Present — First to Start
- PC7 reads LOW → connector is in.
- Device sends a sync pulse on PC6 to notify the peer.
- Sets its own clock to 0 immediately.
- Switches PC6 to input and enters the listen phase.

### 2. Listen Phase
While the connector is still plugged in (PC7 = LOW), the device waits for one of two events:

| Event | Meaning | Action |
|-------|---------|--------|
| PC7 goes HIGH | Connector was removed mid-sequence | Abort sync, begin measuring |
| Pulse received on PC6 | Peer device started and sent its pulse | Set clock to 0, begin measuring |

### 3. Connector Absent at Power-on
If PC7 is HIGH when the device first checks, the connector is not present. The sync sequence is skipped and the device proceeds directly to measuring.

---

## Notes

- Both devices should ideally be connected **before** power-on for a clean synchronized start.
- The device that powers on first sends the pulse and sets its clock; the second device slaves its clock to the received pulse.
- If only one device is present (PC7 never goes LOW), normal single-device operation proceeds without sync.
