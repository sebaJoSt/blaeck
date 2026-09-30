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
  parameters as text, and `paramCount` says how many arrived. A host may percent-encode a
  parameter (`%2C` for a comma); it arrives decoded.
- `device.tick()` in `loop()` is what reads the serial port and calls the handler.
- Nothing is checked: the handler decides what the parameters mean, and what to do with ones
  that don't fit.

A plain command is not listed anywhere, so a host can send it but offers no control for it.
The name travels on the wire, so it may hold only letters, digits, `_`, `-` and `.`:
`SET_RANGE`, not `Set range`. It may not start with `BLAECK.`.

## Buttons

A button is a press a host offers as a control. It carries no value, so its function takes no
parameters, and parameters sent with a press are ignored:

```cpp
void onStatus()
{
  reportStatus();
}

device.addButton("STATUS", onStatus)
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

A host presses it by sending its name, `<STATUS>`. Parameters sent with a press are ignored.
For a press with fixed arguments, pass a lambda:

```cpp
device.addButton("ACTIVATE_ALL", []() { activateRange(1, 40); })
    .withDisplayName(F("Activate all"));
```

Ordinary configuration strings are copied; their buffers can be reused after the call.

## Every command

`onAnyCommand()` registers one function that sees every command, the built-in `BLAECK.` ones,
a host's values for inputs and refused commands included - for logging, or for forwarding
commands elsewhere. It returns whether it took the command: a command nothing else has is then
acknowledged as accepted. A function that only logs returns `false`, so a mistyped name is
still answered as unknown.

```cpp
bool forwardPump(const char *command, const char *const *params, byte count)
{
  if (strncmp(command, "PUMP_", 5) != 0)
    return false;
  Serial2.print(command);
  for (byte i = 0; i < count; i++)
  {
    Serial2.print(',');
    Serial2.print(params[i]);
  }
  Serial2.println();
  return true;
}

device.onAnyCommand(forwardPump);
```

## Names

A command name belongs to one input, sensor, button or command on the whole board. A second one
is refused. A host sends the name, so it may hold only letters, digits, `_`, `-` and `.`; a
label with spaces or other characters goes in `withDisplayName()`. Registering a plain command or button again under its own name replaces its
function.

## When a command is rejected

A rejected name, or a board out of RAM, drops the command.

```cpp
if (device.hasRejections())
{
  device.printRejections(&Serial);
}
```
