"""Check real Arduino reporting over Serial: python drive_signal_reporting.py SERIAL_PORT.

Upload SignalReportingTest first and close Loggbok/serial monitors. Opening the port
usually resets a Mega. No pins are driven. Runs direct and buffered output, checking
decoded values, selected signal indexes, CRC32, callbacks and device-clock timing.
Failures exit nonzero. --self-test checks the decoder without a board or pyserial.
Frames are read with blaeckpy's decoder (pip install git+https://github.com/sebaJoSt/blaeckpy).
"""
import argparse
import math
import time
import unittest

try:
    from blaeckpy.protocol import _decode
    from blaeckpy.protocol import frames as wire
except ImportError:
    raise SystemExit("This driver reads frames with blaeckpy's decoder: "
                     "pip install git+https://github.com/sebaJoSt/blaeckpy")

# The harness's signals by index, at the Mega's widths: what the sketch registers, so data frames
# are read without asking for the device list.
SIGNALS = [_decode.Signal(f"signal {i}", "", dtype) for i, dtype in enumerate(
    [wire.DType.FLOAT] * 4 + [wire.DType.BOOL, wire.DType.TEXT, wire.DType.LONG,
                                wire.DType.ULONG, wire.DType.FLOAT])]


def decode_data(frame):
    """A data frame's values, timestamp and flags. The decoder checks the markers, the CRC32 and
    every field; this adds what the harness expects of its frames."""
    decoded = _decode.decode(frame, SIGNALS)
    if decoded.key != wire.Key.DATA:
        raise ValueError("Not a data frame")
    data = decoded.body
    if data.timestamp_mode != 1:
        raise ValueError(f"Expected MICROS timestamp, got mode {data.timestamp_mode}")
    return {"values": data.values, "timestamp": data.timestamp, "flags": data.flags}


class Decoder:
    """The serial stream as the driver reads it: each data frame decoded, each text line whole."""

    def __init__(self):
        self.stream = _decode.Decoder()
        self.stream.device_list = _decode.DeviceList("", "", 0, [
            _decode.Device(0, 0, 0, 0, "", "", "", SIGNALS)])
        self.text = ""

    def feed(self, data):
        result = []
        for item in self.stream.feed(bytes(data)):
            if isinstance(item, str):
                self.text += item
                *lines, self.text = self.text.split("\n")
                result.extend(line.rstrip("\r") for line in lines)
            elif item.key == wire.Key.DATA:
                if item.body.timestamp_mode != 1:
                    raise ValueError(f"Expected MICROS timestamp, got mode {item.body.timestamp_mode}")
                result.append({"values": item.body.values, "timestamp": item.body.timestamp,
                               "flags": item.body.flags})
        if len(self.stream._buffer) + len(self.text) > 16384:
            raise ValueError("Unterminated frame or unexpected serial output")
        return result


class Driver:
    def __init__(self, port):
        self.port = port
        self.decoder = Decoder()
        self.sequence = 0
        self.callbacks = 0
        self.millis = 0
        self.checks = 0
        self.hello = None

    def request(self, action, *args, builtin=None):
        self.sequence += 1
        command = f"<TEST,{self.sequence},{action}"
        command += "".join(f",{arg}" for arg in args) + ">"
        if builtin:
            command = f"<BLAECK.{builtin}>" + command
        self.port.write(command.encode("ascii"))
        self.port.flush()
        deadline = time.monotonic() + 8
        frames = []
        while time.monotonic() < deadline:
            for item in self.decoder.feed(self.port.read(max(1, self.port.in_waiting))):
                if isinstance(item, dict):
                    frames.append(item)
                elif item.startswith("BLAECK_REPORTING_TEST "):
                    self.hello = item
                elif item.startswith("TEST_DONE "):
                    _, sequence, ok, callbacks, millis, rejected = item.split()
                    if int(sequence) != self.sequence or ok != "1" or rejected != "0":
                        raise AssertionError(f"Failed/mismatched harness response: {item}")
                    self.callbacks, self.millis = int(callbacks), int(millis)
                    return frames
                elif item.strip():
                    raise AssertionError(f"Unexpected harness output: {item}")
        raise TimeoutError(f"No completion for {command}; correct sketch/port and no other serial client?")

    def expect(self, label, expected, action="POLL", *args, builtin=None, interval=False, requested=False):
        frames = self.request(action, *args, builtin=builtin)
        if expected is None:
            assert not frames, f"{label}: unexpected frames {frames}"
        else:
            assert len(frames) == 1, f"{label}: expected one frame, got {frames}"
            expected_flags = (0x04 if interval else 0) | (0x02 if requested else 0)
            assert frames[0]["flags"] & 0x06 == expected_flags, f"{label}: wrong reporting flags {frames[0]['flags']:#x}"
            actual = frames[0]["values"]
            assert actual.keys() == expected.keys(), f"{label}: indexes {actual.keys()} != {expected.keys()}"
            for index, value in expected.items():
                got = actual[index]
                equal = math.isnan(got) if isinstance(value, float) and math.isnan(value) else got == value
                assert equal, f"{label}: signal {index}: {got!r} != {value!r}"
        self.checks += 1
        print(f"PASS {label}")
        return frames

    def reset(self, buffered):
        # A board starts with data stopped; DATA_START also clears the baselines RESET resets.
        self.expect("data start", None, "SYNC", builtin="DATA_START")
        self.expect("interval stop", None, "SYNC", builtin="INTERVAL_STOP")
        self.expect("reset", None, "RESET", int(buffered))

    def set(self, index, value):
        self.expect(f"assignment {index} does not send", None, "SET", index, value)

    def wait(self, milliseconds):
        self.expect("waiting alone does not service reporting", None, "WAIT", milliseconds)


