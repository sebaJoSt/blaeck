# Sub-devices

A sub-device is another board, or a part of this one, shown in Home Assistant as its own device
below this board. The board fetches its values over any link - I2C, UART, CAN, radio - and
reports them under it.

```text
Greenhouse                 device
└─ Pump controller         sub-device
   ├─ Flow                 signal
   └─ PumpSpeed            control
```

## Example

```cpp
Blaeck device;
BlaeckDeviceRef pump;

float pumpFlow;
float pumpSpeed;

void onPumpSpeed()                    // <PumpSpeed,40>
{
  sendSpeedToPump(pumpSpeed);         // your code: forward over the link
}

void setup()
{
  Serial.begin(115200);
  device.begin(Serial);
  device.withName(F("Greenhouse"));

  pump = device.addDevice(F("Pump controller"));
  pump.addSignal(F("Flow"), &pumpFlow);
  pump.addNumberInput(F("PumpSpeed"), &pumpSpeed, onPumpSpeed)
      .withRange(0.0f, 100.0f, 1.0f);
}

void loop()
{
  if (readFlowFromPump(pumpFlow))     // your code: ask the pump over the link
  {
    pump.markPresent();
    pump.writeAll();
  }
  else
    pump.markMissing();

  device.tick();
}
```

- `addDevice()` returns a handle with the same `add…()` calls as the board.
- `markMissing()` shows the sub-device as unavailable; its signals are left out and its controls
  refused until `markPresent()`.
- `writeAll()` sends the sub-device's signals right away, with the time of the reading.
- `writeRestarted()` reports that the sub-device restarted.

[SubDevices](../examples/more/SubDevices) is a complete example with two boards on a UART.
