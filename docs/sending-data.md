# Sending data

The host sets the logging interval. Each signal decides whether to join it, filter unchanged
values, or report changes independently. Explicit writes are available for individual samples.

## A sketch that logs

```cpp
#include <Blaeck.h>

Blaeck device;

float temperature = 0.0;

float readSensor()
{
  return analogRead(A0) * 0.1;
}

void setup()
{
  Serial.begin(115200);
  device.begin(Serial);

  device.addSignal(F("Temperature"), &temperature);
}

void loop()
{
  temperature = readSensor();
  device.tick();
}
```

`device.tick()` reads incoming commands and services automatic reporting. By default, every
signal is included when the host's interval is due.

With these default policies, no data is sent until a host activates interval reporting:

```
<BLAECK.ACTIVATE,1000>      one reading per second
<BLAECK.DEACTIVATE>         stop interval reporting
```

Loggbok sends these for you. You can also type them into the serial monitor; the replies
carry binary data rather than readable values.

Two smaller calls do one half each. `device.read()` reads and dispatches;
`device.writeIfDue()` services both automatic reporting paths. Together they are `tick()`.

Your sketch cannot set the interval, but it can read what the host asked for:

```cpp
if (device.isTimedDataActive())
  Serial.println(device.getIntervalMs());
```

That is worth doing to show the interval on a state channel, or to remember it across a power
cut.

## Sending when something happens

Explicit writes bypass the signal's automatic policies and change thresholds. They work
without ACTIVATE but still honor pause/resume writes.

`writeAllData()` sends every signal now:

```cpp
if (temperature > 40.0)
  device.writeAllData();
```

`write()` sends one signal, and stores the value on the way:

```cpp
device.write("Temperature", readSensor());
```

Send both edges of a short-lived value, and the logged data says how long it lasted. Leave the
end of it to a host's timeout and that duration exists nowhere:

```cpp
bool pulse = false;
unsigned long pulseSince = 0;

void loop()
{
  device.tick();

  if (triggered() && !pulse)
  {
    pulse = true;
    pulseSince = millis();
    device.write("Pulse", pulse);
  }
  if (pulse && millis() - pulseSince >= 2000)
  {
    pulse = false;
    device.write("Pulse", pulse);
  }
}
```

`write()` finds the signal by walking the list and comparing names, which costs more the more
signals there are. Where it runs often, look the name up once and pass the number:

```cpp
int tempIndex = -1;

void setup()
{
  // ... after addSignal()
  tempIndex = device.findSignalIndex("Temperature");
}

void loop()
{
  device.write(tempIndex, readSensor());
}
```

## Per-signal reporting

Choose one interval policy when registering a signal:

```cpp
device.addSignal(F("Temperature"), &temperature)
    .writeAtInterval(BLAECK_ON_CHANGE, 0.05f);
```

| Policy | At each host interval |
| --- | --- |
| `writeAtInterval(BLAECK_ALWAYS)` | Send the current value; this is the default |
| `writeAtInterval(BLAECK_ON_CHANGE, delta)` | Send if it differs enough from the last sent value |
| `writeAtInterval(BLAECK_OFF)` | Do not include it |

Every `ACTIVATE` makes an initial interval report immediately due. It includes all
interval-enabled signals, without change filtering, even if they were sent before.
This also applies when `ACTIVATE` changes the interval while already active. Later
intervals apply the policies above normally. If writes are paused, the initial report
waits until they resume.

The initial report updates each included signal's shared baseline and rate-limit clock
normally. It does not reset or force signals with `writeAtInterval(BLAECK_OFF)`; their
independent change reporting and explicit writes continue as before.

Calling `writeAtInterval()` again replaces the previous interval policy. Its threshold
defaults to `BLAECK_ANY_CHANGE` (zero): any unequal value qualifies. Assign the variable
normally and keep calling `tick()`; there are no update flags to manage.

For changes that should not wait for the host interval:

```cpp
device.addSignal(F("Temperature"), &temperature)
    .writeAtInterval(BLAECK_OFF)
    .writeOnChange(0.1f, 100);
```

`writeOnChange(delta, minIntervalMs)` checks the current value on every `tick()` or
`writeIfDue()`. The threshold is required; the minimum interval defaults to 100 ms.
It is a rate limit since the last report, not a debounce timer. It works without ACTIVATE,
and DEACTIVATE does not stop it. Pause/resume writes still governs all data reporting.

Use `writeOnChange(BLAECK_ANY_CHANGE)` to report any difference from the last sent value,
still subject to the minimum interval. Numeric zero remains equivalent; it does not turn
reporting off. The same named threshold works in
`writeAtInterval(BLAECK_ON_CHANGE, BLAECK_ANY_CHANGE)`.

Disable immediate change reporting with `writeOnChange(BLAECK_OFF)`, without changing
the interval policy or blocking explicit writes:

```cpp
auto signal = device.addSignal(F("Temperature"), &temperature);
signal.writeOnChange(BLAECK_ANY_CHANGE, 250);
signal.writeOnChange(BLAECK_OFF); // Back to interval-only reporting.
signal.writeOnChange(0.1);       // Enable again, with the default 100 ms rate limit.
```

