# blaeck

Unified library combining BlaeckSerial and BlaeckTCP.
Version 7.0.0 is in development; setting its metadata does not publish a release.

- Follow `extras/API-STYLE.md` for public documentation and implementation comments.
- One concrete `Blaeck` class in `src/Blaeck.h`, with no core base class or virtual transport hooks.
  `Blaeck.cpp` implements protocol/catalog logic and lifecycle; `BlaeckTransport.cpp` implements
  connection I/O using Arduino's generic interfaces, never a concrete network library.
  Keep one catalog per device, not two wrapped devices. The small typed server adapter
  in `src/detail/BlaeckServerAdapter.h` and CRC32 helper in `src/detail/BlaeckCRC32.h`
  are header-defined.
- No mandatory third-party library dependencies. Keep the internal CRC32 helper's
  fixed protocol parameters and upstream MIT notice intact.
- Write/look-up names and text signal/state values accept RAM strings and F() literals.
  Signal values retain a pointer plus a storage flag; pushed state text is not retained.
  Keep flash reads explicit, preserve RAM lifetime rules, and compare text contents rather
  than pointers. Text getters still return RAM strings. Native tests simulate separate
  flash storage; this supplements but does not replace AVR hardware validation.
- One `BlaeckBeginRef` handle configures table sizes, client count and debug output.
- TCP takes an already-started server via `begin(server)`. TelnetPrint is an optional
  user-supplied server, never an implicit dependency. Require `accept()`; do not substitute
  `available()`. Keep concrete client ownership in the adapter and session state in Blaeck.
- Streams use begin(stream) by reference. Select buffering defaults on attachment but
  preserve explicit overrides set before begin().
- begin() is allowed once per instance, including failed initialization. Later calls,
  even after end(), report an error and return an inert setup handle without changing
  the transport or catalogs. TCP reconnects and host takeovers do not call begin().
- Both connections report lowercase `blaeck` version 7.0.0. Keep the class/header
  `Blaeck`/`Blaeck.h` and uppercase BLAECK protocol framing and command names unchanged.
- TCP has one host at a time. A connection's first `BLAECK.*` command makes it the host; the
  previous host is closed and reported as "Client #N disconnected: replaced as host". Frames go
  only to the host, terminals get Terminal text, and terminal commands are not acknowledged.
- Reuse the allocated client slots. Teardown closes accepted clients, not the supplied
  server. Report allocation/setup errors through transportError()/printTransportError().
- Examples start with `Blaeck.h` to select this package while the old libraries coexist.
- `src/detail/BlaeckDefaults.h` loads the optional user `BlaeckConfig.h`.
  Shared layout settings must reach every translation unit. Buffering defaults are per transport.
- There is one topic example set, not Serial/TCP trees. Each main sketch shows
  device.begin(Serial) and device.begin(server) in explicit USE_TCP branches.
  Do not hide the choice or begin call in a connection setup wrapper.
- NetworkSetup.h exposes the board's concrete server alias as NetworkSetup::Server.
- Text files use LF. Preserve byte equality of setup tab copies.
- Edit `examples/Basic/NetworkSetup.h`, then run
  `python extras/scripts/syncnetwork.py`.
- Keep only the default MAC address in committed examples. Never commit WiFi credentials.
- No builds, flashing, commits, pushes or publishing without user authorization.
- Reporting uses per-signal writeAtInterval(BLAECK_ALWAYS/ON_CHANGE/OFF) and independent
  writeOnChange(delta, minIntervalMs), disabled with writeOnChange(BLAECK_OFF).
  BLAECK_ANY_CHANGE is a named zero threshold; numeric zero remains valid. Both share
  one last-sent baseline. tick() is read() plus writeIfDue(); do not restore update
  flags or timed-write API variants.
- Native reporting/transport tests and representative Mega Serial/GIGA TCP builds have
  passed. SignalReportingTest also passed direct/buffered Serial checks on a physical
  Optiboot Mega. This is not evidence of Loggbok integration or hardware TCP validation.
- Do not modify the existing BlaeckSerial or BlaeckTCP repositories for this library.