def run(driver, buffered):
    print(f"\n--- {'buffered' if buffered else 'direct'} writes ---")
    driver.reset(buffered)
    driver.expect("initial values without INTERVAL_START", {2: 0, 3: 0, 4: False, 5: "", 6: 0, 7: 0, 8: 0})
    driver.expect("equal values suppressed", None)
    driver.expect("interval start does not poll", None, "SYNC", builtin="INTERVAL_START,0")
    driver.expect("initial interval includes previously sent combined value", {0: 0, 1: 0, 3: 0}, interval=True)
    driver.set(1, 0.25)
    driver.expect("below interval threshold", {0: 0}, interval=True)
    driver.set(1, 0.5)
    driver.expect("inclusive accumulated threshold", {0: 0, 1: 0.5}, interval=True)
    driver.set(1, 2)
    driver.set(1, 0.5)
    driver.expect("away and back is not queued", {0: 0}, interval=True)
    driver.wait(1100)
    driver.set(2, 1)
    driver.set(3, 1)
    driver.expect("both paths merge without duplicates", {0: 0, 2: 1, 3: 1}, interval=True)
    driver.expect("INTERVAL_STOP stops only intervals", None, "SYNC", builtin="INTERVAL_STOP")
    driver.wait(1100)
    driver.set(2, 2)
    driver.expect("immediate remains active", {2: 2})
    sent_at = driver.millis
    driver.set(2, 3)
    driver.expect("rate limited", None)
    assert (driver.millis - sent_at) % (1 << 32) < 1000, "Host too slow for rate-limit precondition"
    driver.wait(1100)
    driver.expect("rate limit expires", {2: 3})
    driver.expect("explicit write bypasses threshold/rate limit", {2: 10}, "WRITE", 2, 10)
    driver.expect("explicit value becomes baseline", None)
    driver.set(2, 11)
    driver.expect("explicit write resets rate-limit clock", None)
    driver.wait(1100)
    driver.expect("changed since explicit write", {2: 11})

    driver.expect("data stop", None, "SYNC", builtin="DATA_STOP")
    driver.set(4, 1)
    driver.expect("stopped change not sent", None)
    driver.expect("data start", None, "SYNC", builtin="DATA_START")
    driver.expect("data start resends every on-change value",
                  {2: 11, 3: 1, 4: True, 5: "", 6: 0, 7: 0, 8: 0})
    driver.set(4, 0)
    driver.expect("boolean ignores numeric threshold", {4: False})
    driver.expect("boolean equal suppressed", None)
    driver.expect("grow text", None, "TEXT", 270, "a")
    driver.expect("exact 255-byte text snapshot", {5: "a" * 255})
    driver.expect("edit beyond wire limit", None, "TEXT", 270, "b")
    driver.expect("untransmitted text does not count", None)
    driver.expect("edit transmitted text", None, "TEXT", 255, "b")
    driver.expect("same-length text comparison", {5: "a" * 254 + "b"})
    driver.expect("empty text", None, "TEXT", 0, "a")
    driver.expect("text shrinks to empty", {5: ""})
    for value in (-2147483648, 2147483647, 2147483646):
        driver.set(6, value)
        driver.expect("signed 32-bit endpoint/exact one-unit change", {6: value})
    for value in (4294967295, 4294967294, 0):
        driver.set(7, value)
        driver.expect("unsigned 32-bit endpoint/exact one-unit change", {7: value})
    for token, expected in (("nan", math.nan), ("inf", math.inf), ("-inf", -math.inf), ("0", 0)):
        driver.set(8, token)
        driver.expect("floating-point transition", {8: expected})
        driver.expect("stable floating-point representation", None)

    driver.expect("enable callback", None, "CALLBACK", 1)
    driver.expect("start long interval", None, "SYNC", builtin="INTERVAL_START,1000")
    driver.expect("restarting the interval refreshes all interval values", {0: 0, 1: 0.5, 3: 1}, interval=True)
    anchor = driver.millis
    assert driver.callbacks == 1
    driver.expect("no callback between intervals", None)
    assert driver.callbacks == 1
    driver.wait(600)
    driver.expect("explicit write between intervals", {3: 10}, "WRITE", 3, 10)
    assert driver.callbacks == 1
    driver.set(3, 10.5)
    driver.wait(500)
    driver.expect("explicit write does not move interval cadence", {0: 0, 3: 10.5}, interval=True)
    assert 1000 <= (driver.millis - anchor) % (1 << 32) < 1600, "Host too slow for cadence precondition"
    assert driver.callbacks == 2
    all_values = {0: 0, 1: 0.5, 2: 11, 3: 10.5, 4: False, 5: "", 6: 2147483646, 7: 0, 8: 0}
    frames = driver.expect("requested snapshot includes OFF signals", all_values, "SYNC", builtin="WRITE_DATA", requested=True)
    assert frames[0]["flags"] & 2 and driver.callbacks == 3
    driver.expect("full snapshot refreshed all baselines", None)
    driver.expect("disable interval", None, "SYNC", builtin="INTERVAL_STOP")
    driver.expect("immediate-only check does not call callback", None)
    assert driver.callbacks == 3


