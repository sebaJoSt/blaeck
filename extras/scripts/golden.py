#!/usr/bin/env python3
"""Plays the golden scenarios on blaeck and records every frame it sends.

  golden.py           regenerate extras/tests/golden/frames/ from the scenarios
  golden.py --check   regenerate in memory and fail if a recorded file differs

Each scenario (extras/tests/golden/scenarios/NAME.json) becomes a small C++ program that declares
the device and plays its steps on blaeck, built with the host-test stubs and a fixed clock. What
blaeck sends is the reference: blaeckpy plays the same scenarios and must send the same bytes.
Scenarios use only what blaeckpy can declare, so values are bool, int (long long), float (double)
and str. See extras/tests/golden/README.md for the format.
"""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GOLDEN = ROOT / "extras" / "tests" / "golden"
HOST = ROOT / "extras" / "tests" / "host"

C_TYPES = {"bool": "bool", "int": "long long", "float": "double"}
TEXT_SIZE = 256

STATE_CLASSES = {
    "measurement": "BLAECK_STATE_CLASS_MEASUREMENT",
    "total": "BLAECK_STATE_CLASS_TOTAL",
    "total_increasing": "BLAECK_STATE_CLASS_TOTAL_INCREASING",
    "measurement_angle": "BLAECK_STATE_CLASS_MEASUREMENT_ANGLE",
}
NUMBER_MODES = {"auto": "BLAECK_NUMBER_MODE_AUTO", "box": "BLAECK_NUMBER_MODE_BOX",
                "slider": "BLAECK_NUMBER_MODE_SLIDER"}
TEXT_MODES = {"plain": "BLAECK_TEXT_MODE_PLAIN", "password": "BLAECK_TEXT_MODE_PASSWORD"}
TIMESTAMP_MODES = {"none": "BLAECK_NO_TIMESTAMP", "micros": "BLAECK_MICROS", "unix": "BLAECK_UNIX"}

PRELUDE = r"""
#include <Blaeck.h>
#include <cstdio>
#include <cstring>
#include <string>

// A serial port: commands go in, frames come out.
class Port : public Stream
{
public:
  std::string in, out;
  size_t pos = 0;
  int available() override { return (int)(in.size() - pos); }
  int read() override { return pos < in.size() ? (unsigned char)in[pos++] : -1; }
  int peek() override { return pos < in.size() ? (unsigned char)in[pos] : -1; }
  size_t write(uint8_t b) override { out.push_back((char)b); return 1; }
};

static Blaeck device;
static Port port;

// One line per frame: the step that caused it, a tab, the frame in hex, markers to LF.
static void dump(const char *label)
{
  size_t start = 0;
  while (start < port.out.size())
  {
    size_t end = port.out.find("/>\n", start);
    if (end == std::string::npos)
    {
      printf("%s\tPARTIAL\n", label);
      break;
    }
    end += 3;
    printf("%s\t", label);
    for (size_t i = start; i < end; i++)
      printf("%02X", (unsigned char)port.out[i]);
    printf("\n");
    start = end;
  }
  port.out.clear();
}

static void tickAndDump(const char *label)
{
  device.tick();
  dump(label);
}

static void command(const char *label, const char *text)
{
  port.in = text;
  port.pos = 0;
  tickAndDump(label);
}

static void advance(unsigned long ms)
{
  hostMillis() += ms;
  hostMicros() += ms * 1000UL;
}

static unsigned long long unixStart = 0;
static unsigned long long unixClock() { return unixStart + hostMicros(); }

static void nothing() {}
static void ignoreCommand(const char *, const char *const *, byte) {}
"""


def c_string(text: str) -> str:
    """A C string literal for any text, its bytes as UTF-8. Octal escapes, three digits each,
    so a following digit is never read as part of the escape."""
    out = []
    for b in text.encode("utf-8"):
        if b in (0x22, 0x5C) or b < 0x20 or b >= 0x7F:
            out.append(f"\\{b:03o}")
        else:
            out.append(chr(b))
    return '"' + "".join(out) + '"'


