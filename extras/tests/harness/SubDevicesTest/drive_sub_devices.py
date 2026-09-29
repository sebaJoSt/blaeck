"""Check sub-devices from addDevice() on real hardware: python drive_sub_devices.py SERIAL_PORT.

Upload SubDevicesTest first and close Loggbok/serial monitors. Only a Mega and USB are
needed: the pump controller is simulated in the sketch. Checks the device list (B7), which
device each signal, input, sensor and command belongs to, the pump's commands sent by name, the
notices (C1), data frames and refused commands while the pump is missing and after it returns,
and the restart notice and event for a pump restart.
Failures exit nonzero.
"""
import argparse
import struct
import sys
import time
import zlib

START = b"<blaeck:"
END = b"/>\n"
VERSION = "7.0.0"

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


# The DeviceID each catalog entry carries: 0 for the board, 1 for the pump.
BOARD = bytes([0])
PUMP = bytes([1])
# The PropertyIndex a 0x95 of PumpLink carries: SET_PUMP_SPEED, SIM_SILENT, PumpLink.
PUMP_LINK = bytes([2, 0])


class Link:
    """Splits the serial byte stream into frames (key, payload) and text lines."""

    def __init__(self, port):
        self.port = port
        self.buffer = bytearray()

    def _items(self):
        while self.buffer:
            if self.buffer.startswith(START):
                # A raw LF only ever ends a frame, right after "/>".
                end = self.buffer.find(b"\n")
                if end < 0:
                    return
                if self.buffer[end - 2:end] != b"/>":
                    raise ValueError(f"Frame does not end in /> and LF: {bytes(self.buffer[:end + 1])!r}")
                body = unescape(bytes(self.buffer[len(START):end - 2]))
                del self.buffer[:end + 1]
                if len(body) < 7 or body[1] != ord(":") or body[6] != ord(":"):
                    raise ValueError(f"Malformed frame header: {body[:8]!r}")
                yield ("frame", body[0], body[7:], body)
            elif START.startswith(bytes(self.buffer)):
                return
            else:
                newline = self.buffer.find(b"\n")
                if newline < 0:
                    return
                line = bytes(self.buffer[:newline]).rstrip(b"\r").decode("ascii", "replace")
                del self.buffer[:newline + 1]
                yield ("text", line)

    def collect(self, until, timeout=5.0):
        """Reads until `until(item)` is true; returns the items read, that one included."""
        items = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.buffer.extend(self.port.read(max(1, self.port.in_waiting)))
            for item in self._items():
                items.append(item)
                if until(item):
                    return items
        raise TimeoutError(f"No answer within {timeout} s; got {[i[:2] for i in items]}")

    def send(self, text, until, timeout=5.0):
        self.port.write(text.encode("ascii"))
        self.port.flush()
        return self.collect(until, timeout)


def frame_with(key):
    return lambda item: item[0] == "frame" and item[1] == key


def ack_for(message_id):
    """The acknowledgement of the command sent with this message id; earlier acks are skipped."""
    return lambda item: (item[0] == "frame" and item[1] == 0xA5
                         and struct.unpack_from("<I", item[3], 2)[0] == message_id)


def done(command):
    return lambda item: item == ("text", f"DONE {command}")


def frames(items, key):
    return [item for item in items if item[0] == "frame" and item[1] == key]


FLOAT = 8
# The longest command a Mega receives: its 128-byte buffer less the terminator.
PAYLOAD_MAX = 127


def device_record(device_id, state, name, hw, fw, signals=()):
    """One B7 record: ID, parent 0, flags 0, state, the three names, then the signal count and
    each signal's name and type code."""
    record = bytes([device_id, 0, 0, 0, state]) + b"\0".join(s.encode() for s in (name, hw, fw)) + b"\0"
    record += len(signals).to_bytes(2, "little")
    for signal, dtype in signals:
        record += signal.encode() + b"\0" + bytes([dtype])
    return record


