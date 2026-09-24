# Blaeck prototype

Unified prototype combining BlaeckSerial and BlaeckTCP; not a released library.
Version 0.0.0 is a prototype placeholder, not a release-number decision.

- One concrete `Blaeck` class in `src/Blaeck.h`, with no core base class or virtual transport hooks.
  `Blaeck.cpp` implements protocol/catalog logic and lifecycle; `BlaeckTransport.cpp` implements
  connection I/O using Arduino's generic interfaces, never a concrete network library.
  Keep one catalog per device, not two wrapped devices. Only the small typed server adapter
  in `src/detail/BlaeckServerAdapter.h` is header-defined.
- One `BlaeckBeginRef` handle configures table sizes, client count and debug output.
- TCP takes an already-started server via `begin(server)`. TelnetPrint is an optional
  user-supplied server, never an implicit dependency. Require `accept()`; do not substitute
  `available()`. Keep concrete client ownership in the adapter and session state in Blaeck.
- Streams use begin(stream) by reference. Select buffering defaults on attachment but
  preserve explicit overrides across later begin() calls.
- Wire identities remain BlaeckSerial/BlaeckTCP by connection type. Do not rename them yet.
- Reuse the allocated client slots. Teardown closes accepted clients, not the supplied
  server. Report allocation/setup errors through transportError()/printTransportError().
- Examples start with `Blaeck.h` to select this package while the old libraries coexist.
- `src/detail/BlaeckDefaults.h` loads the optional user `BlaeckConfig.h`.
  Shared layout settings must reach every translation unit. Buffering defaults are per transport.
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