Disabling is harmless if already off and frees tracking storage when interval filtering
does not need it. Re-enabling replaces the threshold and minimum interval. If interval
filtering retained the shared baseline and clock, they are reused; otherwise the next
eligible report sends an initial value. `BLAECK_OFF` takes no minimum-interval argument.
Other mode values, such as `BLAECK_ALWAYS` and `BLAECK_ON_CHANGE`, are invalid for
`writeOnChange()` and produce a policy warning rather than acting as numeric thresholds.

The two paths are independent. For small changes at intervals and larger changes promptly:

```cpp
device.addSignal(F("Temperature"), &temperature)
    .writeAtInterval(BLAECK_ON_CHANGE, 0.05f)
    .writeOnChange(0.1f);
```

There is **one last-sent baseline**, not one per policy. An automatic report, explicit
`write()`, `writeAllData()`, or host-requested full snapshot updates that baseline and the
immediate rate-limit clock for every included signal. Explicit and immediate writes do not
move the host's interval schedule. If both paths qualify together, the signal appears only
once in one frame.

Interval reports carry an interval flag, including frames merged with immediate changes.
Explicit writes and immediate-only reports leave it clear, so a host can show data activity
without restarting its interval countdown.

Numeric thresholds are inclusive and measured from the last sent value, so small changes can
accumulate. Thresholds must be finite and nonnegative. Booleans and text ignore the threshold;
use `BLAECK_ANY_CHANGE`. Text comparisons retain an exact copy of the transmitted text,
up to the protocol's 255-byte limit, rather than a hash. Stable NaN representations do not continually resend;
transitions involving NaN, infinity or subnormal floating-point values qualify as changes.

Change tracking allocates a small record per opted-in signal. Text also retains a buffer
that grows when needed. Invalid policies and allocation failures are reported through the
debug stream and `hasRejections()` / `printRejections()`. A failed policy change leaves the
previous policy intact; a text-buffer allocation failure sends no partial data frame.

The first eligible report sends an initial value without applying its threshold or rate
limit. Baselines are not persisted across restart, and a new TCP host session refreshes them.
Initialize registered variables before servicing reporting. A locally accepted frame advances
the baseline; a short transport write invalidates affected baselines so they can be retried.
This is not an acknowledgment that the host received or stored the data.

Both change modes sample current values, not a queue. A value that changes and returns between
checks may never be reported. Use explicit `write()` calls when every measurement or edge
matters. If no signals qualify at an interval, no data frame is sent.

The [WriteModes example](../examples/WriteModes) demonstrates all these choices side by side.

## Reading the sensors at the right moment

The first sketch on this page reads its sensor on every pass of `loop()`. That works, and it
means every reading sent is up to one pass old. For interval snapshots, a callback can refresh
the variables before filtering:

```cpp
void readAllSensors()
{
  temperature = readSensor();
}

device.setBeforeWriteCallback(readAllSensors);
```

The callback runs when a host interval is due, even if filtering leaves nothing to send.
It also runs for explicit or host-requested `writeAllData()`. It does not run for single-signal
`write()` or the every-tick immediate change check; refresh those variables in your sketch.
It runs in normal `loop()` context, not an interrupt.

## Timestamps

[TimestampsRTC](../examples/more/TimestampsRTC) supplies wall-clock
timestamps using the UNO R4's RTC.

By default the data carries no time and the host stamps it when it arrives. That is fine when
the link is quick and nothing buffers.

For a time the device itself stands behind, pick a mode in `setup()`. `BLAECK_MICROS` needs
nothing else - the library reads `micros()` and counts the overflows, so the number keeps
climbing past the 71 minutes a 32-bit microsecond counter holds. Keep calling `tick()` or
`writeIfDue()` regularly even when no data qualifies, so rollovers can be observed. A sketch
that can sleep through a complete rollover needs a real clock instead:

```cpp
device.setTimestampMode(BLAECK_MICROS);
```

`BLAECK_UNIX` takes the time from a clock only your sketch can reach, so it needs a callback:

```cpp
unsigned long long unixMicros()
{
  return (unsigned long long)rtc.getEpoch() * 1000000ULL;
}

device.setTimestampCallback(unixMicros);
device.setTimestampMode(BLAECK_UNIX);
```

Without one it stamps zero and every reading lands in 1970. `hasValidTimestampCallback()` says
whether you have one.

Set the mode once, in `setup()`. Timestamps from either side of a change are not comparable.

An automatic frame timestamps the reporting pass, not each sensor's acquisition time.
For a known acquisition time, pass it explicitly to `write()` or `writeAllData()`.
`writeIfDue(timestamp)` also accepts a supplied timestamp; scheduling and rate limits still
use `millis()`.

## Buffered writes

Data is either assembled in RAM and sent in one call, or written out piece by piece as it is
produced. Buffering costs `60 + signals * 30` bytes of SRAM and suits a USB bridge that dislikes
many small writes; writing directly avoids that frame buffer. Change-tracking snapshots
are separate and are needed in either mode.

The default is per board: off on AVR, where the SRAM is scarce and the bridge chips take small
writes happily, and on everywhere else. Those defaults are there because of real faults on real
boards, so change this only with a reason:

```cpp
device.setBufferedWrites(true);
```

## The catalogs

A value on the wire names its signal by position, not by name. So before any of it means
anything, a host has to have the list of signals, and the lists of commands, state channels and
event channels with it.

Your sketch does not have to send these. They go out when the device starts, again whenever a
host asks, and again whenever you change what the device declares - always ahead of the next
value, so nothing is ever read against a list that has moved.
