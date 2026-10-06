"""Fire each event the harness can fire, and see which produced a frame.

An event or type that was never declared is dropped in silence, so the
board's own output cannot tell the two apart - only the absence of an 0x85 on the wire
can. Each case says which it expects.
"""
import re
import sys
import time

import serial

if len(sys.argv) != 2:
    sys.exit("Usage: python drive_events.py SERIAL_PORT")
PORT = sys.argv[1]

# (command, expect a frame, why)
CASES = [
    ("<E_ok>",         True,  "declared event, declared type"),
    ("<E_appended>",   True,  "type appended at runtime by addEventType"),
    ("<E_badtype>",    False, "type was never declared"),
    ("<E_badchannel>", False, "event was never declared"),
    ("<E_disabled>",   True,  "disabledByDefault is how a host files it, not whether it is sent"),
    ("<E_case>",       False, "type differs only in case"),
]


def read_for(s, seconds):
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        buf += s.read(4096)
    return buf


def unescape(raw):
    """A frame's bytes between its markers as sent before escaping: a backslash and the byte XOR 0x20."""
    out = bytearray()
    escaped = False
    for b in raw:
        if escaped:
            out.append(b ^ 0x20)
            escaped = False
        elif b == 0x5C:
            escaped = True
        else:
            out.append(b)
    return bytes(out)


def frames_of(raw):
    """(key, message id, payload) of each frame in raw, unescaped."""
    frames = []
    for body in re.findall(rb"<blaeck:([^\n]*)/>\n", raw):
        body = unescape(body)
        frames.append((body[0:1], body[2:6], body[7:]))
    return frames


def text_of(raw):
    t = re.sub(rb"<blaeck:[^\n]*\n", b"", raw)
    out = []
    for line in t.split(b"\n"):
        line = line.replace(b"\r", b"")
        p = bytes(c for c in line if 32 <= c < 127)
        if len(p) >= 3 and len(p) >= len(line) * 0.8:
            out.append(p.decode("ascii", "replace"))
    return out


with serial.Serial(PORT, 115200, timeout=0.2) as s:
    # A Mega resets when DTR is raised and prints its startup checks; a Giga does not, so
    # the banner may already be long gone. Ask what the board is either way.
    s.setDTR(False); time.sleep(0.2); s.setDTR(True)
    lines = text_of(read_for(s, 4.0))
    print("---- startup ----")
    if any(l.startswith(("PASS", "FAIL")) for l in lines):
        for line in lines:
            print(" ", line)
    else:
        print("  (no reset on this board, so no startup checks)")
    # A board starts with data and entities stopped.
    for start in (b"<BLAECK.DATA_START>", b"<BLAECK.ENTITIES_START>"):
        s.write(start); s.flush()
        read_for(s, 0.3)
    s.reset_input_buffer()
    s.write(b"<WIDTHS>"); s.flush()
    for line in text_of(read_for(s, 1.0)):
        if line.startswith("board "):
            print(" ", line)

    print("\n---- events ----")
    bad = 0
    for cmd, expect, why in CASES:
        s.reset_input_buffer()
        s.write(cmd.encode()); s.flush()
        raw = read_for(s, 0.9)
        frames = frames_of(raw)
        events = [p for k, _m, p in frames if k[0] == 0x85]
        ran = any(l.startswith("FIRED") for l in text_of(raw))
        got = len(events) > 0
        ok = got == expect
        bad += not ok
        print(f"  {'ok  ' if ok else 'MISMATCH'} {cmd:16} expected={'frame' if expect else 'none ':5} "
              f"got={'frame' if got else 'none ':5}  handler={'ran' if ran else 'DID NOT RUN'}  ({why})")
        for e in events:
            print(f"        0x85 payload: {e.hex(' ')}")

    print(f"\n{len(CASES) - bad}/{len(CASES)} as expected")
