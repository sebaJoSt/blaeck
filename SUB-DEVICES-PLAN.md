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
- 0x01 becomes transport-neutral "Device Not Responding".
- Transport is always the sketch's job (UART, CAN, I2C, RF, or none for local parts).
- Not a full `Blaeck` per sub-device: `sizeof(Blaeck)` is 506 bytes on a Mega, 346 on an Uno.

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
Alternative if kept small: rename `inDevice()` to `withDevice()` (matches the with...() style).

ESPHome, for comparison: `esphome: devices:` list (id, name, area_id) and `device_id:` on each
entity; C++ `Device` class, `EntityBase::set_device_()`; the parent is the "main device".

### 2. Device list frame: B3, B6 or a new B7

- B3 (current): no parent field, no display name.
- B6 (BlaeckTCP 4-6, blaecktcpy): parent field, but carries TCP multi-client fields
  (ClientNo, ClientDataEnabled, ClientName, ClientType, ServerRestarted, DeviceType). The spec
  lists a 2-field client trailer; BlaeckTCP, blaecktcpy and Loggbok use 4.
- B7 draft (key reserved in the B2-B7 range):

```
B7 Devices
  LibName\0 LibVersion\0         once per frame
  DeviceCount (1)
  per device:
    DeviceID (1)                 0 = board; equals the SlaveID in catalogs
    ParentID (1)                 board: 0; allows deeper trees later
    DeviceFlags (2)              which optional fields follow
    Name\0                       identity
    HWVersion\0 FWVersion\0
    [DisplayName\0]              changeable label
    [further optional fields]
```

  C0 stays unchanged (Loggbok reads the name at a fixed offset).

Open for B7:
- Optional fields now: DisplayName only, or also manufacturer, model, serial number,
  suggested area, configuration URL (Home Assistant device info).
- A display name for the board too (`DeviceDisplayName` next to `DeviceName`)?
- Send B7 only when the host announces support in GET_DEVICES (current Loggbok cannot parse
  B7), or always?
- blaeck only (reserve a flag bit), or designed for blaecktcpy's hub and "local" devices too?

Loggbok's use of B6 fields today: LibraryName/Version (feature gating), ParentSlaveID (tree),
HW/FW (display); DeviceType only for "local"; ClientDataEnabled warns on "0"; the rest display.

### 3. Naming in docs

"Devices" clashes with `Blaeck device;` and `DeviceName`. Proposed: title "Sub-devices"
(ESPHome's term), `docs/sub-devices.md`, and wording that covers second boards, RF bridge
sensors and parts of the board. Drop the "master" sentence from the addDevice() doc comment.

## To redo or do elsewhere

- blaeck-protocol (spec changes were discarded, redo with the final design):
  status-codes.md 0x01 "Device Not Responding" (keep the old BlaeckSerial rule: values present
  but invalid); a "`@` - Routing" section in commands.md; the device frame chosen above.
- Loggbok: parse B7 if chosen; optionally show "device not responding" for 0x01 (today D2 with
  0x01 is parsed normally); `MqttBridge.BuildDeviceId` is unused dead code.
- blaecktcpy hub decoder: only if blaeck boards should run behind a hub with B7.

## Known limits

- Signal names must be unique across the whole board (Loggbok rejects duplicates); use
  `withNameSuffix()` for repeated sensors, derived from something stable like the bus address.
- Set up in setup(): sub-devices or assignments added after a host connected are not announced;
  sub-devices cannot be removed. A scan at startup works.
- Status 0x01 names only the first missing sub-device per frame.
- A frame whose signals all belong to missing sub-devices is not sent, so WRITE_DATA for only
  those gets no data frame.
- One timestamp per data frame (the board's send time).
- After a sub-device restart only C0 is sent; its state channel values are not resent.

## Tests still to run

- Mega: `drive_device_tree.py` against DeviceTreeTest (see extras/tests/harness/README.md).
- Loggbok on the work machine: device tree, MQTT/Home Assistant devices and controls (routed
  `@` commands), database columns.
- Optional: two Megas with the SubDevices example (TX1-RX1 crossed, GND).
