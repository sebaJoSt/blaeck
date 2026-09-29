# Configuration

This library uses one shared configuration for Serial and TCP. Include `Blaeck.h` first
when the old standalone libraries are also installed. The examples below use the Serial
transport; TCP uses `begin(server)` after `server.begin()`, and the same core settings.

One thing is decided outside the code that uses it: which parts of the library are built at all.

## Tables

Everything you register lives in a table that grows as you add to it, eight entries at a time.
There is nothing to size. `begin()` returns a handle for the connection limit and the debug
stream, and both are optional:

```cpp
void setup()
{
  Serial.begin(115200);

  device.begin(Serial).withDebugStream(&Serial1);
}
```

Call `begin()` once per instance, normally in `setup()`. A later call reports
`BeginAlreadyCalled` and does not change the transport or apply its chained settings.
This also applies after `end()` or failed initialization; a latched `OutOfMemory`
error is retained. `end()` closes the transport; it does not reset the instance for
another `begin()`.

RAM is the only limit. On AVR, with the default settings, a signal costs 12 bytes, an event type
6, an event channel 13, a device 10, a property about 40 and a command 66 - the largest there is.
A signal with a unit, icon or other description takes 18 bytes more. Every eight entries of a
table share about 4 bytes of bookkeeping, and the last group of eight may hold up to seven
unused slots. Each configuration string passed as ordinary RAM text is copied, at about its
length plus 7 bytes (a reference count, the terminator and the heap's own header); an `F()`
string costs nothing extra. A Mega's 8 kB is gone at a few hundred of anything, where an ESP32
has room for thousands. A property's presentation - unit, label, icon, device class - takes
about 20 bytes more, only once it has one.

Entries are added in `setup()`, and a table never moves or frees what it holds, so the heap
doesn't fragment. `clearAllSignals()` and the other clear calls keep the space and reuse it.
When RAM runs out, the entry is dropped and reported by `hasRejections()`, `printRejections()`
and the debug stream.

## Configuration text

Ordinary strings work throughout the API. Names, metadata, command state links, press payloads
and event types supplied through registration or configuration calls are copied when stored.
Their buffers can then be changed or discarded. `F()` remains optional and avoids these copies.
An allocation failure leaves an existing setting unchanged and is reported by `hasRejections()`,
`printRejections()` and the debug stream.

Signal and bound state **values**, text getters and the device identity fields below retain
their existing lifetime requirements; they are not copied configuration.

## Finding out what did not fit

An entry that can't be added is dropped. Your sketch still runs, still logs, and is simply
missing a signal or a control - which is why it is worth asking.

`withDebugStream()` names a stream for the library to report on. Enable it before registration
to see why an entry was rejected. The debug stream may be the same stream the data goes to.

On a board with only one `Serial`, end `setup()` with this instead:

```cpp
device.printRejections(&Serial);
```

It prints one line per table with registration rejections, and nothing when there were none:

```
Blaeck registration rejections:
  3 signal registrations rejected.
  Possible causes include invalid or conflicting names, invalid event types, or insufficient memory.
  Enable withDebugStream() before registration for details.
```

It is safe on the Blaeck stream because no data has been written yet at the end of `setup()`.

To ask in code: `hasRejections()` for any table at all, or `hasRejectedSignals()`,
`hasRejectedCommands()` and `hasRejectedEventChannels()` for one.

## Naming the device

Three fields say what the device is. A host lists it by the name, and groups its signals and
controls under it:

```cpp
device.DeviceName = "Waveform Generator";
device.DeviceHWVersion = "Weather Station PCB v2"; // Optional hardware-name override
device.DeviceFWVersion = "1.0";
```

The device name starts as `"Unnamed"` and the firmware version as `"n/a"`. A device name set
to an empty string or to null goes out as `"Unnamed"` too.

The hardware name defaults to the board selected when compiling. Mega 2560, Uno, Nano,
Leonardo, Micro, GIGA R1, UNO R4 WiFi/Minima, MKR Zero and Nano ESP32 have friendly names.
Other targets use the core's `ARDUINO_BOARD` string if available, otherwise `"n/a"`.
This is the build target, not detection of the physical board, its manufacturer or PCB
revision. A generic ESP32 target therefore reports its generic identifier.

Omit `DeviceHWVersion` in a portable sketch, or assign it to describe your own hardware as
above. The default is set only at construction; `begin()` does not reset an override.
All three fields are kept as pointers rather than copied, so a quoted literal is always safe
and a name built at runtime has to live in a global buffer.

## Compile-time settings

The rest are `#define`s. Three of them switch a feature off, which reclaims the flash and SRAM
it would have cost:

| Define | Set to 0 to drop |
|---|---|
| `BLAECK_ENABLE_SIGNAL_META` | Everything a signal declares about itself. Saves 2 bytes of SRAM per signal on AVR, and 18 per described signal |
| `BLAECK_ENABLE_COMMAND_META` | Everything a button declares beyond its name |
| `BLAECK_ENABLE_EVENTS` | Events |

Your sketch needs no `#ifdef` around any of it. The calls still compile and simply store
nothing, so the same sketch builds either way.

These change a default:

| Define | Default |
|---|---|
| `BLAECK_COMMAND_MAX_CHARS_DEFAULT` | 128, or 48 on a small AVR. Bytes per command buffer, including the terminating null byte. Two fixed buffers plus one per allocated TCP slot |
| `BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT` | `false` on AVR, `true` everywhere else |
| `BLAECK_TCP_BUFFERED_WRITES_DEFAULT` | `true` on every board |
| `BLAECK_TCP_NO_DELAY_DEFAULT` | `true`. Applied to supplied servers and accepted clients that provide `setNoDelay()` |
| `BLAECK_TCP_STOP_TIMEOUT_MS` | 100. How long closing a connection waits for the peer, on clients that provide `setConnectionTimeout()` |
| `BLAECK_BUFFERED_WRITES_DEFAULT` | Optional build-wide override for both transports, unless a transport-specific default is supplied |
| `BLAECK_USB_PACKET_BYTES` | 64. The USB packet size a frame is padded away from, so it never ends on a full one and stalls in the host. Lower it to match a core built with a smaller endpoint |
| `BLAECK_STATE_MAX_OPTION_CHARS` | 24. Room for one resolved select option while a frame is built |

Command buffers support configured sizes from 1 to 65535 bytes, subject to the board's
RAM and compiler object-size limits. The maximum command content is one byte less than
the buffer size; `<` and `>` are not stored. Oversized commands are rejected rather than
executed as fragments. Larger buffers do not raise the separate command-name,
parameter-count, or typed text-value limits.

Buffer storage alone uses twice the configured size for Serial, or `(2 + client slots)`
times the size for TCP. With four TCP slots, the default 128-byte buffers use 768 bytes;
300-byte buffers use 1800 bytes. Increasing the size does not happen automatically.

> [!IMPORTANT]
> An override has to reach **both** your sketch and the library's compiled source files.
> Some settings size members of `Blaeck`. A setting seen by only one gives the class two
> different layouts, and that corrupts memory without a word about it.
> `BLAECK_USB_PACKET_BYTES` sizes nothing, but the code that reads it sits in
> `BlaeckTransport.cpp`, so a sketch-only override does not change its behavior.
>
> So do not `#define` them at the top of your `.ino`. That reaches your sketch and never the
> library.

### PlatformIO

```ini
build_flags = -DBLAECK_COMMAND_MAX_CHARS_DEFAULT=128
```

This reaches every file. Nothing else to do.

### Arduino IDE and arduino-cli

blaeck includes a `BlaeckConfig.h` if it can find one. Legacy `BlaeckSerialConfig.h` and
`BlaeckTCPConfig.h` files visible to the library cause an explicit migration error:
move their common settings into one `BlaeckConfig.h`. In an Arduino build it
cannot: your sketch folder is not on the compiler's include path, so a config file next to your
`.ino` is ignored without a word. That is the Arduino build system, not this library
([arduino-cli#501](https://github.com/arduino/arduino-cli/issues/501)).

The simplest fix is to put the config where the include path already reaches, at
`libraries/blaeck/src/BlaeckConfig.h`. It works in the IDE and the CLI. The catch is
that it now belongs to the library: it applies to every sketch you build, and updating the
library overwrites it.

To keep the config with the sketch instead, put it in the sketch folder and add the folder to
the include path yourself:

```bash
arduino-cli compile --fqbn <board> \
  --build-property "compiler.cpp.extra_flags=-I{build.source.path}" \
  MySketch
```

`{build.source.path}` is your sketch folder. Nothing in your Arduino installation is touched,
and this is the one your CI can reproduce. The IDE has no place to pass that flag, so building
there needs the same line in a `platform.local.txt` next to your core's `platform.txt` - per
core, so once for avr, again for esp32, and so on.
