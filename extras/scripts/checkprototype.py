"""Check unified prototype structure and packaging without compiling firmware."""
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def require(condition, message):
    if not condition:
        sys.exit("FAIL: " + message)


def main():
    src = ROOT / "src"
    require(sorted(p.name for p in src.rglob("*.cpp"))
            == ["Blaeck.cpp", "BlaeckCore.cpp"], "unexpected compiled source")
    core = (src / "BlaeckCore.h").read_text(encoding="utf-8")
    impl = (src / "Blaeck.cpp").read_text(encoding="utf-8")
    public = (src / "Blaeck.h").read_text(encoding="utf-8")
    require("class Blaeck : public BlaeckCore" in public, "missing unified public class")
    require("begin(Stream &stream)" in public and "server.accept()" in public,
            "missing connection overloads")
    require('return _tcpSelected ? "BlaeckTCP" : "BlaeckSerial";' in public,
            "wire identities must remain unchanged")
    require("namespace blaeck_serial" not in core and "namespace blaeck_tcp" not in core,
            "obsolete public namespace aliases")
    require(not (src / "BlaeckSerial.h").exists() and not (src / "BlaeckTCP.h").exists(),
            "obsolete public transport headers")
    for p in src.rglob("*"):
        if p.suffix in (".h", ".cpp"):
            require(not re.search(r'#include\s*[<"](?:TelnetPrint|WiFi|Ethernet|NetTypes)\.h',
                                  p.read_text(encoding="utf-8")),
                    "concrete networking dependency in " + str(p))
    require(not (src / "detail" / "BlaeckTCPImpl.h").exists(), "old inline TCP implementation")
    adapter = (src / "detail" / "BlaeckServerAdapter.h").read_text(encoding="utf-8")
    require("_server.accept()" in adapter and "_server.available()" not in adapter,
            "server adapter must use accept(), never guess with available()")
    require("_setBufferedWritesDefault(BLAECK_TCP_BUFFERED_WRITES_DEFAULT)" in public
            and "_setBufferedWritesDefault(BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT)" in impl,
            "transport defaults must be selected on attach")

    manifest = json.loads((ROOT / "library.json").read_text())
    properties = dict(line.split("=", 1) for line in
                      (ROOT / "library.properties").read_text().splitlines() if "=" in line)
    require(manifest["version"] == properties["version"] == "0.0.0",
            "prototype manifests must agree")
    require(properties["depends"] == "CRC"
            and [d["name"] for d in manifest["dependencies"]] == ["CRC"],
            "mandatory dependencies must stay network-free")
    require(properties["includes"] == "Blaeck.h", "IDE entry point must be network-free")
    for path in manifest["export"]["include"]:
        require((ROOT / path).exists(), "missing export path: " + path)

    examples = ROOT / "examples"
    topics = ("Basic", "Signals", "Commands", "StateChannels", "EventChannels",
              "WriteModes", "WaveformGenerator")
    require(not (examples / "Serial").exists() and not (examples / "TCP").exists(),
            "duplicate transport example trees")
    sketches = [p for p in examples.rglob("*.ino") if p.stem == p.parent.name]
    require(len(sketches) == 14, "expected seven topics and seven specialized examples")
    for topic in topics:
        folder = examples / topic
        text = (folder / (topic + ".ino")).read_text(encoding="utf-8")
        require("Blaeck device;" in text and "device.begin(Serial)" in text
                and "device.begin(server)" in text
                and "NetworkSetup::Server server(23);" in text
                and "networkLoop();" in text, "missing explicit connection choices in " + topic)
        require(not (folder / "ConnectionSetup.h").exists()
                and (folder / "NetworkSetup.h").exists(), "unexpected setup tabs in " + topic)
        branches = re.search(r"#if USE_TCP\s+networkBegin\(23\);\s+server.begin\(\);\s+"
                             r"device.begin\(server\)(.*?);"
                             r"\s+#else\s+device.begin\(Serial\)(.*?);\s+#endif",
                             text, re.S)
        require(branches is not None, "unexpected begin structure in " + topic)
        tcp_sizes = re.findall(r"\.with(?!DebugStream)(\w+)\(([^)]*)\)", branches[1])
        serial_sizes = re.findall(r"\.with(?!DebugStream)(\w+)\(([^)]*)\)", branches[2])
        require(tcp_sizes == serial_sizes, "connection branches size different tables in " + topic)
    for p in sketches:
        text = p.read_text(encoding="utf-8")
        if "Blaeck device;" in text:
            require("#include <Blaeck.h>" in text, "missing public entry point: " + str(p))
            require(not re.search(r'#include [<"]Blaeck(?:Serial|TCP)\.h', text),
                    "old public header in " + str(p))

    for p in (ROOT / "examples").rglob("*"):
        if p.suffix not in (".h", ".ino", ".cpp"):
            continue
        text = p.read_text(encoding="utf-8")
        macs = re.findall(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", text)
        require(all(mac.upper() == "DE:AD:BE:EF:FE:ED" for mac in macs),
                "non-default MAC in " + str(p))
    secrets = (ROOT / "examples" / "more" / "WiFi" / "arduino_secrets.h").read_text()
    require('#define SECRET_SSID ""' in secrets and '#define SECRET_PASS ""' in secrets,
            "WiFi credentials must remain empty")

    subprocess.run([sys.executable, str(ROOT / "extras" / "scripts" / "syncnetwork.py"),
                    "--check"], check=True)
    print("PASS: prototype structure, optional networking, defaults, manifests and examples")


if __name__ == "__main__":
    main()