def c_value(type_: str, value) -> str:
    if type_ == "bool":
        return "true" if value else "false"
    if type_ == "int":
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError(f"an int value is a whole number, not {value!r}")
        return f"{value}LL"
    if type_ == "float":
        return repr(float(value))
    raise ValueError(f"no literal for type {type_!r}")


class Program:
    """The C++ for one scenario: globals, setup and steps."""

    def __init__(self, name: str):
        self.name = name
        self.globals: list[str] = []
        self.setup: list[str] = []
        self.steps: list[str] = []
        self.signals: dict[str, str] = {}      # name -> C variable, board and sub-devices alike
        self.properties: dict[str, str] = {}
        self.devices: dict[str, str] = {}      # sub-device name -> handle variable
        self.var_count = 0

    def variable(self, type_: str, value, size: int = TEXT_SIZE) -> str:
        var = f"v{self.var_count}"
        self.var_count += 1
        if type_ == "str":
            self.globals.append(f"char {var}[{size}] = {c_string(value)};")
        else:
            self.globals.append(f"{C_TYPES[type_]} {var} = {c_value(type_, value)};")
        return var

    # ----- declarations -----

    def signal(self, owner: str, s: dict) -> None:
        if s["name"] in self.signals:
            raise ValueError(f"signal {s['name']!r} twice: steps address signals by name")
        type_ = s["type"]
        var = self.variable(type_, s.get("value", "" if type_ == "str" else 0))
        target = f"(const char *){var}" if type_ == "str" else f"&{var}"
        call = f"{owner}.addSignal({c_string(s['name'])}, {target})"
        if "write_at_interval" in s:
            mode = s["write_at_interval"]
            if mode == "always":
                call += ".writeAtInterval(BLAECK_ALWAYS)"
            elif mode == "off":
                call += ".writeAtInterval(BLAECK_OFF)"
            else:
                call += f".writeAtInterval(BLAECK_ON_CHANGE, {float(mode['on_change'])!r})"
        if "write_on_change" in s:
            delta = s["write_on_change"]
            if delta == "off":
                call += ".writeOnChange(BLAECK_OFF)"
            else:
                call += f".writeOnChange({0.0 if delta == 'any' else float(delta)!r}, {s.get('min_interval_ms', 100)}UL)"
        self.setup.append(call + ";")
        self.signals[s["name"]] = var

    def presentation(self, item: dict, number: bool) -> str:
        chain = ""
        if number and "unit" in item:
            chain += f".withUnit({c_string(item['unit'])})"
        for key, method in (("display_name", "withDisplayName"), ("icon", "withIcon"),
                            ("device_class", "withDeviceClass")):
            if key in item:
                chain += f".{method}({c_string(item[key])})"
        if number and "state_class" in item:
            chain += f".withStateClass({STATE_CLASSES[item['state_class']]})"
        if number and "display_precision" in item:
            chain += f".withDisplayPrecision({int(item['display_precision'])})"
        category = item.get("category", "none")
        if category == "config":
            chain += ".config()"
        elif category == "diagnostic":
            chain += ".diagnostic()"
        if item.get("disabled_by_default"):
            chain += ".disabledByDefault()"
        if item.get("force_update"):
            chain += ".forceUpdate()"
        return chain

    def property(self, owner: str, p: dict) -> None:
        kind, name = p["kind"], c_string(p["name"])
        if kind in ("number_input", "sensor") and "signal" in p:
            if kind != "sensor":
                raise ValueError("only a sensor shows a signal's value")
            var = self.signals[p["signal"]]
            call = f"{owner}.addSensor({name}, &{var})"
            number = True
        elif kind in ("number_input", "sensor"):
            type_ = p["type"]
            if type_ not in ("int", "float"):
                raise ValueError(f"a {kind} holds an int or a float, not {type_!r}")
            var = self.variable(type_, p.get("value", 0))
            method = "addNumberInput" if kind == "number_input" else "addSensor"
            call = f"{owner}.{method}({name}, &{var})"
            number = True
            if "min" in p:
                limits = [c_value(type_, p["min"]), c_value(type_, p["max"])]
                if "step" in p:
                    limits.append(c_value(type_, p["step"]))
                call += f".withRange({', '.join(limits)})"
            if "mode" in p:
                call += f".withMode({NUMBER_MODES[p['mode']]})"
        elif kind == "switch":
            var = self.variable("bool", p.get("value", False))
            call = f"{owner}.addSwitch({name}, &{var})"
            number = False
        elif kind == "text_input":
            size = p["max_length"] + 1
            var = self.variable("str", p.get("value", ""), size)
            call = f"{owner}.addTextInput({name}, {var}, {size})"
            if "mode" in p:
                call += f".withMode({TEXT_MODES[p['mode']]})"
            number = False
        else:
            raise ValueError(f"property kind {kind!r} is not supported yet")
        call += self.presentation(p, number)
        if "write_on_change" in p:
            call += f".writeOnChange({float(p['write_on_change'])!r}, {p.get('min_interval_ms', 100)}UL)"
        self.setup.append(call + ";")
        self.properties[p["name"]] = var

    def event(self, owner: str, e: dict) -> None:
        types = ",".join(e["types"])
        call = f"{owner}.addEvent({c_string(e['name'])}, {c_string(types)})"
        self.setup.append(call + self.presentation(e, False) + ";")

    def button(self, owner: str, b: dict) -> None:
        call = f"{owner}.addButton({c_string(b['name'])}, nothing)"
        self.setup.append(call + self.presentation(b, False) + ";")

    def entities(self, owner: str, holder: dict) -> None:
        for s in holder.get("signals", []):
            self.signal(owner, s)
        for p in holder.get("properties", []):
            self.property(owner, p)
        for e in holder.get("events", []):
            self.event(owner, e)
        for b in holder.get("buttons", []):
            self.button(owner, b)

    def declare(self, scenario: dict) -> None:
        board = scenario.get("board", {})
        if "name" in board:
            self.setup.append(f"device.withName({c_string(board['name'])});")
        if "hw_version" in board:
            self.setup.append(f"device.withHWVersion({c_string(board['hw_version'])});")
        if "fw_version" in board:
            self.setup.append(f"device.withFWVersion({c_string(board['fw_version'])});")
        mode = scenario.get("timestamp_mode", "none")
        if mode == "unix":
            self.setup.append(f"unixStart = {int(scenario['unix_start_us'])}ULL;")
            self.setup.append("device.setTimestampMode(BLAECK_UNIX, unixClock);")
        elif mode != "none":
            self.setup.append(f"device.setTimestampMode({TIMESTAMP_MODES[mode]});")
        self.entities("device", scenario)
        for d in scenario.get("devices", []):
            handle = f"d{len(self.devices)}"
            chain = ""
            if "hw_version" in d:
                chain += f".withHWVersion({c_string(d['hw_version'])})"
            if "fw_version" in d:
                chain += f".withFWVersion({c_string(d['fw_version'])})"
            self.setup.append(f"BlaeckDeviceRef {handle} = device.addDevice({c_string(d['name'])}){chain};")
            self.devices[d["name"]] = handle
            self.entities(handle, d)
        for c in scenario.get("commands", []):
            self.setup.append(f"device.onCommand({c_string(c)}, ignoreCommand);")

    # ----- steps -----

    def assign(self, var: str, type_value) -> str:
        if isinstance(type_value, str):
            return f"strcpy({var}, {c_string(type_value)});"
        if isinstance(type_value, bool):
            return f"{var} = {'true' if type_value else 'false'};"
        if isinstance(type_value, int):
            return f"{var} = {type_value}LL;"
        return f"{var} = {float(type_value)!r};"

    def step(self, index: int, s: dict) -> None:
        (kind, arg), *rest = s.items()
        extra = dict(rest)
        label = c_string(f"{index} {kind}" + (f" {arg}" if not isinstance(arg, (dict, list)) else ""))
        owner = self.devices[extra["device"]] if "device" in extra else "device"
        if kind == "command":
            self.steps.append(f"command({label}, {c_string(arg)});")
        elif kind == "set_signal":
            self.steps.append(self.assign(self.signals[arg], extra["value"]))
            self.steps.append(f"tickAndDump({label});")
        elif kind == "set_property":
            self.steps.append(self.assign(self.properties[arg], extra["value"]))
            self.steps.append(f"tickAndDump({label});")
        elif kind == "advance_ms":
            self.steps.append(f"advance({int(arg)}UL);")
            self.steps.append(f"tickAndDump({label});")
        elif kind == "tick":
            self.steps.append(f"tickAndDump({label});")
        elif kind == "write_event":
            event, type_ = arg
            self.steps.append(f"{owner}.writeEvent({c_string(event)}, {c_string(type_)});")
            self.steps.append(f"dump({label});")
        elif kind == "write_all":
            self.steps.append(f"{owner}.writeAll();")
            self.steps.append(f"dump({label});")
        elif kind in ("mark_missing", "mark_present", "write_restarted"):
            method = {"mark_missing": "markMissing", "mark_present": "markPresent",
                      "write_restarted": "writeRestarted"}[kind]
            self.steps.append(f"{self.devices[arg]}.{method}();")
            self.steps.append(f"dump({label});")
        else:
            raise ValueError(f"step {index}: unknown kind {kind!r}")

    def source(self, scenario: dict) -> str:
        self.declare(scenario)
        for i, s in enumerate(scenario["steps"]):
            self.step(i, s)
        body = "\n  ".join(["device.begin(port);", *self.setup, *self.steps, "return 0;"])
        return PRELUDE + "\n" + "\n".join(self.globals) + "\n\nint main()\n{\n  " + body + "\n}\n"


