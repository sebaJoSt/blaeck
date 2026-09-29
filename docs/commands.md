# Commands

A command is something sent to your sketch from outside that isn't a value: run a calibration,
print a report, press a button. You write a function, register it under a name, and Blaeck
calls it when that name arrives.

For a value a host sets - a setpoint, a switch, a mode - use an input instead: Blaeck checks
and stores the value, and the host sees it back. See [Properties](properties.md).

## Handling a command

This is a command as it arrives on the serial port:

```
<Print,Hello,3>
```

Angle brackets around it, the name first, then the parameters, separated by commas. You can
type one into the serial monitor yourself.

You write a function that runs when it arrives, and register it in `setup()`:

```cpp
#include <Blaeck.h>

Blaeck device;

void onPrint(const char *command, const char *const *params, byte paramCount)
{
  if (paramCount != 2)
    return;
  for (int i = 0; i < atoi(params[1]); i++)
    Serial.println(params[0]);
}

void setup()
{
  Serial.begin(115200);
  device.begin(Serial);

  device.onCommand("Print", onPrint);
}

void loop()
{
  device.tick();
}
```

- The handler is a plain function, written outside `setup()` and `loop()`. `params` holds the
  parameters as text, and `paramCount` says how many arrived.
- `device.tick()` in `loop()` is what reads the serial port and calls the handler.
- Nothing is checked: the handler decides what the parameters mean, and what to do with ones
  that don't fit.

A plain command is not listed anywhere, so a host can send it but offers no control for it.
The name travels on the wire, so write it as an identifier: `SET_RANGE`, not `Set range`. It
may not start with `#` or `BLAECK.`.

## Buttons

A button is a command a host offers as a control. A press carries no value:

```cpp
void onStatus(const char *command, const char *const *params, byte paramCount)
{
  reportStatus();
}

device.onButtonCommand("STATUS", onStatus)
    .withDisplayName(F("Request status"))
    .diagnostic();
```

| Call | What it does |
|---|---|
| `withDisplayName(F("Request status"))` | Label shown instead of the name. The wire keeps using the name |
| `withIcon(F("mdi:tune"))` | A [Material Design Icons](https://pictogrammers.com/library/mdi/) name |
| `withDeviceClass(F("restart"))` | `restart`, `identify` or `update`; changes the icon and wording |
| `config()` | Files it as a setting |
| `diagnostic()` | Files it as something about the board rather than what it does |
| `disabledByDefault()` | Registered, but switched off until someone enables it |
| `withPressPayload(F("1,40"))` | Fixed parameters a press sends |

With a press payload, the handler reads `params[0]` and `params[1]` as it would from any other
sender. Nothing checks a press payload, so a typo in it is only found by what the handler does
with it.

Ordinary configuration strings are copied; their buffers can be reused after the call.

## Every command

`onAnyCommand()` registers one function that sees every command, the built-in `BLAECK.` ones
and a host's values for inputs included - for logging, or for forwarding commands elsewhere.

## Names

A command name belongs to one input, sensor, button or command on the whole board. A second one
is refused. Registering a plain command or button again under its own name replaces its
function.

## When a command is rejected

A rejected name, or a board out of RAM, drops the command.

```cpp
if (device.hasRejections())
{
  device.printRejections(&Serial);
}
```
