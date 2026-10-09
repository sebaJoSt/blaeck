# Golden scenarios

What blaeck sends, recorded, so that another implementation of the protocol can be checked
against it byte for byte. blaeckpy plays the same scenarios and must send the same frames.

- `scenarios/NAME.json` describes a device and a list of steps.
- `frames/NAME.txt` holds every frame blaeck sent while playing it: the step that caused the
  frame, a tab, the frame in hex from `<blaeck:` to the LF.

`python extras/scripts/golden.py` plays every scenario on blaeck, built for this computer with the
host-test stubs, and rewrites `frames/`. `--check` plays them and fails if a recorded file differs;
CI runs it, so a change to what blaeck sends shows up as a failing check until the files are
written again and committed.

## Scenario format

Scenarios use only what blaeckpy can declare: values are `bool`, `int` (a `long long`), `float`
(a `double`) and `str`.

```json
{
  "description": "What the scenario is for.",
  "board": {"name": "Greenhouse", "hw_version": "...", "fw_version": "1.0"},
  "timestamp_mode": "none | micros | unix",
  "unix_start_us": 1700000000000000,
  "signals": [{"name": "Temperature", "type": "float", "value": 21.5,
               "write_at_interval": "always | off | {\"on_change\": 0.5}",
               "write_on_change": "0.5 | any | off", "min_interval_ms": 100}],
  "properties": [{"kind": "number_input | sensor | switch | text_input", "name": "...", "...": "..."}],
  "events": [{"name": "Alarms", "types": ["Overheat"]}],
  "buttons": [{"name": "Reboot"}],
  "commands": ["MOTOR"],
  "devices": [{"name": "Pump controller", "signals": [], "properties": [], "events": [], "buttons": []}],
  "steps": [{"command": "<BLAECK.GET_DEVICES>"}]
}
```

Properties: a `number_input` or `sensor` has `type` (`int` or `float`) and `value`, or, for a
sensor, `signal` to show a signal's value. Number inputs take `min`, `max`, `step` and `mode`
(`auto`, `box`, `slider`); a `text_input` takes `max_length` and `mode` (`plain`, `password`). Every
property, event and button takes `display_name`, `icon`, `device_class`, `category` (`config`,
`diagnostic`) and `disabled_by_default`; numbers also `unit`, `state_class`, `display_precision`,
`force_update`, `write_on_change` and `min_interval_ms`.

Steps, each followed by a `tick()` unless it only writes:

| Step | Does |
|---|---|
| `{"command": "<...>"}` | the host sends this command |
| `{"set_signal": "Name", "value": v}` | the sketch sets a signal's variable |
| `{"set_property": "Name", "value": v}` | the sketch sets a property's variable |
| `{"advance_ms": n}` | the clock moves on n milliseconds |
| `{"tick": true}` | a tick with nothing else |
| `{"write_event": ["Alarms", "Overheat"], "device": "..."}` | writeEvent(), on a sub-device if named |
| `{"write_all": true}` | writeAll() |
| `{"mark_missing": "Device"}`, `{"mark_present": ...}`, `{"write_restarted": ...}` | on a sub-device |

The clock starts at 0 and moves only with `advance_ms`, so every run sends the same bytes.
