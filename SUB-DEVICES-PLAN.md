# Sub-devices: status, plan and open questions

Working note for the `sub-devices` branch. Remove it before merging.

## Goal

BlaeckSerial's I2C master/slave mode is gone. Instead, blaeck lets a sketch show things as their
own devices below the board, while the sketch does the communication itself: a second board on a
UART, CAN or I2C, the sensors of an RF bridge, or parts of the board itself. blaeck only reports.

## What the branch has

- `device.addDevice(name)` returns a `BlaeckDeviceRef`, with `withHWVersion()` and
  `withFWVersion()` (default "n/a"). Slave IDs are assigned in order, 1-255. An empty or
  duplicate name, or a full table, is rejected. `withDevices(n)` on the begin() chain.
- `.inDevice(ref)` on signals, typed commands, state channels and event channels. A command's
  `withOwnState()` channel follows its command in either order. Plain `onCommand()` commands
  stay on the board (no handle).
- Frames: B3 device list and C0 restart report the board as master (single without devices) and
  each sub-device as a slave. B0, A0, 90, 95, 80 and 85 carry the owner's ownership bytes.
  Without sub-devices every frame is byte-identical to before.
- `markMissing()` / `markPresent()`: a missing sub-device's signals are left out of data frames,
  with status 0x01 and payload `[0, first skipped index lo, hi, slave ID]`. On return, its
  changed-only signals are sent again.
- `writeRestarted()` on the handle sends a C0 for that sub-device only.
- `@<slaveID>:` command routing, as Loggbok sends it for a sub-device's command
  (`<@1:#12:SET_PUMP_SPEED,40>`). Runs only if the command belongs to that sub-device; an unknown
  ID stays in the name and is answered unknown. Ack hashes cover the command after all prefixes.
- Tests: native tests (frames byte by byte, rejections, missing/present, routing; both checked
  with deliberate mutations). `extras/tests/harness/DeviceTreeTest` for a Mega with a simulated
  second board and `drive_device_tree.py` (19 checks).
- `examples/more/SubDevices/MainBoard` and `PumpBoard`: UART, byte-based message with checksum,
  3 missed replies = missing, restart detection by uptime.
- `docs/devices.md`, README rows, measured AVR sizes (signal 12, command 66, state channel 35,
  event channel 13, device 10 bytes).

## How it was verified

- Native test suite, all variants (CI Host Tests and locally).
- The real DeviceTreeTest sketch compiled natively with Serial on a pseudo-terminal, driven by
  the real Python driver: all checks pass, and a broken build makes the driver fail.
- The two example sketches run natively as two processes linked by a pseudo-terminal, with a
  Python host: device list, routed speed command, "unplugged" pump (signals gone, 0x01, "no
  answer"), plugged back in (one restart notice for the pump, values back).
- Not yet: a real Mega, and Loggbok end to end (tree, MQTT/Home Assistant, database columns).

## Decided

- One level of sub-devices; the board is the only parent.
- Names are the identity. Loggbok builds MQTT topics and Home Assistant identity from the device
  name path (`MqttTopics.ResolveDevicePath`), not from slave IDs, so IDs may follow
  registration order. Renaming a device makes it a new device in Home Assistant.
- Transport is always the sketch's job (UART, CAN, I2C, RF, or none for local parts).
- Not a full `Blaeck` per sub-device: `sizeof(Blaeck)` is 506 bytes on a Mega, 346 on an Uno.
- Device list: a new B7 (see below), sent always. No capability negotiation: Loggbok asks
  GET_DEVICES first and must understand B7 and C1 before blaeck 7.0 is released.
- Device state reaches hosts in two ways, never as data-frame status:
  - B7 carries a DeviceState byte per device, a snapshot at GET_DEVICES time: NotResponding,
    and Restarted (a restart not yet reported). DeviceState is not part of the identity.
  - a reworked restart notice, C1 "Device Notification", carries every change after that:
    restarted, not responding, responding again. Sent always, for the board's own restart too.
  - Each restart is reported exactly once, in B7 or C1, whichever reaches a host first.
  - Missing sub-devices' signals are still left out of data frames (a gap in the database),
    and data-frame status 0x01 is dropped. Home Assistant needs availability, because an MQTT
    sensor keeps showing its last value through a gap.
- Per-device writes: `pump.writeAllData()` / `writeAllData(timestamp)` send only that
  sub-device's signals, with the time of the actual reading. For slow sources (an RF sensor
  every 60 s), use `writeAtInterval(BLAECK_OFF)` and write when data arrives, instead of
  repeating an old value with a new timestamp every interval. Uses the existing D2 frame.