def sample_frame(values=((0, wire.DType.FLOAT, 1.5),)):
    return wire.data(values, schema_hash=0, timestamp_mode=wire.TimestampMode.MICROS,
                       timestamp=123456)


class DecoderTests(unittest.TestCase):
    def test_fragmented_uart(self):
        decoder, items = Decoder(), []
        raw = sample_frame() + b"TEST_DONE 1 1 0 123 0\r\n"
        for value in raw:
            items.extend(decoder.feed(bytes([value])))
        self.assertEqual(items[0]["values"], {0: 1.5})
        self.assertEqual(items[0]["timestamp"], 123456)
        self.assertEqual(items[1], "TEST_DONE 1 1 0 123 0")

    def test_bad_crc(self):
        frame = bytearray(sample_frame())
        frame[24] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC"):
            decode_data(bytes(frame))

    def test_duplicate_indexes(self):
        with self.assertRaisesRegex(ValueError, "twice"):
            decode_data(sample_frame(((0, wire.DType.FLOAT, 1.0),) * 2))

    def test_text_and_integer_widths(self):
        values = ((5, wire.DType.TEXT, "abc"), (6, wire.DType.LONG, -2147483648),
                  (7, wire.DType.ULONG, 4294967295))
        self.assertEqual(decode_data(sample_frame(values))["values"],
                         {5: "abc", 6: -2147483648, 7: 4294967295})

    def test_other_frame_and_incomplete_data(self):
        decoder = Decoder()
        self.assertEqual(decoder.feed(wire.command_ack(1, 2, 0)), [])
        self.assertEqual(decoder.feed(sample_frame()[:-1]), [])
        self.assertEqual(len(decoder.feed(b"\n")), 1)

    def test_a_timestamp_other_than_micros_is_refused(self):
        with self.assertRaisesRegex(ValueError, "MICROS"):
            decode_data(wire.data(((0, wire.DType.FLOAT, 1.0),), schema_hash=0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(DecoderTests))
        return 0 if result.wasSuccessful() else 1
    if not args.port:
        parser.error("SERIAL_PORT is required unless --self-test is used")
    import serial
    with serial.Serial(args.port, 115200, timeout=0.1) as port:
        time.sleep(3)
        driver = Driver(port)
        driver.expect("identify harness", None, "HELLO")
        if driver.hello != "BLAECK_REPORTING_TEST 1 2 4 4":
            raise AssertionError(f"Expected Mega/AVR widths, received {driver.hello!r}")
        for buffered in (False, True):
            run(driver, buffered)
        print(f"\nPASS: {driver.checks} checks on {args.port}; direct and buffered writes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
