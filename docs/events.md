# Events

An event is something that happened: a threshold was crossed, a motor stalled, a run finished.

It is the third kind, and the one with no value. A signal is a value sampled again and again. A
property is a value that stands until it changes. An event is a moment, with nothing to read
afterwards.

## A sketch that reports two events

```cpp
#include <Blaeck.h>

Blaeck device;

float temperature = 0.0;
bool tooHot = false;

float readSensor()
{
  return analogRead(A0) * 0.1;
}

void setup()
{
  Serial.begin(115200);
  device.begin(Serial);

  device.addSignal(F("Temperature"), &temperature);
  device.addEvent(F("Alarm"), F("overheated,cooled_down"))
      .withIcon(F("mdi:thermometer-alert"));
}

void loop()
{
  device.tick();

  temperature = readSensor();

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

- `addEvent()` takes a name and every type that event will ever report, as one
  comma-separated list.
- `writeEvent()` reports one of them. It is fire and forget: a host may show it, and it is never
  logged.
- `tooHot` is what keeps one crossing from firing an event on every pass of `loop()`. The
  library does no such filtering - you decide what counts as having happened.

Home Assistant shows an event entity that fires twice per overheating.

## The list of types

The list is not optional, and its order matters.

What travels on the wire is a pair of numbers: which event, which type. A host reads them
against the list it was given, so position is what identifies a type. You may add to the end of
a list. Reordering it, or removing an entry from the middle, changes what every later type
means.

Types read as identifiers - `cooled_down`, not `Cooled down`. Home Assistant shows them as
written.

A type the board only has sometimes can be added on its own:

```cpp
device.addEvent(F("Alarm"), F("overheated,cooled_down"));

if (hasBatteryMonitor)
  device.addEventType(F("Alarm"), F("low_battery"));
```

It returns false if the type is blank, is already on the list, names an event that was never
added, or does not fit.

## Saying how much

An event carries nothing but its own name, so the wording is fixed when you compile. There is
no way to attach a temperature to `overheated`.

Where a number matters, something else carries it. Log it as a signal if you want it in the
history, or make it a [sensor](properties.md) if it only has to be visible.

## Describing an event

| Call | What it does |
|---|---|
| `withIcon(F("mdi:pulse"))` | A [Material Design Icons](https://pictogrammers.com/library/mdi/) name |
| `withDeviceClass(F("doorbell"))` | `button`, `doorbell` or `motion`: what kind of thing the event reports |
| `diagnostic()` | Marks it as information about the device rather than what it does |
| `disabledByDefault()` | Registered, but switched off until someone enables it |

Ordinary event names, type lists and metadata strings are copied; their buffers can be reused
after registration.

An event name has to be unique only among the events of its board or sub-device, so it may
share a name with a signal, a sensor or a button.

## When an event or type does not fit

Events and types take RAM as they are added; see [Configuration](configuration.md) for what
each costs. Ordinary type strings need copied storage too; types from the same comma-separated
list share one copy.

```cpp
if (device.hasRejections())
{
  device.printRejections(&Serial);
}
```

The summary counts events and types apart. A dropped type is the quieter failure of the two:
the event still works, and one of the things it was meant to report simply never arrives.
