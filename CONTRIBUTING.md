# Contributing to blaeck

Thank you for taking the time to contribute. Submit changes through
[GitHub pull requests](https://github.com/sebaJoSt/blaeck/pulls).

Please:

1. Document every public name you add and give it a sketch-facing example. Follow
   [extras/API-STYLE.md](extras/API-STYLE.md).
2. Use `Blaeck device;` in examples and doc comments. The brand is lowercase `blaeck`;
   the class and header remain `Blaeck` and `Blaeck.h`.
3. Explain public API or wire-format changes in the pull request. Both Serial and TCP
   use the same implementation; consider both transports and compatibility with hosts.
4. Use LF line endings for text files, as specified by `.gitattributes`. Change
   NetworkSetup.h in Basic first, then synchronize its copies:

   ```powershell
   python extras\scripts\syncnetwork.py
   ```

Keep changes focused, leave WiFi credentials empty, and use only the example MAC address.
Do not copy changes back into the legacy BlaeckSerial or BlaeckTCP repositories as part
of a change to this library.

## Checks

The source-only packaging and example check is:

```powershell
python extras\scripts\checkpackage.py
```

This is not a build or runtime test. State which checks you actually ran in your pull
request; do not imply that unrun checks passed.
The host suite is run with `python extras\scripts\runhosttests.py` and requires a C++ compiler.
It exercises protocol/transport behavior and per-signal reporting with deterministic clocks,
including shared baselines, numeric/text comparisons, rate limits, allocation failures,
short writes and reconnects. Reporting is also run with signal metadata disabled.

For API documentation coverage, install `libclang` and run:

```powershell
python extras\scripts\checkdocs.py src\Blaeck.h -- -Iextras\tests\host
python extras\scripts\checkdocs.py src\Blaeck.h --extract -- -Iextras\tests\host
```

The second command generates the ignored `DocCodeBlocks.ino`; it does not compile it.
The parser needs C++ standard-library headers compatible with libclang. On Windows
with a newer Visual Studio STL, append
`-std=c++17 -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` after the include argument
if the STL rejects libclang's version. This affects documentation parsing only;
Arduino builds still use the board core's compiler settings.

GitHub Actions defines Arduino Serial/TCP example and fixture builds, PlatformIO
builds and package checks, native host tests, API documentation coverage and
doc-example compilation, plus source/NetworkSetup-copy checks. These workflows do not
publish packages. Hardware and host-integration drivers remain manual; see
[the harness instructions](extras/tests/harness/README.md).

## Editor setup

The included `.clangd` tells clangd to read a compilation database from `.development`.
When ready to generate it, run this from the repository root with the Arduino CLI and
the selected board core installed:

```powershell
arduino-cli compile --fqbn arduino:avr:mega --only-compilation-database --build-path .development examples\WaveformGenerator
```

This prepares editor configuration; it does not validate a firmware build. Regenerate
the database after changing boards, build flags or dependencies. It describes this
sketch and board, not every supported configuration.

The Microsoft C/C++ extension does not read `.clangd`. Set its `compileCommands` option
to the generated `compile_commands.json` in `.development` using local VS Code settings.
Both `.development` and `.vscode` are ignored by Git because they contain machine-specific
configuration.

Editors reading raw `.ino` files may still report missing function declarations that
the Arduino build generates automatically, especially in sketches with multiple tabs.
