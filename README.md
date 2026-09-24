# blaeck

One Arduino package, one device class, one protocol core. Attach a **Blaeck** device to a
Stream or to an already-started TCP server. **Version 7.0.0 is in development and has
not been released.**

Based on BlaeckSerial `25b5434e3433d6c46309a75beb244b389d8dec80` and
BlaeckTCP `68ca1a19c0f7139bb7e496333541476b2bccd20f`.
The original repositories are unchanged.

## One public API

```cpp
#include <Blaeck.h>

Blaeck device;
float Temperature = 21.0f;

void setup()
{
  Serial.begin(115200);
  device.begin(Serial).withSignals(1);
  device.addSignal(F("Temperature"), &Temperature);
}

void loop()
{
  device.tick();
}
```

For TCP, supply a listening server instead:

```cpp
#include <Blaeck.h>
#include <Ethernet.h>

EthernetServer server(23);
Blaeck device;

void setup()
{
  // Bring Ethernet up first.
  server.begin();
  device.begin(server).withClients(4).withSignals(1);
  // Register the same signals, commands, state and events here.
}

void loop()
{
  device.tick();
}
```

Streams and servers are passed by reference, not pointer or port number. There are no
public `BlaeckSerial`/`BlaeckTCP` classes or transport-specific headers in this library.
Use `Blaeck.h` even when the original standalone libraries remain installed.
The device and handle types share the `blaeck` namespace; `Blaeck.h` also makes them
available without qualification, so sketches still write `Blaeck device;`.