def device_list(*records):
    return (b"blaeck\0" + VERSION.encode() + b"\0" + PAYLOAD_MAX.to_bytes(2, "little") +
            bytes([len(records)]) + b"".join(records))


def notice(device_id, event):
    """A C1 payload: 1 restarted, 2 not responding, 3 responding again."""
    return bytes([device_id, event])


def owner_before(payload, name, gap=0):
    """The DeviceID byte of the catalog record that holds `name`."""
    at = payload.find(name.encode() + b"\0")
    if at < 1 + gap:
        raise AssertionError(f"{name} not found in catalog")
    return payload[at - 1 - gap:at - gap]


def decode_data(body):
    """Signal indexes and float values of a D3 frame (all test signals are floats)."""
    crc, = struct.unpack("<I", body[-4:])
    if zlib.crc32(body[:-4]) != crc:
        raise AssertionError("Data CRC32 mismatch")
    mode = body[10]
    position = 11 + (8 if mode != 0 else 0)
    values = {}
    while position < len(body) - 4:
        index, = struct.unpack_from("<H", body, position)
        value, = struct.unpack_from("<f", body, position + 2)
        values[index] = value
        position += 6
    if position != len(body) - 4:
        raise AssertionError("Misaligned signal payload")
    return values


class Checks:
    def __init__(self):
        self.failed = 0

    def check(self, name, condition, detail=""):
        print(("PASS " if condition else "FAIL ") + name + ("" if condition else f": {detail}"))
        if not condition:
            self.failed += 1


