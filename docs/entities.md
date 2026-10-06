# Entities

An entity is a control, sensor or event in Home Assistant.

| Declared with | What it is | Home Assistant | Section |
|---|---|---|---|
| `addNumberInput()` | A number the user enters | number | Controls |
| `addTextInput()` | Text the user enters | text | Controls |
| `addSwitch()` | Turns something on or off | switch | Controls |
| `addSelect()` | An option the user selects from a fixed list | select | Controls |
| `addButton()` | Triggers an action; has no state | button | Controls |
| `addSensor()` | Read-only information; a binary sensor has two states | sensor; binary_sensor for a `bool`; enum sensor for an index with options | Sensors |
| `addEvent()` | Something that happened, such as a doorbell press | event | Events |

## A sketch with a number, a sensor and a button

```cpp
#include <Blaeck.h>

Blaeck device;

float setpoint = 21.0f;
float temperature = 18.0f;

void onReset()
{
  setpoint = 21.0f;
}

void setup()
{
  Serial.begin(115200);
  device.begin(Serial);

  device.addNumberInput(F("Setpoint"), &setpoint)   // <Setpoint,22.5>
      .withRange(5.0f, 30.0f, 0.5f)
      .withUnit(F("\xC2\xB0" "C"));
  device.addSensor(F("Temperature"), &temperature)
      .withUnit(F("\xC2\xB0" "C"));
  device.addButton("Reset", onReset);               // <Reset>
}

void loop()
{
  temperature += (setpoint - temperature) * 0.001f;
  device.tick();
}
```

- The sketch reads `setpoint` and writes `temperature`; Blaeck does the rest.
- `<Setpoint,22.5>` sets the number. Blaeck stores the value, acknowledges it and sends it back.
- `<Reset>` presses the button.
- `tick()` sends every value that changed.

## Controls

Number, text, switch and select hold a value; a button doesn't.

### Controls with a value

Each binds a variable:

```cpp
float setpoint = 21.0f;
char label[32] = "Kitchen";
bool heaterOn = false;
byte mode = 0;

device.addNumberInput(F("Setpoint"), &setpoint)           // <Setpoint,22.5>
    .withRange(5.0f, 30.0f, 0.5f);
device.addTextInput(F("Label"), label, sizeof(label));    // <Label,Hallway>
device.addSwitch(F("Heater"), &heaterOn);                 // <Heater,1>
device.addSelect(F("Mode"), &mode, F("Off,Heat,Auto"));   // <Mode,Heat> or <Mode,1>
```

| Call | Binds | Accepts |
|---|---|---|
| `addNumberInput()` | any number type | a number such as `21.5`, `-3` or `1e3`, within the range |
| `addTextInput()` | a `char` buffer, up to 256 bytes | text up to `size - 1` bytes; an empty value clears it |
| `addSwitch()` | a `bool` | `0` or `1` |
| `addSelect()` | any integer type | an option's name, or its position counted from 0 |

The acknowledgement tells whether a value was taken. A select stores the position;
`getSelectOptionNameAt()` gives the option's name for it.

An optional function runs when the user sets the value:

```cpp
void onPumpSpeed() { sendSpeedToPump(pumpSpeed); }

pump.addNumberInput(F("PumpSpeed"), &pumpSpeed, onPumpSpeed)   // <PumpSpeed,75>
    .withRange(0.0f, 100.0f, 1.0f);
```

When the sketch itself changes `pumpSpeed`, the new value is sent, but `onPumpSpeed()` doesn't
run.

#### Ranges and steps

`withRange(min, max, step)` sets the values a number accepts and the step Home Assistant offers.
The range follows the variable's type:

```cpp
device.addNumberInput(F("Gain"), &gain).withRange(0.0f, 2.0f, 0.1f);    // <Gain,0.9>          float: decimal step
device.addNumberInput(F("Speed"), &speed).withRange(0, 255, 1);         // <Speed,128>         byte: whole numbers
device.addNumberInput(F("Total"), &total).withRange(0, 4000000000, 1);  // <Total,3999999999>  unsigned long: exact, also on AVR
```

- Values are stored on the step: `<Gain,0.9>` stores 0.9, the step itself.
- The range goes out in the variable's own type, so a step of 0.1 shows as 0.1.
- A decimal step takes a `float` variable. The debug stream reports a range the variable can't
  hold.

### Buttons

A button carries no value, so its function takes no parameters:

```cpp
void onStatus()
{
  reportStatus();
}

device.addButton("STATUS", onStatus)   // <STATUS>
    .withDisplayName(F("Request status"))
    .diagnostic();
```

A lambda gives one function fixed arguments, so several buttons share it:

```cpp
device.addButton("ACTIVATE_ALL", []() { activateRange(1, 40); })     // <ACTIVATE_ALL>
    .withDisplayName(F("Activate all"));
device.addButton("ACTIVATE_FIRST", []() { activateRange(1, 10); })   // <ACTIVATE_FIRST>
    .withDisplayName(F("Activate first ten"));
```

## Sensors

The argument of `addSensor()` decides the kind:

```cpp
device.addSensor(F("Temperature"), &temperature);                 // a number
device.addSensor(F("DoorOpen"), &doorOpen);                       // a bool
device.addSensor(F("State"), &stateIndex, F("Idle,Heating"));     // an index and its options
device.addSensor(F("LastError"), lastError, sizeof(lastError));   // text
device.addSensor(F("Running"), isRunning);                        // a function
```