The code on this branch still sends B3, C0 and status 0x01; it has to follow this plan.

## Open questions

### 1. Registration API

Proposed: a shared base class with all registration functions (`addSignal`, `addStateChannel`,
`addEventChannel`, `onCommand`, `onNumberCommand` ... `onTextCommand`), inherited by `Blaeck`
and by the sub-device handle, so a sub-device registers directly:

```cpp
BlaeckDeviceRef pump = device.addDevice(F("Pump controller"));
pump.addSignal(F("Flow"), &pumpFlow);
pump.onNumberCommand("SET_PUMP_SPEED", onSetPumpSpeed).withRange(0.0f, 100.0f, 1.0f);
```

Replaces `inDevice()`; plain `onCommand()` could then belong to a sub-device too. About 60
declarations with their docs move into the base class. To check: editor hover on inherited
members, checkdocs.py finding base-class docs, `Blaeck` staying non-polymorphic.
AGENTS.md currently says "One concrete `Blaeck` class ... with no core base class"; a
registration base class is not a transport hook, but that rule would have to be reworded.
Alternative if kept small: rename `inDevice()` to `withDevice()` (matches the with...() style).

ESPHome, for comparison: `esphome: devices:` list (id, name, area_id) and `device_id:` on each
entity; C++ `Device` class, `EntityBase::set_device_()`; the parent is the "main device".

### 2. Device list and notification frames: B7 and C1

- B3 (current): no parent field, no display name.
- B6 (BlaeckTCP 4-6, blaecktcpy): parent field, but carries TCP multi-client fields
  (ClientNo, ClientDataEnabled, ClientName, ClientType, ServerRestarted, DeviceType). The spec
  lists a 2-field client trailer; BlaeckTCP, blaecktcpy and Loggbok use 4.
- Why restart state belongs in the device list: BlaeckSerial announced a restart with C0 on
  its first pass, because the serial port stays open through a reboot. BlaeckTCP had no C0: a
  reboot drops every TCP connection, so a notice at boot reaches nobody; the host always
  reconnects and asks GET_DEVICES, and B6's ServerRestarted ("true until the first device
  list after boot") told it. blaeck serves both transports, so it keeps both paths and makes
  sure a restart is reported only once. B6's flaw with several clients (only the first to ask
  saw "1") no longer applies: blaeck has one host at a time.

- B7 (next key in the B2-B7 block), chosen:

```
B7 Devices
  LibName\0 LibVersion\0         once per frame
  DeviceCount (1)
  per device:
    DeviceID (1)                 0 = board; equals the SlaveID in catalogs      identity
    ParentID (1)                 board: 0; allows deeper trees later            identity
    DeviceFlags (2)              which optional fields follow                   identity
    DeviceState (1)              bit 0 NotResponding, bit 1 Restarted           snapshot
    Name\0                       identity
    HWVersion\0 FWVersion\0      versions (may change between sessions)
    [DisplayName\0]              changeable label
    [further optional fields]
```

- C1 (next key in the C0-C3 block), replaces C0:

```
C1 Device Notification
  DeviceID (1)        0 = the board, 1..255 = sub-devices; same ID as in B7 and the catalogs
  Event (1)           0x01 restarted, 0x02 not responding, 0x03 responding again
                      (0x00 and 0x04-0xFF reserved)
```

  Message ID 0 (not an answer), no CRC, one event per frame. No name, versions or library
  info: the host looks them up in B7, and duplicating them would let the two disagree.

- Rules:
  1. Restarted means "a restart not yet reported to a host". It is set at boot for the board
     and by `writeRestarted()` for a sub-device, and cleared as soon as it has gone out, in B7
     or as C1 "restarted", whichever reaches a host first. On TCP that is practically always
     B7, since a host's first command is GET_DEVICES; on Serial it is C1 at boot, since the
     host is already listening. A C1 "restarted" is held back until a host can receive frames.
  2. NotResponding is the state at GET_DEVICES time; every later change arrives as C1
     (`markMissing()` / `markPresent()` send it only on a real change).
  3. Missing sub-devices stay in B7 with the bit set: B7 lists everything the sketch declared,
     and the catalogs still refer to their IDs. A sub-device is absent only if the sketch never
     declares it (for example a scan at startup did not find it).
  4. B7 behaves like the signal list (B0): sent only in answer to GET_DEVICES and fixed for a
     logging session, except DeviceState. blaeck never sends it on its own, not at startup and
     not when a name or version changes (visible from the next session). The catalogs blaeck
     resends on change (state channels, events, commands, signal config) stay as they are.

