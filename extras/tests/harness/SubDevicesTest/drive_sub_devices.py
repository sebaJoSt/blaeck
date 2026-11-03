"""Check sub-devices from addDevice() on real hardware: python drive_sub_devices.py SERIAL_PORT.

Upload SubDevicesTest first and close Loggbok/serial monitors. Only a Mega and USB are
needed: the pump controller is simulated in the sketch. Checks the device list (B7), which
device each signal, input, sensor and command belongs to, the pump's commands sent by name, the
notices (C1), data frames and refused commands while the pump is missing and after it returns,
and the restart notice and event for a pump restart. Frames are read with blaeckpy's decoder
(pip install git+https://github.com/sebaJoSt/blaeckpy).
Failures exit nonzero.
"""
import argparse
import collections
import sys
import time

VERSION = "1.0.0"

try:
    from blaeckpy.protocol import _decode
except ImportError:
    sys.exit("This driver reads frames with blaeckpy's decoder: "
             "pip install git+https://github.com/sebaJoSt/blaeckpy")


# The DeviceID each catalog entry carries: 0 for the board, 1 for the pump.
BOARD = 0
PUMP = 1
# The PropertyIndex a 0x95 of PumpLink carries: SET_PUMP_SPEED, SIM_SILENT, PumpLink.
PUMP_LINK = bytes([2, 0])


class Link:
    """The serial byte stream as frames ("frame", key, fields, decoded frame) and text lines
    ("text", line). blaeckpy's decoder checks each frame's markers, CRC32 and fields."""

    def __init__(self, port):
        self.port = port
        self.reset()

    def reset(self):
        """Forget what was read so far, frames, text and the device list alike."""
        self.buffer = bytearray()
        self.decoder = _decode.Decoder()
        self.text = ""
        self.pending = collections.deque()

    def _items(self):
        if self.buffer:
            data = bytes(self.buffer)
            self.buffer.clear()
            for item in self.decoder.feed(data):
                if isinstance(item, str):
                    self.text += item
                    *lines, self.text = self.text.split("\n")
                    self.pending.extend(("text", line.rstrip("\r")) for line in lines)
                else:
                    self.pending.append(("frame", item.key, item.fields, item))
        while self.pending:
            yield self.pending.popleft()

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
    return lambda item: item[0] == "frame" and item[1] == 0xA5 and item[3].message_id == message_id


def done(command):
    return lambda item: item == ("text", f"DONE {command}")


def frames(items, key):
    return [item for item in items if item[0] == "frame" and item[1] == key]


FLOAT = 8
# The longest command a Mega receives: its 128-byte buffer less the terminator.
PAYLOAD_MAX = 127


def device_record(device_id, state, name, hw, fw, signals=()):
    """One B7 record: ID, parent 0, flags 0, state, the three names, then the signal count and
    each signal's name, unit (none here) and type code."""
    record = bytes([device_id, 0, 0, 0, state]) + b"\0".join(s.encode() for s in (name, hw, fw)) + b"\0"
    record += len(signals).to_bytes(2, "little")
    for signal, dtype in signals:
        record += signal.encode() + b"\0\0" + bytes([dtype])
    return record


def device_list(*records):
    return (b"blaeck\0" + VERSION.encode() + b"\0" + PAYLOAD_MAX.to_bytes(2, "little") +
            bytes([len(records)]) + b"".join(records))


def notice(device_id, event):
    """A C1 payload: 1 restarted, 2 not responding, 3 responding again."""
    return bytes([device_id, event])


def owners(entity_list):
    """name -> DeviceID of every property, event and button in a decoded entity list."""
    items = entity_list.properties + entity_list.events + entity_list.buttons
    return {item.name: item.device_id for item in items}


def decode_data(frame):
    """Signal index -> value of a data frame, read against the device list the driver asked for."""
    if not isinstance(frame.body, _decode.Data):
        raise AssertionError("A data frame before the device list")
    return frame.body.values


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
    # A board starts with data and entities stopped.
    link.send("<BLAECK.DATA_START>", ack_for(0))
    link.send("<BLAECK.ENTITIES_START>", ack_for(0))
    time.sleep(0.5)
    port.reset_input_buffer()
    link.reset()

    items = link.send("<BLAECK.GET_DEVICES>", frame_with(0xB7))
    devices = frames(items, 0xB7)[-1][2]
    check("device list: the board with BoardValue, then the pump as device 1 with Flow and Pressure",
          devices == device_list(device_record(0, 0, "SubDevicesTest", "Arduino Mega 2560", "1.0",
                                               [("BoardValue", FLOAT)]),
                                 device_record(1, 0, "Pump controller", "Simulated", "1.0",
                                               [("Flow", FLOAT), ("Pressure", FLOAT)])), devices)

    entity_list = frames(link.send("<BLAECK.WRITE_ENTITIES>", frame_with(0x90)), 0x90)[-1][3].body
    owner = owners(entity_list)
    check("the SET_PUMP_SPEED input and the PumpLink sensor belong to the pump",
          owner.get("SET_PUMP_SPEED") == PUMP and owner.get("PumpLink") == PUMP, owner)
    check("the SIM_SILENT switch stays on the board", owner.get("SIM_SILENT") == BOARD, owner)
    check("PumpAlarms belongs to the pump", owner.get("PumpAlarms") == PUMP, owner)
    check("the SIM_RESTART button stays on the board", owner.get("SIM_RESTART") == BOARD, owner)
    check("the plain POLL command is not listed", "POLL" not in owner, owner)

    link.send("<SET_PUMP_SPEED,40>", done("SET_PUMP_SPEED"))
    items = link.send("<POLL>", done("POLL"))
    values = decode_data(frames(items, 0xD3)[-1][3])
    check("pump answering: all three signals", sorted(values) == [0, 1, 2], values)
    check("the forwarded speed reached the pump", abs(values.get(1, -1) - 4.0) < 1e-6, values)

    # How Loggbok sets an input of the pump: by name, like the board's. The callback runs
    # after the ack.
    items = link.send("<#5:SET_PUMP_SPEED,30>", done("SET_PUMP_SPEED"))
    acks = [f for f in frames(items, 0xA5) if f[3].message_id == 5]
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
