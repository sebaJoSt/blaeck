"""Copy Basic's NetworkSetup.h to every existing example copy.

Usage (from the repository root):
  python extras/scripts/syncnetwork.py
  python extras/scripts/syncnetwork.py --check

Edit examples/Basic/NetworkSetup.h first. Synchronization overwrites differences in
the other copies; --check reports differences without changing any files.
"""
import argparse
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="report differences without writing files")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    different = 0
    total = 0
    for name in ("NetworkSetup.h",):
        source = root / "examples" / "Basic" / name
        data = source.read_bytes()
        copies = sorted((root / "examples").rglob(name))
        total += len(copies)
        for path in copies:
            if path == source or path.read_bytes() == data:
                continue
            different += 1
            if args.check:
                print(f"{path.relative_to(root)} differs from {source.relative_to(root)}",
                      file=sys.stderr)
            else:
                path.write_bytes(data)
                print(f"Updated {path.relative_to(root)}")

    if args.check and different:
        print("Run extras/scripts/syncnetwork.py to synchronize the copies.",
              file=sys.stderr)
        return 1
    print(f"{total} setup tabs checked; {different} different.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
