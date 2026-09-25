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
