# Hardware and integration harnesses

These sketches are manual Serial fixtures for protocol, metadata and host-integration
checks. CI compiles them on Mega and GIGA but does not upload or run them.
Use a test board and test host configuration: drivers send commands, change settings,
fire events and may reboot the board. Do not connect them to production equipment.

Upload the matching sketch and use 115200 baud. For direct Serial drivers, close
Loggbok and other serial monitors first; for integration drivers, keep Loggbok connected
with the required MQTT/Home Assistant or TimescaleDB outputs configured.

| Fixture | Driver / purpose |
|---|---|
| CommandTest | `drive_commands.py SERIAL_PORT`, `probe_ownstate.py SERIAL_PORT`: command validation and command-state frames |
| EventTest | `drive_events.py SERIAL_PORT`: event types, framing and timestamp widths |
| DatatypeTest | Manual host inspection of all supported data types |
| StateChannelTest | Manual host inspection of state channels |
| SignalMetadataTest | `drive_signal_metadata.py [seconds\|capture.json]`: MQTT discovery and optional HA registry checks |
| CommandMetadataTest | `drive_command_metadata.py [seconds\|capture.json]`: discovery/registry; `roundtrip_command_state.py [device_filter]`: HA command/state round trips |
| EventMetadataTest | `drive_event_metadata.py [seconds\|capture.json]`: discovery/registry and live HA event checks |
| SignalTimingTest | `drive_signal_timing.py [table]`: issue HA commands and check recorded TimescaleDB rows |
| SignalReportingTest | `drive_signal_reporting.py SERIAL_PORT`: Mega/AVR reporting policies, shared baselines, rate limits, numeric/text values and CRC32, with direct and buffered Serial writes |

Run each Python driver from its sketch directory, or pass its full path.
The scripts' module docstrings describe their individual expectations.

## Mega signal-reporting checks

`SignalReportingTest` needs only a Mega and USB; it does not drive pins or use external
services. Uploading replaces the board's current sketch. Close other serial clients first.
Substitute the actual Mega port for `COMxx`:

```powershell
arduino-cli compile --fqbn arduino:avr:mega extras\tests\harness\SignalReportingTest
arduino-cli upload --fqbn arduino:avr:mega --port COMxx extras\tests\harness\SignalReportingTest
python extras\tests\harness\SignalReportingTest\drive_signal_reporting.py COMxx
```

Use the matching board profile for both compile and upload. For a Mega with the custom
Optiboot profile installed here, replace `arduino:avr:mega` with `my_boards:avr:mega`;
`arduino-cli board list` identifies the connected board and port.

The driver identifies the harness and AVR type widths before changing anything. It checks
decoded data frames rather than trusting device-side PASS messages, and exits nonzero on
failure. Cadence/rate-limit checks also check their device-clock timing preconditions; a
slow or interrupted host run fails explicitly rather than producing misleading results.
The sketch services automatic reporting only when asked, making assignments and reporting
passes independently observable. This tests real AVR arithmetic, memory and Serial output,
not continuous `loop()` throughput. Allocation failures, TCP reconnects and clock rollover
remain covered by the native suite.

The driver's parser can be checked without a board or Python dependencies:

```powershell
python extras\tests\harness\SignalReportingTest\drive_signal_reporting.py --self-test
```

The harness passed 165 checks on a physical ATmega2560 with Optiboot, across direct and
buffered Serial output. This does not establish Loggbok integration or TCP behavior.

## Python dependencies and connections

Install only what the chosen driver needs: `pyserial` for direct Serial checks,
`paho-mqtt` for discovery captures, `websocket-client` for HA registry/login, and
`psycopg2-binary` for TimescaleDB checks.

Live discovery drivers require `MQTT_HOST`, using a TLS WebSocket endpoint on port 443
and path `/`. A saved `capture.json` supplies discovery messages without contacting MQTT.
Optional HA registry checks still run in capture mode when HA is configured.

For HA, set `HA_URL` and either `HA_TOKEN` or `HA_USERNAME` / `HA_PASSWORD`.
The shared `ha_registry.py DEVICE_FILTER [--json]` helper inspects the entity registry.
For timing checks also set `TSDB_HOST`, `TSDB_DB`, `TSDB_USER`, `TSDB_PASSWORD`, and
optionally `TSDB_PORT` (default 5432). Supply credentials only through the environment.

These drivers are not part of the native host suite in `extras/scripts/runhosttests.py`,
which exercises library behavior without a board or external services.
