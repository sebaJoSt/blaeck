<picture>
  <source media="(prefers-color-scheme: dark)" srcset="extras/blaeck-dark.svg">
  <source media="(prefers-color-scheme: light)" srcset="extras/blaeck-light.svg">
  <img src="extras/blaeck-light.svg" alt="blaeck" height="75">
</picture>

---

blaeck is an Arduino library. It sends the values a sketch holds over Serial or TCP,
using the [blaeck protocol](https://sebajost.github.io/blaeck-protocol/).

With blaeck, a sketch can:

- **Log signals:** values such as a temperature or a counter.
- **Show up in Home Assistant:** with controls, sensors and events.
- **Take commands:** text such as `<SwitchLED,1>`.

Loggbok reads the board, logs the signals and passes the entities to Home Assistant over MQTT.
Loggbok is internal and not publicly released. The protocol is documented, so any program can
read a board, and a serial monitor or TCP terminal can send commands.

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
  device.begin(Serial);

  device.withName(F("Weather Station"));

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
  device.begin(server).withDebugStream(&Serial);

  device.withName(F("Weather Station"));

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
  you started. Call `begin()` only once per instance, normally in `setup()`; even
  `end()` does not allow another call.
- `addSignal(...)` registers a variable. blaeck keeps a pointer to it and reads it
  whenever it sends data, so you only have to keep the variable up to date.
- `tick()` reads incoming commands and sends the values when they are due. Call it in every
  `loop()`.

The host decides how often data is sent. It sends `<BLAECK.DATA_START>` and
`<BLAECK.INTERVAL_START,1000>` to get one frame per second, and `<BLAECK.INTERVAL_STOP>` to stop
interval-driven data.
The data frames are binary, not readable text in a terminal.

TCP sketches need the networking library for their board; see [networking](docs/network.md) for supported server requirements
and the optional TelnetStream setup.

## Documentation

| Guide | What it covers |
|---|---|
| [Signals](docs/signals.md) | Registering values to log, and naming them |
| [Entities](docs/entities.md) | Controls, sensors and events, as Home Assistant shows them |
| [Commands](docs/commands.md) | Handling commands, and a function that sees every command |
| [Sub-devices](docs/sub-devices.md) | Showing a second board, or a part of this one, as its own device |
| [Sending data](docs/sending-data.md) | Intervals, sending it yourself, timestamps, buffered writes |
| [Configuration](docs/configuration.md) | Tables, memory and compile-time settings |
| [Networking](docs/network.md) | Servers, client limits, terminal output and transport errors |

## Examples

The examples are in `examples/`. In the Arduino IDE, open them with
**File > Examples > blaeck**.

The main examples include a [NetworkSetup.h tab](examples/Basic/NetworkSetup.h) with
ready-made Ethernet setup for Mega/GIGA shields and ESP32-PoE/WT32-ETH01 boards.
Leave `USE_TCP` at `0` for Serial or set it to `1` for TCP and configure the included tab.
Using `NetworkSetup.h` is optional. You can use your own networking libraries and setup instead.

Start with **Basic**, then **Signals** and **Commands**. Follow with **Controls**, **Sensors**
and **Events**, then **WaveformGenerator** to see the pieces working together.

| Example | What it teaches |
|---|---|
| [Basic](examples/Basic) | The smallest sketch that logs two values |
| [Signals](examples/Signals) | Numeric, boolean and text signals, and numbered arrays |
| [Commands](examples/Commands) | Plain commands, and a function that sees every command |
| [Controls](examples/Controls) | A number, text, switch, select and buttons |
| [Sensors](examples/Sensors) | A number, bool, function, index and text sensor |
| [Events](examples/Events) | Declaring and reporting occurrences |
| [WaveformGenerator](examples/WaveformGenerator) | A complete, controllable waveform dashboard |
| [WriteModes](examples/WriteModes) | Interval, on-change, and explicit signal writes |
| [more / ConfigurableSignals](examples/more/ConfigurableSignals) | Choose which signals to log through commands and save the selection in EEPROM |
| [more / SHT31TempHumiditySensor](examples/more/SHT31TempHumiditySensor) | Read a real temperature and humidity sensor; requires Adafruit SHT31 |
| [more / TimestampsRTC](examples/more/TimestampsRTC) | Wall-clock timestamps using the UNO R4's RTC |
| [more / TimestampsNTP](examples/more/TimestampsNTP) | Network-synchronized timestamps |
| [more / WiFi](examples/more/WiFi) | WiFi-specific connection setup |
| [more / ESP32C6BugBoard](examples/more/ESP32C6BugBoard) | An ESP32-C6 board-specific example |
| [more / BridgeESP32PoE](examples/more/BridgeESP32PoE) | A bridge between a UART and TCP |
| [more / SubDevices](examples/more/SubDevices) | A second board shown as its own device, polled over a UART |

## Coming from BlaeckSerial or BlaeckTCP

blaeck replaces BlaeckSerial and BlaeckTCP, which end at 6.0.1. It continues their version
numbering, but its API is new, so a sketch needs changes rather than a plain upgrade. The main
ones:

| BlaeckSerial / BlaeckTCP 6.0.1 | blaeck |
|---|---|
| `#include <BlaeckSerial.h>` and `BlaeckSerial BlaeckSerial;` | `#include <Blaeck.h>` and `Blaeck device;` |
| `BlaeckSerial.begin(&Serial, 2)` | `device.begin(Serial)`. Tables grow as entries are added |
| `BlaeckTCP.begin(clients, &Serial, 2, port)` | Start the server yourself, then `device.begin(server).withClients(clients)`. Add `.withDebugStream(&Serial)` for debug output |
| `update()`, `markSignalUpdated()`, `writeUpdatedData()`, `timedWrite…()` | `write()` sends a value at once; `writeAtInterval()` and `writeOnChange()` choose when each signal is sent. See [Sending data](docs/sending-data.md) |
| `setIntervalMs()` | Removed. The host sets the interval with `BLAECK.INTERVAL_START` |
| Writes go out at once | Nothing goes out until the host sends `BLAECK.DATA_START` (data) or `BLAECK.ENTITIES_START` (controls, sensors and events) |
| `deleteSignals()` | `clearAllSignals()` |
| `setCommandCallback()` | `onCommand()` or `onAnyCommand()` |
| Names as `String` | `const char *` or `F()`; add `.c_str()` to a `String` |
| `BlaeckSerialConfig.h`, `BlaeckTCPConfig.h` | One `BlaeckConfig.h`. If an old config file is still present, the build fails and asks you to move its settings there |

BlaeckSerial's I2C master/slave mode has no replacement. A sketch that uses `beginMaster()` or
`beginSlave()` should stay on BlaeckSerial 6.0.1.

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
