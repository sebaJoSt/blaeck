# Commands

A command is a name with parameters, sent to the sketch: run a calibration, print a report.
Commands are not entities, so Home Assistant shows no control for them. Controls are described
in [Entities](entities.md).

## Handling a command

A command arrives on the serial port as:

```
<Print,Hello,3>
```

Angle brackets enclose it, the name comes first, and commas separate the parameters. The serial
monitor sends one as typed.

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

  device.onCommand("Print", onPrint);   // <Print,Hello,3>
}

void loop()
{
  device.tick();
}
```

- `params` holds the parameters as text, `paramCount` their number. A percent-encoded
  parameter (`%2C` for a comma) arrives decoded.
- `tick()` reads the serial port and calls the handler.
- Parameters are not checked; the handler decides what they mean.

## Every command

`onAnyCommand()` sets one function that sees every command: the built-in `BLAECK.` ones, values
for controls, button presses and refused commands. It runs after the command's own
handler. A board has one such function; a second call replaces the first.

The return value tells whether the function took the command. A command nothing else knows is
then acknowledged as accepted. A function that only logs returns `false`, so a mistyped name is
still answered as unknown.

One function does all the work, here logging every command to `Serial1` and forwarding the
`PUMP_` family to `Serial2`:

```cpp
bool handleAnyCommand(const char *command, const char *const *params, byte count)
{
  Serial1.print(F("Command: "));
  Serial1.println(command);

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

device.onAnyCommand(handleAnyCommand);   // <PUMP_SPEED,75> goes out on Serial2 as PUMP_SPEED,75
```

## Names

A command name belongs to one control, sensor or command on the whole board; a second one
is refused. Names hold only letters, digits, `_`, `-` and `.` (`SET_RANGE`, not `Set range`) and
can't start with `BLAECK.`. A command registered again under its own name gets the new function.

## When a command is rejected

A rejected name, or a board out of RAM, drops the command.

```cpp
if (device.hasRejections())
{
  device.printRejections(&Serial);
}
```
