# Properties

A property is a current value a host shows. It is never logged: a signal is history, a property
is what the device holds right now. There are two families:

- **Inputs** a host sets and sees back: `addNumberInput()`, `addTextInput()`, `addSwitch()`,
  `addSelect()`.
- **Sensors** a host only sees: `addSensor()`.

In Home Assistant, inputs appear under Controls and sensors under Sensors.

| Declared with | Home Assistant | Section |
|---|---|---|
| `addNumberInput()` | number | Controls |
| `addTextInput()` | text | Controls |
| `addSwitch()` | switch | Controls |
| `addSelect()` | select | Controls |
| `addSensor()` | sensor; binary_sensor for a `bool`; an enum sensor for an index with options | Sensors |

## A sketch with an input and a sensor

```cpp
#include <Blaeck.h>

Blaeck device;

float setpoint = 21.0f;
float temperature = 18.0f;

void setup()
{
  Serial.begin(115200);
  device.begin(Serial);

  device.addNumberInput(F("Setpoint"), &setpoint)
      .withRange(5.0f, 30.0f, 0.5f)
      .withUnit(F("\xC2\xB0" "C"));
  device.addSensor(F("Temperature"), &temperature)
      .withUnit(F("\xC2\xB0" "C"));
}

void loop()
{
  temperature += (setpoint - temperature) * 0.001f;
  device.tick();
}
```

- Both bind a global variable. The sketch reads `setpoint` and writes `temperature`; nothing
  else is needed.
- A host sets the input by its name: `<Setpoint,22.5>`. Blaeck checks the value, stores it in
  `setpoint`, acknowledges it and sends it back at once.
- `tick()` checks every property and sends one when it changes.

## Inputs

| Call | Binds | A host may send |
|---|---|---|
| `addNumberInput(name, &value)` | any number type | a number as JSON writes it (`21.5`, `-3`, `1e3`); within `withRange()` if set, and within what the variable holds; without a fraction for an integer |
| `addTextInput(name, buffer, size)` | a `char` buffer of up to 256 bytes | text up to `size - 1` bytes, at most 255; an empty value clears it |
| `addSwitch(name, &value)` | a `bool` | `0` or `1` |
| `addSelect(name, &index, options)` | any integer type | an option's name, or its position counted from 0 |

A value that doesn't fit is refused and changes nothing; the host's acknowledgement says why.
A select always stores the position, whichever form was sent. `getSelectOptionNameAt()` gives
the name of a position, so the sketch doesn't keep the names twice.

Each input takes an optional function, which runs after a host has set the value:

```cpp
void onPumpSpeed() { sendSpeedToPump(pumpSpeed); }

pump.addNumberInput(F("PumpSpeed"), &pumpSpeed, onPumpSpeed)
    .withRange(0.0f, 100.0f, 1.0f);
```

It runs only for a host's value, not when the sketch changes the variable: the sketch knows its
own changes, and a function that set the variable would call itself. The new value is sent back
in both cases.

### On its step

`withRange(min, max, step)` also stores a number on its step, counted from `min`: a value within
a thousandth of a step of one is stored as exactly that. So 0.9, which AVR's `atof()` reads as
0.90000004, is stored as 0.9. A value further off is kept as sent.

A bound the variable cannot hold - `withRange(0, 100000, 1)` on an `int` - sets no range at all
and says so on debug, since a range reaching past the variable would admit a write that cannot
be stored. The range goes out in the variable's own type and at its width, so a step of 0.01 on
a `float` reaches a host as 0.01 rather than as the 0.009999999776482582 a widened float names.

That width is also why a whole-number variable takes whole bounds and a whole step: a fraction
would arrive truncated, 0.5 as 0. `withRange(0, 10, 0.5)` on a `byte` keeps the range and drops
the step; `withRange(0.5, 10.5, 1)` drops the range and keeps the step. Both say so on debug.
Declare the variable as a `float` for a fractional step.

## Sensors

`addSensor()` takes a variable or a function, and what it is decides the kind:

```cpp
device.addSensor(F("Temperature"), &temperature);                 // a number
device.addSensor(F("DoorOpen"), &doorOpen);                       // a bool
device.addSensor(F("State"), &stateIndex, F("Idle,Heating"));     // an index and its options
device.addSensor(F("LastError"), lastError, sizeof(lastError));   // text
device.addSensor(F("Running"), isRunning);                        // a function
```

A function is called on every check, so it must return quickly and must not send anything
itself. Use one for a value that is worked out rather than held. A host that tries to set a
sensor is refused.

Text that only changes when something happens needs a buffer: write into it, and `tick()`
sends it.

## When a property is sent

A property is sent when it changes, checked on every `tick()`. By default any change counts,
and a property is sent at most every 100 ms. `writeOnChange()` changes both:

```cpp
device.addSensor(F("Temperature"), &temperature)
    .writeOnChange(0.1, 1000);   // at least 0.1 apart, at most once a second
```

A host's value is sent back at once. `writeProperty()` sends a property now, changed or not -
for a short pulse `tick()` could miss, or after a gap:

```cpp
device.writeProperty(F("Endstop"));
```

It writes a frame, so never call it from an interrupt: set a flag there and call it in `loop()`.

A property of a device from `addDevice()` that is marked missing is not checked; send its value
with `writeProperty()` before marking it missing if a host should see it.

## Describing a property

| Call | On | What it does |
|---|---|---|
| `withDisplayName(F("Output enabled"))` | all | Label shown instead of the name. A host still sets it by the name |
| `withIcon(F("mdi:tag"))` | all | A [Material Design Icons](https://pictogrammers.com/library/mdi/) name |
| `withDeviceClass(F("temperature"))` | all | What the value is, in Home Assistant's vocabulary |
| `config()` | all | Files it as a setting |
| `diagnostic()` | all | Files it as information about the device |
| `disabledByDefault()` | all | Registered, but switched off until someone enables it |
| `forceUpdate()` | all | A host records every value, even one equal to the last |
| `writeOnChange(delta, ms)` | all | When a change is sent, see above |
| `withRange(min, max, step)` | numbers | The values an input accepts, and its step |
| `withUnit(F("V"))` | numbers | The unit shown after the value |
| `withStateClass(...)` | numbers | How a host treats the values over time |
| `withDisplayPrecision(1)` | numbers | Decimal places shown |
| `withMode(...)` | number and text inputs | A box or a slider; a masked text field |

Device classes come from a different list for each kind: `F("temperature")` on a number,
`F("door")` or `F("motion")` on a `bool`. A name the host doesn't know costs that one entity, so
declare nothing rather than guess.

Ordinary strings are copied, so their buffers can be reused after the call.

## Names

A property is set by its name alone, so a name belongs to one input, sensor, button or command
on the whole board, devices from `addDevice()` included. A second one is refused. Names compare
case-sensitively, may hold only letters, digits, `_`, `-` and `.`, can't start with `BLAECK.`,
and must fit the command buffer. A label with spaces or other characters goes in
`withDisplayName()`. A signal may share a property's name.

## Changing the set while running

`clearAllControls()` removes the inputs and the buttons, `clearAllSensors()` the sensors; what
the other call covers stays. Add the new ones after it, and the changed list is sent to the host
by itself. Handles returned before the call no longer refer to what they did.

```cpp
device.clearAllSensors();
device.addSensor(F("Pressure"), &pressure);
```

## When a property is rejected

A rejected name, or a board out of RAM, drops the property; its handle ignores every call.

```cpp
if (device.hasRejections())
{
  device.printRejections(&Serial);
}
```