Still open for B7/C1:
- Optional fields now: DisplayName only, or also manufacturer, model, serial number,
  suggested area, configuration URL (Home Assistant device info).
- A display name for the board too (`DeviceDisplayName` next to `DeviceName`)?
- blaeck only (reserve a flag bit), or designed for blaecktcpy's hub and "local" devices too?
- A third state "unknown" until the sketch first reports, or start as responding (sketches
  that are unsure call `markMissing()` in setup())?

Loggbok's use of B6 fields today: LibraryName/Version (feature gating), ParentSlaveID (tree),
HW/FW (display); DeviceType only for "local"; ClientDataEnabled warns on "0"; the rest display.

### 3. Naming in docs

"Devices" clashes with `Blaeck device;` and `DeviceName`. Proposed: title "Sub-devices"
(ESPHome's term), `docs/sub-devices.md`, and wording that covers second boards, RF bridge
sensors and parts of the board. Drop the "master" sentence from the addDevice() doc comment.

## To redo or do elsewhere

- blaeck-protocol (spec changes were discarded, redo with the final design): B7 and C1 frame
  pages and message-keys.md; a "`@` - Routing" section in commands.md; status-codes.md keeps
  0x01 only as BlaeckSerial 6's "I2C Slave Skipped" (values present but invalid), unused by
  blaeck.
- Loggbok, before blaeck 7.0 ships: parse B7 and C1 (its restart handling, including the
  interval recovery, currently reacts to C0 only); after a TCP reconnect, read the board's
  Restarted bit from the B7 answer where it reads B6's ServerRestarted today, now per device;
  per-device availability in Home Assistant
  (today there is one availability topic for the whole bridge); keep the device tree when a
  B7 arrives during logging (ProcessDevices currently replaces the whole list).
  `MqttBridge.BuildDeviceId` is unused dead code.
- Loggbok notifications: bring back events 515/516, generalised, fed by C1. Loggbok commit
  55970bca (2026-08-25, "Drop the I2C skip events, which no device can raise any more")
  removed them: `I2CSkipTracker`, events 515 "I2C Warning" / 516 "I2C Recovered" with device
  name resolution, their two settings entries in `SettingsViewModel`, and
  `SkippedSlaveCount` / `FirstSkippedSlaveID`. Restore the event IDs, settings entries and name
  resolution as "Device not responding" / "Device responding again", driven by C1 (and B7's
  NotResponding at GET_DEVICES time). Do not restore the tracker: it inferred skipped slaves
  from signals absent in a data frame, which only worked because BlaeckSerial 6 always sent
  every signal; blaeck sends partial frames (writeOnChange, per-device writes), so a quiet
  sub-device would look skipped. Later, the hub's 0x80/0x81 events (511/512) could share this
  path if blaecktcpy switches to C1; C1 would then need a place for the hub's auto-reconnect
  flag (today byte 0 of the 0x80 payload).
- Loggbok gap, independent of sub-devices: after a TCP reconnect it asks GET_DEVICES again but
  never compares the answer with the session. ProcessDevices replaces the list, and
  ValidateDevicesAfterReconnectAsync only checks that a device answered, ClientDataEnabled and
  ServerRestarted. A different board at the same address (DHCP reuse, reflashed firmware) is
  accepted silently. Proposed rule, matching "device list fixed per session":
  - identity must match, else reject the reconnect with a clear error and stop or wait: board
    name and library name, the same sub-devices by ID, name and parent, and the same signal
    list (schema hash);
  - versions may differ, with a warning (a firmware update during the outage is plausible);
  - DeviceState may differ and is left out of the comparison (a sub-device went missing, or
    the board restarted, during the outage).
- blaecktcpy hub decoder: only if blaeck boards should run behind a hub with B7.

## Known limits

- Signal names must be unique across the whole board (Loggbok rejects duplicates); use
  `withNameSuffix()` for repeated sensors, derived from something stable like the bus address.
- Set up in setup(): sub-devices or assignments added after a host connected are not announced;
  sub-devices cannot be removed. A scan at startup works.
- A frame whose signals all belong to missing sub-devices is not sent, so WRITE_DATA for only
  those gets no data frame.
- One timestamp per data frame; per-device `writeAllData()` (planned) gives each sub-device
  its own frame and reading time.
- After a sub-device restart only a notification is sent; its state channel values are not
  resent.

## Tests still to run

- Mega: `drive_device_tree.py` against DeviceTreeTest (see extras/tests/harness/README.md).
- Loggbok on the work machine: device tree, MQTT/Home Assistant devices and controls (routed
  `@` commands), database columns.
- Optional: two Megas with the SubDevices example (TX1-RX1 crossed, GND).
