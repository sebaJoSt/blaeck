# Network

Use the same Blaeck device API with a server instead of a Stream. Network mode adds
multiple clients and distinguishes protocol hosts from text terminals.

## Starting the server

The sketch owns the listening server. Bring the network up, start the server, then pass
it to `device.begin(server)`:

```cpp
#include <Blaeck.h>
#include <Ethernet.h>

EthernetServer server(23);
Blaeck device;

void setup()
{
  // mac is the sketch's configured MAC address.
  Ethernet.begin(mac);
  server.begin();
  device.begin(server)
      .withClients(4)
      .withSignals(50)
      .withDebugStream(&device.Terminal);
}
```

`withClients()` sets how many connections are accepted at once, hosts and terminals
together; the default is 4. Each connection takes a receive buffer, 128 bytes on a Mega. A
connection beyond the limit is closed straight away. Like the table sizes, the number is
fixed once the first `read()` or `tick()` has run.

The server must outlive its attachment to the device. Only one consumer may accept clients from
it. Separate Blaeck objects can use separate servers, with independent client slots,
catalogs and timing. `device.end()` closes accepted clients and releases transport storage
without stopping or deleting the listening server. Destruction does the same. Calling
`begin(otherServer)` first detaches the old server this way; explicit teardown does not
invoke the disconnect callbacks. A new `begin()` retains the previous client limit unless
the new chain changes it. Register signals again after `begin()`, as before.

### Optional TelnetStream

TelnetStream is not required by Blaeck. To retain its network selection and wrappers,
include its header explicitly and pass its global server:

```cpp
#include <Blaeck.h>
#include <Ethernet.h>     // Or the network library appropriate to the board.
#include <TelnetPrint.h>

Blaeck device;

void setup()
{
  // Bring the network up first.
  TelnetPrint.begin();   // Its default port is 23.
  device.begin(TelnetPrint).withClients(4);
}
```

For a different port, configure TelnetPrint using its concrete server's API before attaching
it. Never reassign or restart a server while Blaeck is using it.

The supplied server must have `accept()` returning a default-constructible, assignable
client value derived from Arduino's `Client`, with `remoteIP()` and `remotePort()` for
diagnostics. Each new connection must be returned only once, even before it sends data.
There is no `available()` fallback. A server lacking `accept()` fails to compile; use a
compatible server or a verified wrapper. Passing TelnetPrint retains its existing selected
type, not a guarantee that every possible TelnetStream platform satisfies this contract.

`BLAECK_TCP_NO_DELAY_DEFAULT` defaults to `true` and is applied wherever the server or client
supports `setNoDelay()`. Servers and clients without that tuning method need no wrapper.

### Storage and errors

One typed adapter is allocated when attaching. Client objects and Blaeck session state are
allocated as two arrays on the first read, using the configured client count. Connections
reuse these slots; Blaeck does not allocate a new client wrapper on each accept. Network
libraries may still allocate their own socket storage.

`transportError()` returns `None`, `NotStarted`, `OutOfMemory`, `InvalidClientCount`,
`ClientLimitLocked`, or `NotServer`. A zero client count is rejected, retaining the previous
limit; calling `.withClients()` after `begin(stream)` reports `NotServer`.
Allocation failure is latched: no clients are accepted and no tight allocation retry loop
runs.

Errors are reported once to a configured debug stream when possible. A terminal cannot
report an allocation failure that prevented it from being accepted, so use a separate
Serial debug stream or call `device.printTransportError(&Serial)` after `read()`/`tick()`.
This transport status is separate from the core's table-registration `hasRejections()`.

## Hosts and terminals

Every connection starts as a **terminal**. It becomes the **host** when it sends a command
whose name starts with `BLAECK.`, such as `<BLAECK.GET_DEVICES>`, and stays the host until it
disconnects or another connection takes over.

| | Host | Terminal |
|---|---|---|
| Typical client | Loggbok, blaecktcpy | PuTTY, telnet |
| How many | one | the remaining connections |
| Receives frames | yes | never |
| Receives text from `device.Terminal` | no | yes |
| Its commands | run and acknowledged | run, not acknowledged |

There is one host at a time, as on a serial port. When another connection sends a `BLAECK.`
command, it becomes the host and the device closes the previous host's connection. That
connection is reported like any other disconnect, and on the debug stream:

```text
Client #2 is the host
Client #0 disconnected: replaced as host
```

Takeover requires an accepted connection. A new connection needs a free slot; when all
`withClients()` slots are occupied, it is closed before any command is read. With
`.withClients(1)`, the existing connection must close and its slot be released first.

