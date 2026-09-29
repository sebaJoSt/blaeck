# Signals

A signal is a variable your sketch sends: a temperature, a counter, a switch position.
Blaeck reads it every time it sends data, so you only have to keep the variable up to
date.

The [Signals example](../examples/Signals/Signals.ino) demonstrates numeric, boolean and text
signals and numbered arrays, using simulated values so no sensor hardware is
needed. [ConfigurableSignals](../examples/more/ConfigurableSignals) shows how a user can
choose which signals to log through commands and keep that selection in EEPROM.

## Registering a signal

Call `addSignal()` in `setup()`, once per variable. Pass a name and the address of the
variable:

```cpp
float temperature;

void setup()
{
  Serial.begin(115200);
  device.begin(Serial);

  device.addSignal(F("Temperature"), &temperature);
}
```

Blaeck stores the pointer, not a copy. The variable must therefore be a global, or
anything else that stays alive for as long as the sketch runs. A local variable inside
`setup()` does not work.

These types are accepted:

| | Types |
|---|---|
| Numbers | `byte`, `short`, `unsigned short`, `int`, `unsigned int`, `long`, `unsigned long`, `long long`, `float`, `double` |
| Boolean | `bool` |
| Text | `char` array, ordinary string literal, or `F()` literal |

For text, pass the buffer itself, not its address:

```cpp
char status[16] = "idle";

device.addSignal(F("Status"), status);
```

## Naming a signal

For a series of signals that share a name and end in a number, use `withNameSuffix()`:

```cpp
for (int i = 0; i < 8; i++)
{
  device.addSignal(F("Sine_"), &sine[i]).withNameSuffix(i + 1);
}
```

This registers `Sine_1` to `Sine_8`. The suffix is a number from 0 to 255.

If you have to build a name at runtime, pass the buffer. It is copied, so you can reuse the
buffer immediately:

```cpp
char signalName[10];

for (int i = 0; i < 8; i++)
{
  snprintf(signalName, sizeof(signalName), "Sine_%d", i + 1);
  device.addSignal(signalName, &sine[i]);
}
```

## Showing a signal on a dashboard

A signal is a name and a type: it becomes a column in the log, and that is all. There is no
unit, icon or label on a signal. If a unit belongs in the log, put it in the name:
`Temperature [C]`.

To show the same value on a dashboard, add a sensor on the same variable. The sensor carries
the unit, device class and precision, and is sent when the value changes:

```cpp
device.addSignal(F("Temperature [C]"), &temperature);
device.addSensor(F("Temperature"), &temperature)
    .withUnit(F("\xC2\xB0" "C"))
    .withDeviceClass(F("temperature"))
    .withDisplayPrecision(1);
```

See [Properties](properties.md).

## When a signal does not fit

The signal table grows as signals are added, so the only limit is RAM. When the board runs out,
the signal is dropped - see [Configuration](configuration.md) for what each one costs.

Ask whether that happened:

```cpp
if (device.hasRejections())
{
  device.printRejections(&Serial);
}
```