- A function suits a value that is worked out. It is called on every check, so it returns
  quickly.
- Text that changes on an occasion lives in a buffer: the sketch writes into it, and `tick()`
  sends it.

## Events

An event is a moment: a threshold was crossed, a motor stalled, a run finished.

```cpp
#include <Blaeck.h>

Blaeck device;

float temperature = 0.0;
bool tooHot = false;

void setup()
{
  Serial.begin(115200);
  device.begin(Serial);

  device.addEvent(F("Alarm"), F("overheated,cooled_down"))
      .withIcon(F("mdi:thermometer-alert"));
}

void loop()
{
  device.tick();

  temperature = analogRead(A0) * 0.1;

  if (temperature > 60.0 && !tooHot)
  {
    tooHot = true;
    device.writeEvent(F("Alarm"), F("overheated"));
  }
  if (temperature < 55.0 && tooHot)
  {
    tooHot = false;
    device.writeEvent(F("Alarm"), F("cooled_down"));
  }
}
```

- `addEvent()` takes a name and every type the event reports, as one comma-separated list.
- `writeEvent()` reports one type. Events are fire and forget, and never logged.
- `tooHot` makes each crossing fire once.

### The list of types

A type travels as its position in the list, so new types go at the end. Types read as
identifiers, such as `cooled_down`; Home Assistant shows them as written.

A type the board only has sometimes is added on its own:

```cpp
device.addEvent(F("Alarm"), F("overheated,cooled_down"));

if (hasBatteryMonitor)
  device.addEventType(F("Alarm"), F("low_battery"));
```

## When a property is sent

Controls with a value and sensors are properties: values that stand until they change. `tick()`
checks them and, by default, sends any change at most every 100 ms. `writeOnChange(delta, ms)`
sets both; 0 ms sends every change at once:

```cpp
device.addSensor(F("Temperature"), &temperature)
    .writeOnChange(0.1, 1000);   // at least 0.1 apart, at most once a second

device.addSensor(F("Pressure"), &pressure)
    .writeOnChange(BLAECK_ANY_CHANGE, 20);   // any change, at most every 20 ms

device.addSensor(F("Endstop"), &endstop)
    .writeOnChange(BLAECK_ANY_CHANGE, 0);   // every change, no limit
```

A value the user sets goes back at once. `writeProperty()` sends a property right away, for a
short pulse:

```cpp
device.writeProperty(F("Endstop"));
```

From an interrupt, a flag does the job: the interrupt sets it, and `loop()` calls
`writeProperty()`.

## Describing an entity

```cpp
device.addSensor(F("BoardTemp"), &boardTemp)
    .withDisplayName(F("Board temperature"))
    .withDeviceClass(F("temperature"))
    .withUnit(F("\xC2\xB0" "C"))
    .withDisplayPrecision(1)
    .diagnostic();
```

| Call | On | What it does |
|---|---|---|
| `withDisplayName(F("Output enabled"))` | all | Label shown instead of the name. Commands still use the name |
| `withIcon(F("mdi:tag"))` | all | A [Material Design Icons](https://pictogrammers.com/library/mdi/) name |
| `withDeviceClass(F("temperature"))` | all | What the entity is, in Home Assistant's vocabulary |
| `config()` | all | Files it under Configuration |
| `diagnostic()` | all | Files it under Diagnostic, as information about the device |
| `disabledByDefault()` | all | Registered, but switched off until someone enables it |
| `forceUpdate()` | controls, sensors | Every value recorded, even one equal to the last |
| `writeOnChange(delta, ms)` | controls, sensors | When a change is sent, see above |
| `withRange(min, max, step)` | numbers | The values a number accepts, and its step |
| `withUnit(F("V"))` | numbers | Unit shown after the value |
| `withStateClass(...)` | numbers | How the values are treated over time, for statistics |
| `withDisplayPrecision(1)` | numbers | Decimal places shown |
| `withMode(BLAECK_NUMBER_MODE_SLIDER)` | number | A box or a slider |
| `withMode(BLAECK_TEXT_MODE_PASSWORD)` | text | A masked field |

Device classes are optional and come from a separate list for each kind:

| Kind | Examples |
|---|---|
| Number | `temperature`, `voltage` |
| `bool` sensor | `door`, `motion` |
| Button | `restart`, `identify`, `update` |
| Event | `button`, `doorbell`, `motion` |

Strings are copied, so buffers can be reused after the call.

## Names

Names use letters, digits, `_`, `-` and `.`, such as `PumpSpeed` or `pump.speed`. A label with
spaces goes in `withDisplayName()`. Names starting with `BLAECK.` are reserved.

- Controls, sensors and commands share one set of names on the board, sub-devices
  included.
- Event names are separate, one set for each board or sub-device.
- A signal may share any entity's name.

## Changing the set while running

| Call | Removes |
|---|---|
| `clearAllControls()` | controls |
| `clearAllSensors()` | sensors |
| `clearAllEvents()` | events and their types |

New entities are added after the call, and the new list is sent by itself. The new add calls
return the handles to use from then on.

```cpp
device.clearAllSensors();
device.addSensor(F("Pressure"), &pressure);
```

## Checking the setup

`printRejections()` counts what was dropped, by kind: a name that is invalid or taken, an
invalid event type, or no memory left. `withDebugStream()` names each one as it happens.
[Configuration](configuration.md) shows the memory each kind takes.

```cpp
if (device.hasRejections())
{
  device.printRejections(&Serial);
}
```
