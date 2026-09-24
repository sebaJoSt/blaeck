<picture>
  <source media="(prefers-color-scheme: dark)" srcset="extras/blaeck-dark.svg">
  <source media="(prefers-color-scheme: light)" srcset="extras/blaeck-light.svg">
  <img src="extras/blaeck-light.svg" alt="blaeck" height="75">
</picture>

---

blaeck is an Arduino library. It sends any value your sketch holds - sensor readings,
calculated results, text - over Serial or TCP as binary data, using the
[blaeck protocol](https://sebajost.github.io/blaeck-protocol/).

It is the first part of a chain:

1. **Your Arduino sketch** uses blaeck to register each variable it sends as a *signal* -
   a temperature, a counter, a switch position. You can also register the commands the board
   accepts and the events it fires.
2. **Loggbok**, a data logging tool, reads the signals over Serial or TCP and stores
   them in a database. It is also an MQTT bridge: it publishes the signals and commands to a
   broker.
3. **Home Assistant** subscribes to that broker and creates one entity for each: a sensor for
   a signal, a slider or button for a command.

Because your sketch declares what it has, the host can discover its signals and controls.
A signal with a unit arrives in Home Assistant as a sensor with that unit. The connection,
MQTT broker and Home Assistant integration still need to be configured.

Loggbok is an internal tool and is not publicly released. The protocol is documented, so you
can write your own host. You can also send commands from a serial monitor or TCP terminal.

**Version 7.0.0 is in development and has not been released.**

## A first sketch

These two sketches send the same simulated temperature and pressure readings.
Choose Serial or TCP.

### Serial

Connect the host to the board's serial port at **115200 baud**.

```cpp
#include <Blaeck.h>

Blaeck device;

float temperature;
long pressure;

void setup()
{
  Serial.begin(115200);
  device.begin(Serial).withSignals(2);

  device.DeviceName = "Weather Station";

  device.addSignal(F("Temperature"), &temperature);
  device.addSignal(F("Pressure"), &pressure);
}

void loop()
{
  temperature = 20.0f + random(100) / 10.0f;
  pressure = 1000 + random(30);

  device.tick();
}
```

### TCP

This version uses an Arduino Ethernet shield and the **Ethernet** library with DHCP.
The example MAC address must be unique on your network. Connect the host to the IP
address printed on Serial, on **TCP port 23**.

```cpp
#include <Blaeck.h>
#include <Ethernet.h>

byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
EthernetServer server(23);
Blaeck device;

float temperature;
long pressure;

void setup()
{
  Serial.begin(115200);

  if (Ethernet.begin(mac) == 0)
  {
    Serial.println(F("DHCP failed. Check the Ethernet connection and restart."));
    while (true)
      delay(1000);
  }

  server.begin();
  device.begin(server).withSignals(2).withDebugStream(&Serial);

  device.DeviceName = "Weather Station";

  device.addSignal(F("Temperature"), &temperature);
  device.addSignal(F("Pressure"), &pressure);

  Serial.print(F("Connect on port 23 at "));
  Serial.println(Ethernet.localIP());
}

void loop()
{
  temperature = 20.0f + random(100) / 10.0f;
  pressure = 1000 + random(30);

  device.tick();
  Ethernet.maintain();
}
```

Three calls do the work:

- `begin(Serial)` hands blaeck the serial port you opened. On a board with more than one
  port you can pass `Serial1` instead. For TCP, `begin(server)` takes the listening server
  you started. `.withSignals(2)` reserves room for two signals.
- `addSignal(...)` registers a variable. blaeck keeps a pointer to it and reads it
  whenever it sends data, so you only have to keep the variable up to date.
- `tick()` reads incoming commands and sends the values when they are due. Call it in every
  `loop()`.

The host decides how often data is sent. It sends `<BLAECK.ACTIVATE,1000>` to get one frame
per second, and `<BLAECK.DEACTIVATE>` to stop interval-driven data.
The data frames are binary, not readable text in a terminal.

TCP sketches need the networking library for their board; see [networking](docs/network.md) for supported server requirements
and the optional TelnetStream setup.

## Documentation

| Guide | What it covers |
|---|---|
| [Signals](docs/signals.md) | Registering values, naming them, and describing how they are shown |
| [Commands](docs/commands.md) | Reacting to commands, and declaring them as controls |
| [State channels](docs/state-channels.md) | Reporting a value that is displayed but not logged |
| [Events](docs/events.md) | Reporting that something happened |
| [Sending data](docs/sending-data.md) | Intervals, sending it yourself, timestamps, buffered writes |
| [Configuration](docs/configuration.md) | Table sizes and compile-time settings |
| [Networking](docs/network.md) | Servers, client limits, terminal output and transport errors |

## Examples

The examples are in `examples/`. In the Arduino IDE, open them with
**File > Examples > blaeck**.

The main examples include a [NetworkSetup.h tab](examples/Basic/NetworkSetup.h) with
ready-made Ethernet setup for Mega/GIGA shields and ESP32-PoE/WT32-ETH01 boards.
Leave `USE_TCP` at `0` for Serial or set it to `1` for TCP and configure the included tab.
Using `NetworkSetup.h` is optional. You can use your own networking libraries and setup instead.

Start with **Basic**, then **Signals** and **Commands**. Follow with **StateChannels** and
**EventChannels**, then **WaveformGenerator** to see the pieces working together.

| Example | What it teaches |
|---|---|
| [Basic](examples/Basic) | The smallest sketch that logs two values |
| [Signals](examples/Signals) | Numeric, boolean and text signals, metadata, and numbered arrays |
| [Commands](examples/Commands) | Plain commands and typed dashboard controls |
| [StateChannels](examples/StateChannels) | Values shown but never logged, from variables, getters or explicit writes |
| [EventChannels](examples/EventChannels) | Declaring and reporting occurrences |
| [WaveformGenerator](examples/WaveformGenerator) | A complete, controllable waveform dashboard |
| [WriteModes](examples/WriteModes) | Immediate writes versus updated-only data sent on the host's interval |
| [more / ConfigurableSignals](examples/more/ConfigurableSignals) | Choose which signals to log through commands and save the selection in EEPROM |
| [more / SHT31TempHumiditySensor](examples/more/SHT31TempHumiditySensor) | Read a real temperature and humidity sensor; requires Adafruit SHT31 |
| [more / TimestampsRTC](examples/more/TimestampsRTC) | Wall-clock timestamps using the UNO R4's RTC |
| [more / TimestampsNTP](examples/more/TimestampsNTP) | Network-synchronized timestamps |
| [more / WiFi](examples/more/WiFi) | WiFi-specific connection setup |
| [more / ESP32C6BugBoard](examples/more/ESP32C6BugBoard) | An ESP32-C6 board-specific example |
| [more / BridgeESP32PoE](examples/more/BridgeESP32PoE) | A bridge between a UART and TCP |

## Reference

Public API documentation is in `src/Blaeck.h`. Your editor shows it when you hover over a call.

The frame formats are described in the
[blaeck protocol specification](https://sebajost.github.io/blaeck-protocol/).

## Help and licence

For questions and bug reports, see [SUPPORT.md](SUPPORT.md). To contribute, see
[CONTRIBUTING.md](CONTRIBUTING.md). blaeck is licensed under the MIT licence
([LICENSE.md](LICENSE.md)).

The internal CRC32 helper is adapted from
[Rob Tillaart's CRC library](https://github.com/RobTillaart/CRC), with its MIT notice
retained in `src/detail/BlaeckCRC32.h`.