def play(path: Path, cxx: str, temp: Path) -> str:
    scenario = json.loads(path.read_text(encoding="utf-8"))
    source = temp / f"{path.stem}.cpp"
    exe = temp / f"{path.stem}.exe"
    source.write_text(Program(path.stem).source(scenario), encoding="utf-8")
    subprocess.run([cxx, "-std=c++11", "-DBLAECK_NATIVE_TEST", f"-I{HOST}", f"-I{ROOT / 'src'}",
                    str(source), str(ROOT / "src" / "Blaeck.cpp"),
                    str(ROOT / "src" / "BlaeckTransport.cpp"), "-o", str(exe)], check=True)
    output = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=20).stdout
    if "\tPARTIAL" in output:
        raise RuntimeError(f"{path.name}: blaeck left a frame unfinished")
    header = (f"# blaeck's frames for scenarios/{path.name}, written by extras/scripts/golden.py.\n"
              "# Do not edit: change the scenario or blaeck, and run the script again.\n"
              "# Each line: the step that caused the frame, a tab, the frame in hex, markers to LF.\n")
    return header + output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if a recorded file differs")
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()

    scenarios = sorted((GOLDEN / "scenarios").glob("*.json"))
    frames_dir = GOLDEN / "frames"
    stale = []
    with tempfile.TemporaryDirectory(prefix="blaeck-golden-") as temp:
        for path in scenarios:
            recorded = frames_dir / f"{path.stem}.txt"
            text = play(path, args.cxx, Path(temp))
            if args.check:
                if not recorded.exists() or recorded.read_text(encoding="utf-8") != text:
                    stale.append(recorded.name)
            else:
                frames_dir.mkdir(exist_ok=True)
                recorded.write_text(text, encoding="utf-8")
                print(f"{path.stem}: {text.count(chr(10)) - 3} frames")
    if args.check:
        known = {p.stem for p in scenarios}
        orphans = [p.name for p in frames_dir.glob("*.txt") if p.stem not in known]
        for name in stale:
            print(f"FAIL: {name} differs from what blaeck sends now; run extras/scripts/golden.py")
        for name in orphans:
            print(f"FAIL: {name} has no scenario")
        if stale or orphans:
            return 1
        print(f"PASS: {len(scenarios)} golden scenarios match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
