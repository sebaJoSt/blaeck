# Blaeck prototype

Local, unpublished prototype combining BlaeckSerial and BlaeckTCP. No remote is configured.
Version 0.0.0 is a prototype placeholder, not a release-number decision.

- One shared `src/BlaeckCore.h/.cpp`; do not import a second core.
- The only public device class/header is `Blaeck`/`Blaeck.h`. `Blaeck.cpp` implements both
  connection types using Arduino's generic interfaces, never a concrete network library.
  Keep one core/catalog per device, not two wrapped devices. Only the small typed server adapter
  in `src/detail/BlaeckServerAdapter.h` is header-defined.
- TCP takes an already-started server via `begin(server)`. TelnetPrint is an optional
  user-supplied server, never an implicit dependency. Require `accept()`; do not substitute
  `available()`. Keep concrete client ownership in the adapter and session state in Blaeck.
- Streams use begin(stream) by reference. Select buffering defaults on attachment but
  preserve explicit overrides, including calls through the base class.
- Wire identities remain BlaeckSerial/BlaeckTCP by connection type. Do not rename them yet.
- Reuse the allocated client slots. Teardown closes accepted clients, not the supplied
  server. Report allocation/setup errors through transportError()/printTransportError().
- Examples start with `Blaeck.h` to select this package while the old libraries coexist.
- Shared layout settings live in `BlaeckConfig.h`. Buffering defaults are per transport.
- There is one topic example set, not Serial/TCP trees. Each main sketch shows
  device.begin(Serial) and device.begin(server) in explicit USE_TCP branches.
  Do not hide the choice or begin call in a connection setup wrapper.
- NetworkSetup.h exposes the board's concrete server alias as NetworkSetup::Server.
- Sources use CRLF. Preserve byte equality of setup tab copies.
- Edit `examples/Basic/NetworkSetup.h`, then run
  `python extras/scripts/syncnetwork.py`.
- Keep only the default MAC address in committed examples. Never commit WiFi credentials.
- No builds, flashing, commits, pushes or publishing without user authorization.
- Current unified-public-class revision has source checks only; previous prototype
  compile/runtime results are not evidence for this revision.
- Do not modify the existing BlaeckSerial or BlaeckTCP repositories for this prototype.