When a slot is available, a reconnecting host can replace its own dead connection without
waiting for the network stack to notice the dropped link. Two hosts that both reconnect on
their own can keep taking over from each other; point only one host at a device.

Closing sends the old host a FIN at once. Where the client supports `setConnectionTimeout()`,
as the Ethernet library's does, the wait for a peer that no longer answers is bounded by
`BLAECK_TCP_STOP_TIMEOUT_MS` (100 ms by default) instead of the library's own second.

The [Connections page](https://sebajost.github.io/blaeck-protocol/protocol/connections) of
the protocol specification is the full contract, for anyone writing a host.

## device.Terminal

In network mode, `device.Terminal` prints to every connected terminal and never to a host.
With `begin(stream)`, it writes text to that Stream instead. Use it like `Serial`:

```cpp
void onLED(const char *command, const char *const *params, byte paramCount)
{
  setLed(atoi(params[0]) == 1);
  device.Terminal.println(ledState ? "LED is ON." : "LED is OFF.");
}
```

Passed to `withDebugStream()`, it also shows what the library refuses and why, every command
that arrives, and connections opening and closing. That makes a terminal the easiest way to
see what a device is doing while a host is connected.

A terminal that connects but never reads can fill its send buffer, and a write to it may then
wait. Short lines don't get there.

## When a connection drops

A connection that closes properly frees its slot at once, and
`setClientDisconnectedCallback()` is called. One that dies without closing - a pulled cable,
a laptop going to sleep - is noticed only when the network stack gives up on it.

A host that reconnects starts as a terminal again, and receives nothing until it sends a
`BLAECK.` command. Loggbok does that as soon as it reconnects, so data continues, and the
device's interval and pause are unchanged. If the board restarted while the connection was
down, the restart notice goes to the first host that connects afterwards.

`setClientConnectedCallback()` and `setClientDisconnectedCallback()` receive the connection's
slot, starting at 0. They are for the sketch's own use, such as a status LED.

## Boards

`Basic` and the topic examples run on these boards through a shared `NetworkSetup.h` tab:

| Board | Network |
|---|---|
| Arduino Mega 2560 | Ethernet shield (W5100/W5500) |
| Arduino Giga R1 | Ethernet shield |
| Olimex ESP32-PoE | built-in Ethernet |
| WT32-ETH01 | built-in Ethernet |

Each topic sketch shows both Serial and TCP setup. Set `USE_TCP` to `1` in the main sketch
to include the board-specific NetworkSetup tab. It defines `NetworkSetup::Server` as an
alias for the board's actual server class, not a Blaeck-specific server implementation.
The sketch starts the server and calls `device.begin(server)`; the other branch calls
`device.begin(Serial)`. These examples do not need TelnetStream.

The `more` folder includes additional networking examples:
[WiFi](../examples/more/WiFi) gets the UNO R4 WiFi and ESP32 boards online, and
[ESP32C6BugBoard](../examples/more/ESP32C6BugBoard) sets the pins for the
ESP32-C6-Bug's Ethernet add-on. These use their own network setup.

The hardware name defaults to the selected build target, with a friendly name for recognised
boards or the core's board identifier otherwise. It is not physical board detection.
`DeviceHWVersion` can still describe custom hardware: the C6 Bug example overrides it because
its generic ESP32C6 build target cannot identify that board or its wiring.

## Network timestamps

The default `BLAECK_NO_TIMESTAMP` mode lets the host timestamp data on arrival.
`device.setTimestampMode(BLAECK_MICROS)` uses device uptime instead; neither needs a clock
source. [TimestampsNTP](../examples/more/TimestampsNTP) uses the ESP32 network clock on an
ESP32-PoE or WT32-ETH01. It waits for synchronization before starting the TCP server and reports
failed attempts on the serial monitor, rather than sending readings dated 1970. It needs
network access to `pool.ntp.org`.

## A serial device on the network

[BridgeESP32PoE](../examples/more/BridgeESP32PoE) forwards bytes unchanged between one TCP
connection and a UART. The device behind it uses `device.begin(Serial)` (or the original
BlaeckSerial library); the bridge does not instantiate Blaeck or interpret its frames.

## Updates over the network

Topic examples default to Serial with services off. Set `USE_TCP` to `1` and uncomment
`#define NETWORK_WITH_SERVICES` before including NetworkSetup.h for OTA/Bonjour.
Each board needs a one-time setup
first; see the
[WaveformGenerator README](../examples/WaveformGenerator/README.md).

Giga OTA storage is included in `NetworkSetup.h`; no additional sketch file is needed.
Its `Arduino_Portenta_OTA` dependency is required only for Giga builds with services enabled.
