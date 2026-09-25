"""Check devices from addDevice() on real hardware: python drive_device_tree.py SERIAL_PORT.

Upload DeviceTreeTest first and close Loggbok/serial monitors. Only a Mega and USB are
needed: the pump controller is simulated in the sketch. Checks the device list, which
device each signal, command and channel belongs to, commands routed to the pump with
'@' as Loggbok sends them, the data frames while the pump is
missing and after it returns, and the restart notice and event for a pump restart.
Failures exit nonzero.
"""
import argparse
import struct
import sys
import time
import zlib

START = b"<BLAECK:"
END = b"/BLAECK>\r\n"
VERSION = "7.0.0"

SINGLE, MASTER, SLAVE = 0x00, 0x01, 0x02
BOARD = bytes([MASTER, 0])
PUMP = bytes([SLAVE, 1])


class Link:
    """Splits the serial byte stream into frames (key, payload) and text lines."""

    def __init__(self, port):
        self.port = port
        self.buffer = bytearray()

    def _items(self):
        while self.buffer:
            if self.buffer.startswith(START):
                end = self.buffer.find(END)
                if end < 0:
                    return
                body = bytes(self.buffer[len(START):end])
                del self.buffer[:end + len(END)]
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


def device_record(owner, name, hw, fw):
    return owner + b"\0".join(s.encode() for s in (name, hw, fw, VERSION, "blaeck")) + b"\0"


def owner_before(payload, name, gap=0):
    """The two ownership bytes of the catalog record that holds `name`."""
    at = payload.find(name.encode() + b"\0")
    if at < 2 + gap:
        raise AssertionError(f"{name} not found in catalog")
    return payload[at - 2 - gap:at - gap]


def decode_data(body):
    """Signal indexes, float values and status of a D2 frame (all test signals are floats)."""
    crc, = struct.unpack("<I", body[-4:])
    if zlib.crc32(body[:-4]) != crc:
        raise AssertionError("Data CRC32 mismatch")
    mode = body[12]
    position = 13 + (8 if mode != 0 else 0)
    if body[position] != ord(":"):
        raise AssertionError("Invalid data header")
    position += 1
    values = {}
    while position < len(body) - 9:
        index, = struct.unpack_from("<H", body, position)
        value, = struct.unpack_from("<f", body, position + 2)
        values[index] = value
        position += 6
    if position != len(body) - 9:
        raise AssertionError("Misaligned signal payload")
    return values, body[-9], body[-8:-4]


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
        items = link.collect(frame_with(0xC0), timeout=8)
        restart = frames(items, 0xC0)[-1][2]
        check("board restart notice names the master",
              restart == device_record(BOARD, "DeviceTreeTest", "Arduino Mega 2560", "1.0"), restart)
    except TimeoutError:
        print("NOTE no restart notice seen (the board did not reset on open); continuing")
    time.sleep(0.5)
    port.reset_input_buffer()
    link.buffer.clear()

    items = link.send("<BLAECK.GET_DEVICES>", frame_with(0xB3))
    devices = frames(items, 0xB3)[-1][2]
    check("device list: board as master, then the pump as slave 1",
          devices == device_record(BOARD, "DeviceTreeTest", "Arduino Mega 2560", "1.0")
          + device_record(PUMP, "Pump controller", "Simulated", "1.0"), devices)

    symbols = frames(link.send("<BLAECK.WRITE_SYMBOLS>", frame_with(0xB0)), 0xB0)[-1][2]
    check("BoardValue belongs to the board", owner_before(symbols, "BoardValue") == BOARD)
    check("Flow and Pressure belong to the pump",
          owner_before(symbols, "Flow") == PUMP and owner_before(symbols, "Pressure") == PUMP)

    commands = frames(link.send("<BLAECK.WRITE_COMMANDS>", frame_with(0xA0)), 0xA0)[-1][2]
    check("SET_PUMP_SPEED belongs to the pump", owner_before(commands, "SET_PUMP_SPEED", 2) == PUMP)
    check("POLL stays on the board", owner_before(commands, "POLL", 2) == BOARD)

    states = frames(link.send("<BLAECK.WRITE_STATE_CHANNELS>", frame_with(0x90)), 0x90)[-1][2]
    check("PumpLink and the command's own PumpSpeed belong to the pump",
          owner_before(states, "PumpLink") == PUMP and owner_before(states, "PumpSpeed") == PUMP)

    events = frames(link.send("<BLAECK.WRITE_EVENT_CHANNELS>", frame_with(0x80)), 0x80)[-1][2]
    check("PumpAlarms belongs to the pump", owner_before(events, "PumpAlarms") == PUMP)

    link.send("<SET_PUMP_SPEED,40>", done("SET_PUMP_SPEED"))
    items = link.send("<POLL>", done("POLL"))
    values, status, payload = decode_data(frames(items, 0xD2)[-1][3])
    check("pump answering: all three signals, status 0",
          sorted(values) == [0, 1, 2] and status == 0 and payload == bytes(4), (values, status, payload))
    check("the forwarded speed reached the pump", abs(values.get(1, -1) - 4.0) < 1e-6, values)

    # How Loggbok sends a command of the pump: routed with '@' and the pump's slave ID.
    items = link.send("<@1:#5:SET_PUMP_SPEED,30>", ack_for(5))
    ack = items[-1][2]
    check("a routed pump command runs and is acknowledged",
          ("text", "DONE SET_PUMP_SPEED") in items and ack[8] == 0, ack[8:10])
    items = link.send("<@1:#6:POLL>", ack_for(6))
    check("a board command routed to the pump is refused",
          ("text", "DONE POLL") not in items and items[-1][2][8] == 1, items[-1][2][8:10])
    items = link.send("<POLL>", done("POLL"))
    values, _, _ = decode_data(frames(items, 0xD2)[-1][3])
    check("the routed speed reached the pump", abs(values.get(1, -1) - 3.0) < 1e-6, values)

    link.send("<SIM_SILENT,1>", done("SIM_SILENT"))
    items = link.send("<POLL>", done("POLL"))
    values, status, payload = decode_data(frames(items, 0xD2)[-1][3])
    check("pump missing: only BoardValue is sent", sorted(values) == [0], values)
    check("pump missing: status 0x01 names Flow and slave 1",
          status == 0x01 and payload == bytes([0, 1, 0, 1]), (status, payload))
    link_states = frames(items, 0x95)
    check("pump missing: PumpLink says so, for the pump",
          bool(link_states) and link_states[-1][2][:2] == PUMP and b"no answer" in link_states[-1][2])

    link.send("<SIM_SILENT,0>", done("SIM_SILENT"))
    items = link.send("<POLL>", done("POLL"))
    values, status, _ = decode_data(frames(items, 0xD2)[-1][3])
    check("pump back: all three signals, status 0", sorted(values) == [0, 1, 2] and status == 0,
          (values, status))
    link_states = frames(items, 0x95)
    check("pump back: PumpLink says ok", bool(link_states) and b"ok" in link_states[-1][2])

    link.send("<SIM_RESTART>", done("SIM_RESTART"))
    items = link.send("<POLL>", done("POLL"))
    restarts = frames(items, 0xC0)
    check("pump restart: a restart notice for the pump only",
          len(restarts) == 1 and restarts[0][2] == device_record(PUMP, "Pump controller", "Simulated", "1.0"),
          [r[2] for r in restarts])
    alarms = frames(items, 0x85)
    check("pump restart: the event comes from the pump", bool(alarms) and alarms[-1][2][:2] == PUMP)

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
