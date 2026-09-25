"""Compile and run the protocol/transport implementation with host-only socket doubles."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    host = ROOT / "extras" / "tests" / "host"
    with tempfile.TemporaryDirectory(prefix="blaeck-server-") as temp:
        exe = Path(temp) / "server-test.exe"
        # BLAECK_NATIVE_TEST drops the 4-byte long checks, which a 64-bit Linux/macOS host fails.
        command = [args.cxx, "-std=c++11", "-Wall", "-Wextra", "-O1", "-DBLAECK_NATIVE_TEST",
                   "-I" + str(host), "-I" + str(ROOT / "src"),
                   str(host / "ServerAdapterTest.cpp"), str(ROOT / "src" / "Blaeck.cpp"),
                   str(ROOT / "src" / "BlaeckTransport.cpp"),
                   "-o", str(exe)]
        subprocess.run(command, check=True)
        subprocess.run([str(exe)], check=True, timeout=20)
        for defines, name in (
            (["-DBLAECK_TEST_SEPARATE_FLASH=1", "-Werror"], "separate-flash"),
            (["-DBLAECK_TEST_SEPARATE_FLASH=1", "-Werror", "-DBLAECK_ENABLE_STATE_CHANNELS=0",
              "-DBLAECK_ENABLE_COMMAND_META=0", "-DBLAECK_ENABLE_EVENTS=0",
              "-DBLAECK_TEST_REPORTING_ONLY=1"], "flash-features-off"),
            (["-DBLAECK_TEST_SEPARATE_FLASH=1", "-Werror", "-DBLAECK_ENABLE_COMMAND_META=0",
              "-DBLAECK_TEST_REPORTING_ONLY=1"], "flash-command-meta-off"),
            (["-DBLAECK_TEST_SEPARATE_FLASH=1", "-Werror", "-DBLAECK_ENABLE_STATE_CHANNELS=0",
              "-DBLAECK_TEST_REPORTING_ONLY=1"], "flash-state-channels-off"),
        ):
            flash_command = command[:]
            flash_command[1:1] = defines
            flash_command[-1] = str(Path(temp) / f"{name}.exe")
            subprocess.run(flash_command, check=True)
            subprocess.run([flash_command[-1]], check=True, timeout=20)
        distinct = Path(temp) / "distinct-defaults-test.exe"
        defaults_command = command[:]
        defaults_command[1:1] = ["-DBLAECK_SERIAL_BUFFERED_WRITES_DEFAULT=false",
                                "-DBLAECK_TCP_BUFFERED_WRITES_DEFAULT=true"]
        defaults_command[-1] = str(distinct)
        subprocess.run(defaults_command, check=True)
        subprocess.run([str(distinct)], check=True, timeout=20)
        metadata_off = Path(temp) / "reporting-no-metadata-test.exe"
        metadata_command = command[:]
        metadata_command[1:1] = ["-DBLAECK_ENABLE_SIGNAL_META=0",
                                 "-DBLAECK_TEST_REPORTING_ONLY=1"]
        metadata_command[-1] = str(metadata_off)
        subprocess.run(metadata_command, check=True)
        subprocess.run([str(metadata_off)], check=True, timeout=20)
        for capacity in (48, 255, 256, 300, 512):
            buffer_exe = Path(temp) / f"command-buffer-{capacity}.exe"
            buffer_command = command[:]
            buffer_command[1:1] = [
                f"-DBLAECK_COMMAND_MAX_CHARS_DEFAULT={capacity}",
                "-DBLAECK_TEST_COMMAND_BUFFER_ONLY=1",
            ]
            buffer_command[-1] = str(buffer_exe)
            subprocess.run(buffer_command, check=True)
            subprocess.run([str(buffer_exe)], check=True, timeout=20)
        for capacity in (1, 65535, 0, -1, 65536, 4294967296):
            bounds = subprocess.run(
                [args.cxx, "-std=c++11", "-fsyntax-only", "-x", "c++",
                 f"-DBLAECK_COMMAND_MAX_CHARS_DEFAULT={capacity}",
                 "-I" + str(host), "-I" + str(ROOT / "src"), "-"],
                input="#include <Blaeck.h>\n", capture_output=True, text=True)
            if capacity in (1, 65535):
                if bounds.returncode != 0:
                    raise RuntimeError(f"Valid buffer size {capacity} rejected:\n"
                                       + bounds.stdout + bounds.stderr)
            elif (bounds.returncode == 0 or
                  "must be between 1 and 65535 bytes" not in bounds.stderr):
                raise RuntimeError(f"Invalid buffer size {capacity} not rejected as expected:\n"
                                   + bounds.stdout + bounds.stderr)
        print("PASS: command-buffer compile-time bounds")
        disabled = Path(temp) / "server-no-delay-test.exe"
        command[1:1] = ["-DBLAECK_TCP_NO_DELAY_DEFAULT=false"]
        command[-1] = str(disabled)
        subprocess.run(command, check=True)
        subprocess.run([str(disabled)], check=True, timeout=20)
        negative = subprocess.run(
            [args.cxx, "-std=c++11", "-fsyntax-only", "-I" + str(host),
             "-I" + str(ROOT / "src"),
             str(host / "UnsupportedServer.cpp")],
            capture_output=True, text=True)
        if negative.returncode == 0 or "accept" not in negative.stderr:
            raise RuntimeError("available()-only server was not rejected as expected:\n"
                               + negative.stdout + negative.stderr)
        print("PASS: available()-only server rejected at compile time")


if __name__ == "__main__":
    main()