**No mandatory third-party library dependencies.** Stream-only sketches use the Arduino
core alone. TCP users supply their chosen networking library explicitly.
CRC32 is bundled in `src/detail/BlaeckCRC32.h`, adapted from
[Rob Tillaart's CRC library](https://github.com/RobTillaart/CRC) with its MIT license
retained in the file. Its fixed protocol parameters preserve the existing checksum format.

**TelnetStream is optional:** include `<TelnetPrint.h>`, bring the network up, then use:

```cpp
TelnetPrint.begin();
device.begin(TelnetPrint);
```

That retains its existing networking selection and wrappers without making it a Blaeck
dependency. Direct-server PlatformIO projects use normal dependency discovery; TelnetStream
projects may still need `lib_ldf_mode = deep`.

## Examples

There is one set of topic sketches:

`Basic`, `Signals`, `Commands`, `StateChannels`, `EventChannels`, `WriteModes`,
and `WaveformGenerator`.

Each sketch shows both connection choices directly. Serial is the default; set **USE_TCP**
to `1` at the top of the sketch to select TCP. The networking tab provides the board's
server type as **NetworkSetup::Server**:

```cpp
#if USE_TCP
#include "NetworkSetup.h"
NetworkSetup::Server server(23);
#endif
```

In `setup()`, both paths use the normal device API:

```cpp
Serial.begin(115200);

#if USE_TCP
networkBegin(23);
server.begin();
device.begin(server).withSignals(2);
#else
device.begin(Serial).withSignals(2);
#endif

// Shared signal/command/state/event registration follows.
```

Only connection setup and its short table-sizing chain are repeated. There is no
ConnectionSetup wrapper. In `loop()`, the device's tick method runs Blaeck; a `#if USE_TCP`
block calls `networkLoop()` for network maintenance. All feature code stays in one sketch.

Uncomment `#define NETWORK_WITH_SERVICES` in the TCP include block for OTA/Bonjour. The
[WaveformGenerator README](examples/WaveformGenerator/README.md) covers prerequisites.
Services are off by default. GIGA QSPI OTA storage remains embedded in `NetworkSetup.h`.

The `more` submenu holds hardware-specific sketches: ConfigurableSignals, SHT31TempHumiditySensor,
TimestampsRTC, TimestampsNTP, WiFi, ESP32C6BugBoard, and BridgeESP32PoE.

Sketches remain self-contained. Edit Basic's NetworkSetup.h, then synchronize existing copies:

```powershell
python extras\scripts\syncnetwork.py
python extras\scripts\syncnetwork.py --check
```

## Behavior and compatibility

- `Blaeck` is one concrete class owning the catalog and active connection, with no core
  base class or virtual transport hooks. `Blaeck.cpp` implements protocol/catalog logic
  and lifecycle; `BlaeckTransport.cpp` implements Stream and TCP session handling.
- Both `begin()` overloads return the same `BlaeckBeginRef` handle. Table sizing,
  `.withClients()` and `.withDebugStream()` can be chained in any order.
- The small typed server adapter in `src/detail/BlaeckServerAdapter.h` is header-defined.
  It requires proper `accept()` behavior and never guesses with `available()`.
- One device uses one connection at a time. `begin(otherConnection)` detaches the old one;
  separate `Blaeck` objects can use separate connections with independent catalogs.
- The caller owns and starts the stream/server. Keep it alive while attached.
  `end()` and destruction close accepted TCP clients but never stop the supplied stream/server.
  No second consumer may accept from that same server.
- `begin()` resets the signal catalog as before: attach first, then register signals.
  Partial input from an earlier attachment is discarded.
- `Terminal` writes text to the attached Stream, or only to TCP terminals (never TCP hosts).
  Connection callbacks and `.withClients()` are TCP-specific. Using `.withClients()` for a
  Stream reports `NotServer` rather than silently pretending there are multiple clients.
- Stream buffering defaults to off on AVR and on elsewhere. TCP buffering defaults to on.
  The default is selected when attaching. An explicit `setBufferedWrites()` selection is
  retained across later `begin()` calls.
- **Both connection types report `blaeck` version `7.0.0`.** The lowercase library name
  matches the package brand. The C++ class remains `Blaeck`, with header `Blaeck.h`.
  Uppercase protocol framing and command names (`<BLAECK:`, `BLAECK.GET_DEVICES`, etc.)
  are unchanged.
- Hosts must recognize the exact `blaeck` identity and its v7 capabilities. Loggbok's
  updated source supports this identity over Serial and TCP; older Loggbok releases
  may not. Legacy `BlaeckSerial` and `BlaeckTCP` identities are supported there through v6.

See [network](docs/network.md) for server requirements, storage, ownership and errors.

## Configuration

Use one **BlaeckConfig.h**, loaded by the internal `detail/BlaeckDefaults.h` and visible to
sketch and library translation units. Shared feature flags and command-buffer settings
apply to both connection types. Legacy
`BlaeckSerialConfig.h`/`BlaeckTCPConfig.h` files cause an explicit migration error.

Independent defaults remain available:

```cpp
#define BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT false
#define BLAECK_TCP_BUFFERED_WRITES_DEFAULT true
```

`BLAECK_BUFFERED_WRITES_DEFAULT` overrides both unless a connection-specific default is set.
`BLAECK_USB_PACKET_BYTES` applies only to streams; `BLAECK_TCP_NO_DELAY_DEFAULT` applies only
to servers/clients supporting that setting. See [configuration](docs/configuration.md).

## Validation status

For contribution guidelines and editor setup, see [CONTRIBUTING.md](CONTRIBUTING.md).
For reporting problems, see [SUPPORT.md](SUPPORT.md).

**This unified-public-class revision has source checks only, as requested. It has not
been compiled or runtime-tested.** Build and size results from the earlier two-class
prototype do not validate this revision.

```powershell
python extras\scripts\checkprototype.py
```

Updated fixtures are ready for separately authorized validation:

- `extras/tests/SerialOnly`: Stream attachment with no concrete networking library.
- `extras/tests/Combined`: two Blaeck devices, both connection types, multiple translation units.
- `extras/tests/TelnetServer`: the optional TelnetPrint route.
- `extras/tests/host`: simulated clients/streams using the real protocol and transports.
  Assertions cover the concrete class and unified begin handle, routing, reuse, failures,
  teardown, connection changes, defaults and explicit overrides, unified wire identity,
  CRC32 reference vectors and incremental updates, USB padding, and rejection of
  `available()`-only servers.

The host suite is run with a C++ compiler; no external CRC library is needed:

```powershell
python extras\scripts\testserver.py
```

No firmware has been uploaded. Hardware interoperability, OTA operation and release
readiness remain unverified. No builds, flashing, commits, pushes or publishing without
authorization.
