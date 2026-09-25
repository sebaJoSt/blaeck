"""Check packaging and example hygiene that neither the compiler nor the host suite covers."""
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def require(condition, message):
    if not condition:
        sys.exit("FAIL: " + message)


def check_manifests():
    manifest = json.loads((ROOT / "library.json").read_text())
    properties = dict(line.split("=", 1) for line in
                      (ROOT / "library.properties").read_text().splitlines() if "=" in line)
    require(manifest["name"] == properties["name"] == "blaeck",
            "package names must match the lowercase brand")

    version_header = (ROOT / "src" / "BlaeckVersion.h").read_text(encoding="utf-8")
    macros = dict(re.findall(
        r"^#define (BLAECK_VERSION(?:_MAJOR|_MINOR|_PATCH)?) (.+)$", version_header, re.M))
    version = macros.get("BLAECK_VERSION", "").strip('"')
    require(re.fullmatch(r"\d+\.\d+\.\d+", version), "BLAECK_VERSION must be MAJOR.MINOR.PATCH")
    require(manifest["version"] == properties["version"] == version,
            "library.json, library.properties and BlaeckVersion.h must agree on the version")
    require(tuple(macros.get("BLAECK_VERSION_" + part) for part in ("MAJOR", "MINOR", "PATCH"))
            == tuple(version.split(".")), "numeric version macros must match " + version)

    require(properties["url"] == "https://github.com/sebaJoSt/blaeck"
            and manifest["repository"]["url"] == "https://github.com/sebaJoSt/blaeck.git",
            "package links must point to the blaeck repository")
    require(not properties.get("depends") and not manifest.get("dependencies"),
            "package must have no mandatory third-party library dependencies")
    require(properties["includes"] == "Blaeck.h", "IDE entry point must be Blaeck.h")
    for path in manifest["export"]["include"]:
        require((ROOT / path).exists(), "missing export path: " + path)


def check_sources():
    # The core uses Arduino's generic interfaces only; CI installs Ethernet for some boards,
    # so a compile alone would not notice a concrete network or CRC library creeping in.
    for p in (ROOT / "src").rglob("*"):
        if p.suffix not in (".h", ".cpp"):
            continue
        text = p.read_text(encoding="utf-8")
        require(not re.search(r'#include\s*[<"](?:CRC\w*|Crc\w*)\.h', text),
                "external CRC dependency in " + str(p))
        require(not re.search(r'#include\s*[<"](?:TelnetPrint|WiFi|Ethernet|NetTypes)\.h', text),
                "concrete networking dependency in " + str(p))


def check_examples():
    for p in (ROOT / "examples").rglob("*"):
        if p.suffix not in (".h", ".ino", ".cpp"):
            continue
        macs = re.findall(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", p.read_text(encoding="utf-8"))
        require(all(mac.upper() == "DE:AD:BE:EF:FE:ED" for mac in macs),
                "non-default MAC in " + str(p))
    secrets = (ROOT / "examples" / "more" / "WiFi" / "arduino_secrets.h").read_text()
    require('#define SECRET_SSID ""' in secrets and '#define SECRET_PASS ""' in secrets,
            "WiFi credentials must remain empty")


def main():
    check_manifests()
    check_sources()
    check_examples()
    subprocess.run([sys.executable, str(ROOT / "extras" / "scripts" / "syncnetwork.py"),
                    "--check"], check=True)
    print("PASS: manifests, versions, dependencies, example MACs and credentials, setup tab copies")


if __name__ == "__main__":
    main()
