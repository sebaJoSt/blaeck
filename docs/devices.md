# Devices

A device is another board, or a separate part of this one, that a host shows as its own device
below this board. The board running blaeck stays the one connected to the host; it fetches the
device's values itself and reports them under the device.

blaeck only does the reporting. How the board talks to the device - I2C, a UART, CAN, a radio -
and when the device counts as missing is up to your sketch.

## A sketch with a second board

```cpp
#include <Blaeck.h>

Blaeck device;
BlaeckDeviceRef pump;

float temperature;
float pumpFlow;

void onPumpSpeed()
{
  sendSpeedToPump(pumpSpeed);         // your code: forward it over the link
}

void setup()
{
  Serial.begin(115200);
  device.begin(Serial);
  device.DeviceName = "Greenhouse";

  pump = device.addDevice(F("Pump controller"))
             .withHWVersion(F("Arduino Uno"))
             .withFWVersion(F("1.0"));

  device.addSignal(F("Temperature"), &temperature);
  pump.addSignal(F("Flow"), &pumpFlow);
  pump.addNumberInput(F("PumpSpeed"), &pumpSpeed, onPumpSpeed)
      .withRange(0.0f, 100.0f, 1.0f);
}

void loop()
{
  temperature = readTemperature();

  if (readFlowFromPump(pumpFlow))     // your code: ask the pump over the link
    pump.markPresent();
  else
    pump.markMissing();

  device.tick();
}
```

A host shows:

```text
Greenhouse
├─ Temperature
└─ Pump controller
   ├─ Flow
   └─ PumpSpeed
```

[SubDevices](../examples/more/SubDevices) is the complete version: a main board and a pump board
talking over a UART, with a checksum, a tolerance for missed replies, restart detection, and a
`reportPumpState()` that sends the pump's speed and link state again after a gap or a restart.

## Registering on a device

`addDevice()` returns a handle, and the handle has the same registration calls as the board:
`addSignal()`, the inputs and `addSensor()`, `addEvent()`, `addEventType()`,
`onCommand()` and `addButton()`. What you register through the handle belongs to the
device; what you register through the board stays on the board.

The writes that take a name look it up in the same place: `pump.write("Flow", value)`,
`pump.writeProperty(...)` and `pump.writeEvent(...)` find the pump's entries, `device.write(...)`
the board's.

Signal and event names only have to be unique within the board or within one device.
The board and two zones can each have a `Temperature` signal or an `Alarm` event; a host
shows them under their own device. Don't repeat the device's name in its entries: a host already
shows "Pump controller" in front of `Flow`. Input, sensor, button and command names are the
exception - they are unique across the whole board, because a host sets or sends them by name.

A host sets a device's input like any other, `<PumpSpeed,40>`: the name is enough. Your
callback forwards the value over your link to the device.

Add devices and register their entries in `setup()`: a host reads the device list and the catalogs when
it connects, and does not ask again for a change made later.

A host identifies a device by its name below the board's name, so keep device names unique and
stable. Renaming a device makes it a new device to the host, just as renaming the board does.
`addDevice()` refuses an empty or duplicate name.

Devices are one level deep: a device cannot have devices of its own.

## When a device stops answering

```cpp
pump.markMissing();   // it stopped answering
pump.markPresent();   // it answers again
```

A host is told at once, or as soon as one can receive frames, so it can show the device as
unavailable. The device list a host asks for says so too.

While a device is missing:

- its signals are left out of every data frame, so a host sees a gap rather than old values
  presented as new;
- `write()` for one of its signals is dropped. With `withDebugStream()`, the first drop is
  noted there, once until `markPresent()`;
- its commands and inputs are refused with the reason "device not responding", and their
  functions don't run;
- its properties are not checked, so nothing of it is sent until it is back.

When the device is back, each signal of it that reports on change is sent again at the next
chance. Its properties are not: fetch them from the device and send them again with
`writeProperty()`, since the host heard nothing of them meanwhile.

Both calls may be made on every poll; a call that changes nothing does nothing. Decide in your
sketch what counts as missing: one missed reply, or several in a row.

## Sending a device's values when they arrive

A device often answers at its own pace. `pump.writeAll()` sends only the pump's signals, right
away, so the frame carries the time of that reading:

```cpp
if (readFlowFromPump(pumpFlow))
  pump.writeAll();
```

For a slow device, such as a radio sensor that reports every minute, turn its signals' interval
off with `writeAtInterval(BLAECK_OFF)` and send them this way, instead of repeating an old value
with a new timestamp on every interval.

## When a device restarts

```cpp
if (reading.uptimeMs < lastPumpUptime)
  pump.writeRestarted();
```

Sends a restart notice for the device, so a host can report that the pump restarted while the
board kept running. Your sketch has to notice the restart; an uptime the device reports is the
simplest way. Send the device's values again afterwards, since they may be back at their
defaults.

## Memory

A board holds at most 254 devices. On AVR each device takes 13 bytes of SRAM plus the names it
copies, and each signal, property, event, button and command carries one byte for its device.
