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

## Giving a signal a unit

Give the unit with `withUnit()`, not in the name:

```cpp
device.addSignal(F("Temperature"), &temperature).withUnit(F("\xC2\xB0" "C"));

for (int i = 0; i < 8; i++)
{
  device.addSignal(F("Current_"), &current[i]).withNameSuffix(i + 1).withUnit(F("mA"));
}
```

The unit is sent in the device list, next to the name. A host shows it, and can convert values
to the unit it expects: Loggbok checks a monitor with limits in `A` against a signal sent in `mA`.
Use the spelling Home Assistant uses, such as `V`, `mA`, `W`, `Hz`, `°C` or `%`, so a host
recognises it. Write `°` as `"\xC2\xB0"` and `μ` as `"\xCE\xBC"`; Arduino source is UTF-8, but
the escapes are safe in any editor.

Signals with the same unit share one stored copy of it, so a series of 40 signals in `mA` stores
`mA` once. Empty text removes the unit; a unit with a control character is refused, and
`hasRejections()` reports it.

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
