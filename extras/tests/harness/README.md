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

Run each Python driver from its sketch directory, or pass its full path.
The scripts' module docstrings describe their individual expectations.

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

These drivers are not part of the native host suite in `extras/scripts/testserver.py`,
which exercises library behavior without a board or external services.
