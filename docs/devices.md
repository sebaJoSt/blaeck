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

void onSetPumpSpeed(const char *command, const char *const *params, byte paramCount)
{
  sendSpeedToPump(atoi(params[0]));   // your code: forward it over the link
}

void setup()
{
  Serial.begin(115200);
  device.begin(Serial).withDevices(1);
  device.DeviceName = "Greenhouse";

  pump = device.addDevice(F("Pump controller"))
             .withHWVersion(F("Arduino Uno"))
             .withFWVersion(F("1.0"));

  device.addSignal(F("Temperature"), &temperature);
  pump.addSignal(F("Flow"), &pumpFlow);
  pump.onNumberCommand("SET_PUMP_SPEED", onSetPumpSpeed)
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
   └─ SET_PUMP_SPEED
```

[SubDevices](../examples/more/SubDevices) is the complete version: a main board and a pump board
talking over a UART, with a checksum, a tolerance for missed replies and restart detection.

## Registering on a device

`addDevice()` returns a handle, and the handle has the same registration calls as the board:
`addSignal()`, `addStateChannel()`, `addEventChannel()`, `addEventType()`, `onCommand()` and the
typed commands. What you register through the handle belongs to the device; what you register
through the board stays on the board. A command's `withOwnState()` channel belongs to the
command's device.

The writes that take a name look it up in the same place: `pump.write("Flow", value)`,
`pump.writeState(...)` and `pump.writeEvent(...)` find the pump's entries, `device.write(...)`
the board's.

Names only have to be unique within the board or within one device. The board and two zones
can each have a `Temperature` signal, a `Status` state channel or an `Alarm` event channel; a
host shows them under their own device. Don't repeat the device's name in its entries: a host
already shows "Pump controller" in front of `Flow`. Command names are the exception - they are
unique across the whole board, because an incoming command is found by its name.

A host sends a device's command with the device's ID in front, such as
`<@1:SET_PUMP_SPEED,40>`. blaeck runs it only if the command belongs to that device, so your
handler is the same whether the command came from a dashboard or was typed without the prefix.

Add devices and assign entries in `setup()`: a host reads the device list and the catalogs when
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

While a device is missing, its signals are left out of every data frame, so a host sees a gap
rather than old values presented as new. A frame that leaves a signal out carries status `0x01`,
Device Not Responding, naming the first signal left out and its device. When the device is back,
each signal of it that reports on change is sent again at the next chance.

Both calls may be made on every poll; a call that changes nothing does nothing. Decide in your
sketch what counts as missing: one missed reply, or several in a row.

## When a device restarts

```cpp
if (reading.uptimeMs < lastPumpUptime)
  pump.writeRestarted();
```

Sends a restart notice with the device's name, so a host can report that the pump restarted
while the board kept running. Your sketch has to notice the restart; an uptime the device
reports is the simplest way.

## Table size and memory

`withDevices()` on the `begin()` chain sets how many devices fit; the default depends on the
board, 4 on a Mega. On AVR each device takes 10 bytes of SRAM plus the names it copies, and
each signal, command and channel carries one byte for its device.
