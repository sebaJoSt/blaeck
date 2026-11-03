"""Send every input and command the harness declares, with a value it must take and one it
must refuse.

An input's value is checked before it is stored, so a refusal is an absence: the board prints
nothing. Each case says which it expects, and the mismatch is the finding.
"""
import sys
import time

import serial

try:
    from blaeckpy.protocol import _decode
except ImportError:
    sys.exit("This driver reads frames with blaeckpy's decoder: "
             "pip install git+https://github.com/sebaJoSt/blaeckpy")

if len(sys.argv) != 2:
    sys.exit("Usage: python drive_commands.py SERIAL_PORT")
PORT = sys.argv[1]

# (command line to send, expect_accepted, why)
CASES = [
    ("<N_int,50>",            True,  "in range"),
    ("<N_int,500>",           False, "above max"),
    ("<N_int,-1>",            False, "below min"),
    ("<N_int,2.5>",           False, "a fraction for an int"),
    ("<N_int,abc>",           False, "not a number"),
    ("<N_int,>",              False, "empty value"),
    ("<N_float,2.5>",         True,  "in range, on the step"),
    ("<N_float,2.6>",         True,  "in range, off the step, kept as sent"),
    ("<N_float,99>",          False, "above max"),
    ("<N_level,200>",         True,  "in range"),
    ("<N_level,256>",         False, "above max"),
    ("<N_badrange,7>",        True,  "range was dropped at declaration"),
    ("<S_enabled,1>",         True,  "on"),
    ("<S_enabled,0>",         True,  "off"),
    ("<S_enabled,7>",         False, "a switch takes 0 or 1"),
    ("<S_enabled,ON>",        False, "text is not 0 or 1"),
    ("<S_flag,1>",            True,  "on"),
    ("<L_wave,Square>",       True,  "a declared option"),
    ("<L_wave,Zigzag>",       False, "not a declared option"),
    ("<L_wave,square>",       False, "options match exactly"),
    ("<L_case,AUTO>",         True,  "second of two options differing only in case"),
    ("<L_range,10V>",         True,  "a declared option"),
    ("<B_ping>",              True,  "a button takes no value"),
    ("<B_ping,1>",            True,  "a value a button did not ask for"),
    ("<B_reboot>",            True,  "diagnostic button"),
    ("<T_label,hello>",       True,  "inside maxLength"),
    ("<T_label," + "x" * 31 + ">", True,  "exactly maxLength"),
    ("<T_label," + "x" * 32 + ">", False, "one over maxLength"),
    ("<T_secret,hunter2>",    True,  "inside maxLength"),
    ("<R_uptime,5>",          False, "a sensor is read only"),
    ("<P_print,Hi,3>",        True,  "plain, two parameters"),
    ("<P_print,Hi>",          False, "plain, handler refuses one parameter"),
    ("<Nope,1>",              False, "no such command"),
]


def read_for(s, seconds):
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        buf += s.read(4096)
    return buf


def split(raw):
    """The frames in raw, decoded, and the text the board printed between them."""
    items = _decode.Decoder().feed(raw)
    return ([i for i in items if isinstance(i, _decode.Frame)],
            "".join(i for i in items if isinstance(i, str)))


def text_of(raw):
    """The board's printed lines; the frames share the port with them."""
    out = []
    for line in split(raw)[1].split("\n"):
        line = line.replace("\r", "")
        printable = "".join(c for c in line if 32 <= ord(c) < 127)
        if len(printable) >= 3 and len(printable) >= len(line) * 0.8:
            out.append(printable)
    return out


def entities_of(raw):
    """name -> (value kind, access bits, DTYPE) for every property of the first entity list in
    raw, and name -> ("event" or "button", flags, None) for the other kinds."""
    lists = [f.body for f in split(raw)[0] if f.key == 0x90]
    if not lists:
        sys.exit("No entity list received")
    out = {p.name: (p.value_kind, p.flags & 3, p.dtype) for p in lists[0].properties}
    out.update({e.name: ("event", e.flags, None) for e in lists[0].events})
    out.update({b.name: ("button", b.flags, None) for b in lists[0].buttons})
    return out


# (value kind, access, DTYPE); None where it depends on the board's int width.
EXPECTED_ENTITIES = {
    "N_int": (0, 3, None),
    "N_float": (0, 3, 8),
    "N_level": (0, 3, 1),
    "S_enabled": (1, 3, 0),
    "L_wave": (2, 3, 1),
    "T_label": (3, 3, 10),
    "R_uptime": (0, 1, 7),
    "Status": (3, 1, 10),
    # Buttons: flags. B_reboot has a device class (bit 2), diagnostic (2 << 3), disabled (bit 5).
    "B_ping": ("button", 0x00, None),
    "B_reboot": ("button", 0x34, None),
}


with serial.Serial(PORT, 115200, timeout=0.2) as s:
    # A Mega resets when DTR is raised and prints its startup checks; a Giga does not, so
    # the banner may already be long gone. Ask what the board is either way.
    s.setDTR(False)
    time.sleep(0.2)
    s.setDTR(True)
    boot = read_for(s, 4.0)
    lines = text_of(boot)
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

    print("\n---- commands ----")
    results = []
    for line, expect, why in CASES:
        s.reset_input_buffer()
        s.write(line.encode("ascii"))
        s.flush()
        raw = read_for(s, 0.9)
        lines = text_of(raw)
        got = any(l.startswith("CMD ") and "refused-by-handler" not in l for l in lines)
        ok = got == expect
        results.append((ok, line, expect, got, why, lines))
        mark = "ok  " if ok else "MISMATCH"
        print(f"  {mark} {line[:44]:46} expected={'accept' if expect else 'refuse':6} "
              f"got={'accept' if got else 'refuse':6}  ({why})")
        for l in lines:
            if l.startswith("CMD ") or "ignored" in l:
                print(f"        | {l}")

    print("\n---- entity list ----")
    s.reset_input_buffer()
    s.write(b"<BLAECK.WRITE_ENTITIES>")
    s.flush()
    entities = entities_of(read_for(s, 1.5))
    for name, (kind, access, dtype) in EXPECTED_ENTITIES.items():
        got = entities.get(name)
        ok = got == (kind, access, dtype) or (got is not None and dtype is None and got[:2] == (kind, access))
        results.append((ok, name, True, ok, "entity", []))
        print(f"  {'ok  ' if ok else 'MISMATCH'} {name:12} expected={(kind, access, dtype)} got={got}")

bad = [r for r in results if not r[0]]
print(f"\n{len(results) - len(bad)}/{len(results)} as expected, {len(bad)} mismatched")
sys.exit(1 if bad else 0)