def run(port):
    link = Link(port)
    checks = Checks()
    check = checks.check

    # Opening the port resets a Mega; the board then announces itself.
    try:
        items = link.collect(frame_with(0xC1), timeout=8)
        restart = frames(items, 0xC1)[-1][2]
        check("board restart notice for device 0", restart == notice(0, 1), restart)
    except TimeoutError:
        print("NOTE no restart notice seen (the board did not reset on open); continuing")
    # The sketch polls its pump every second on its own; the checks below decide when it is asked.
    link.send("<SIM_AUTO,0>", done("SIM_AUTO"))
    time.sleep(0.5)
    port.reset_input_buffer()
    link.buffer.clear()

    items = link.send("<BLAECK.GET_DEVICES>", frame_with(0xB7))
    devices = frames(items, 0xB7)[-1][2]
    check("device list: the board with BoardValue, then the pump as device 1 with Flow and Pressure",
          devices == device_list(device_record(0, 0, "SubDevicesTest", "Arduino Mega 2560", "1.0",
                                               [("BoardValue", FLOAT)]),
                                 device_record(1, 0, "Pump controller", "Simulated", "1.0",
                                               [("Flow", FLOAT), ("Pressure", FLOAT)])), devices)

    # In the entity list, the entry kind sits between the DeviceID and the name.
    entities = frames(link.send("<BLAECK.WRITE_ENTITIES>", frame_with(0x90)), 0x90)[-1][2]
    check("the SET_PUMP_SPEED input and the PumpLink sensor belong to the pump",
          owner_before(entities, "SET_PUMP_SPEED", 1) == PUMP and owner_before(entities, "PumpLink", 1) == PUMP)
    check("the SIM_SILENT switch stays on the board", owner_before(entities, "SIM_SILENT", 1) == BOARD)
    check("PumpAlarms belongs to the pump", owner_before(entities, "PumpAlarms", 1) == PUMP)
    check("the SIM_RESTART button stays on the board", owner_before(entities, "SIM_RESTART", 1) == BOARD)
    check("the plain POLL command is not listed", b"POLL\0" not in entities)

    link.send("<SET_PUMP_SPEED,40>", done("SET_PUMP_SPEED"))
    items = link.send("<POLL>", done("POLL"))
    values = decode_data(frames(items, 0xD3)[-1][3])
    check("pump answering: all three signals", sorted(values) == [0, 1, 2], values)
    check("the forwarded speed reached the pump", abs(values.get(1, -1) - 4.0) < 1e-6, values)

    # How Loggbok sets an input of the pump: by name, like the board's. The callback runs
    # after the ack.
    items = link.send("<#5:SET_PUMP_SPEED,30>", done("SET_PUMP_SPEED"))
    acks = [f for f in frames(items, 0xA5) if struct.unpack_from("<I", f[3], 2)[0] == 5]
    check("a pump input is set and acknowledged",
          bool(acks) and acks[-1][2][8] == 0, acks[-1][2][8:10] if acks else items)
    items = link.send("<@1:POLL>", ack_for(0))
    check("'@' is no prefix: the command is unknown",
          ("text", "DONE POLL") not in items and items[-1][2][8:10] == bytes([1, 1]), items[-1][2][8:10])
    items = link.send("<POLL>", done("POLL"))
    values = decode_data(frames(items, 0xD3)[-1][3])
    check("the speed reached the pump", abs(values.get(1, -1) - 3.0) < 1e-6, values)

    link.send("<SIM_SILENT,1>", done("SIM_SILENT"))
    items = link.send("<POLL>", done("POLL"))
    values = decode_data(frames(items, 0xD3)[-1][3])
    check("pump missing: a notice for device 1", notice(1, 2) in [f[2] for f in frames(items, 0xC1)])
    check("pump missing: only BoardValue is sent", sorted(values) == [0], values)
    link_states = frames(items, 0x95)
    check("pump missing: PumpLink says so, for the pump",
          bool(link_states) and link_states[-1][2][:2] == PUMP_LINK and b"no answer" in link_states[-1][2])
    items = link.send("<#7:SET_PUMP_SPEED,20>", ack_for(7))
    check("pump missing: its command is refused, reason 8",
          ("text", "DONE SET_PUMP_SPEED") not in items and items[-1][2][8:10] == bytes([1, 8]),
          items[-1][2][8:10])
    devices = frames(link.send("<BLAECK.GET_DEVICES>", frame_with(0xB7)), 0xB7)[-1][2]
    check("pump missing: the device list says so",
          device_record(1, 1, "Pump controller", "Simulated", "1.0",
                        [("Flow", FLOAT), ("Pressure", FLOAT)]) in devices, devices)

    link.send("<SIM_SILENT,0>", done("SIM_SILENT"))
    items = link.send("<POLL>", done("POLL"))
    values = decode_data(frames(items, 0xD3)[-1][3])
    check("pump back: a notice for device 1", notice(1, 3) in [f[2] for f in frames(items, 0xC1)])
    check("pump back: all three signals", sorted(values) == [0, 1, 2], values)
    link_states = frames(items, 0x95)
    check("pump back: PumpLink says ok",
          bool(link_states) and link_states[-1][2][:2] == PUMP_LINK and b"ok" in link_states[-1][2])

    link.send("<SIM_RESTART>", done("SIM_RESTART"))
    items = link.send("<POLL>", done("POLL"))
    restarts = frames(items, 0xC1)
    check("pump restart: a restart notice for the pump only",
          [r[2] for r in restarts] == [notice(1, 1)], [r[2] for r in restarts])
    alarms = frames(items, 0x85)
    # The pump's PumpAlarms is the only event, and restarted its only type.
    check("pump restart: PumpAlarms reports restarted", bool(alarms) and alarms[-1][2] == bytes(4))

    print(f"{'FAILED' if checks.failed else 'PASSED'}: {checks.failed} failure(s)")
    return 1 if checks.failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="serial port of the Mega, e.g. COM5 or /dev/ttyACM0")
    args = parser.parse_args()
    import serial  # pyserial; imported here so --help works without it
    with serial.Serial(args.port, 115200, timeout=0.05) as port:
        return run(port)


if __name__ == "__main__":
    sys.exit(main())
