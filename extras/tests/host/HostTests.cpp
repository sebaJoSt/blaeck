#include <Blaeck.h>
#include <cassert>
#include <deque>
#include <iostream>
#include <type_traits>
#include <utility>
#include <vector>
#include <limits>

#ifndef BLAECK_TEST_REPORTING_ONLY
#define BLAECK_TEST_REPORTING_ONLY 0
#endif

#ifndef BLAECK_TEST_COMMAND_BUFFER_ONLY
#define BLAECK_TEST_COMMAND_BUFFER_ONLY 0
#endif

static_assert(!std::is_polymorphic<Blaeck>::value, "Blaeck needs no virtual transport hooks");
static_assert(!std::is_copy_constructible<Blaeck>::value, "Blaeck owns its allocations");
static_assert(!std::is_copy_assignable<Blaeck>::value, "Blaeck owns its allocations");
static_assert(std::is_same<
    decltype(std::declval<BlaeckBeginRef &>()
                 .withSignals(1).withStateChannels(1).withEventChannels(1)
                 .withEventTypes(1).withCommands(1).withDebugStream(nullptr).withClients(1)),
    BlaeckBeginRef &>::value, "Every begin option must preserve the same handle");

static int failAfter = -1;
static size_t allocations = 0;

static_assert(BLAECK_ANY_CHANGE == 0, "Any-change is a zero numeric threshold");
static_assert(std::is_same<decltype(BLAECK_ANY_CHANGE), const double>::value,
              "Any-change must not select the mode overload");
static_assert(std::is_same<
    decltype(std::declval<BlaeckNumericSignalRef &>()
                 .writeOnChange(0).writeOnChange(0, 0)
                 .writeOnChange(BLAECK_ANY_CHANGE, 250).writeOnChange(BLAECK_OFF)),
    BlaeckNumericSignalRef &>::value, "Reporting overloads preserve the handle type");

template <class Handle>
static auto acceptsModeWithInterval(int) -> decltype(
    std::declval<Handle &>().writeOnChange(BLAECK_OFF, 100), std::true_type{});
template <class>
static std::false_type acceptsModeWithInterval(...);

static_assert(!decltype(acceptsModeWithInterval<BlaeckNumericSignalRef>(0))::value,
              "An enum mode plus rate limit must not become a numeric threshold");
static_assert(!decltype(acceptsModeWithInterval<BlaeckBoolSignalRef>(0))::value,
              "Boolean signals must also reject modes with rate limits");
static_assert(!decltype(acceptsModeWithInterval<BlaeckTextSignalRef>(0))::value,
              "Text signals must also reject modes with rate limits");

void *operator new(size_t size, const std::nothrow_t &) noexcept
{
  ++allocations;
  if (failAfter == 0)
    return nullptr;
  if (failAfter > 0)
    --failAfter;
  try { return ::operator new(size); }
  catch (const std::bad_alloc &) { return nullptr; }
}

void *operator new[](size_t size, const std::nothrow_t &) noexcept
{
  return ::operator new(size, std::nothrow);
}

struct SocketState
{
  bool open = true;
  bool noDelay = false;
  uint16_t stopTimeout = 0;
  unsigned int stops = 0;
  std::string input;
  std::string output;
  size_t writeLimit = SIZE_MAX;
};

class FakeClient : public Client
{
public:
  SocketState *state = nullptr;
  FakeClient() {}
  explicit FakeClient(SocketState *s) : state(s) {}
  operator bool() override { return state != nullptr; }
  uint8_t connected() override { return state != nullptr && state->open; }
  void stop() override
  {
    if (state != nullptr)
    {
      state->open = false;
      ++state->stops;
      state = nullptr;
    }
  }
  int available() override { return state ? static_cast<int>(state->input.size()) : 0; }
  int peek() override { return available() ? static_cast<byte>(state->input[0]) : -1; }
  int read() override
  {
    int value = peek();
    if (value >= 0)
      state->input.erase(0, 1);
    return value;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *data, size_t size) override
  {
    assert(connected());
    const size_t accepted = size < state->writeLimit ? size : state->writeLimit;
    state->output.append(reinterpret_cast<const char *>(data), accepted);
    return accepted;
  }
  void setNoDelay(bool enabled) { if (state) state->noDelay = enabled; }
  void setConnectionTimeout(uint16_t ms) { if (state) state->stopTimeout = ms; }
  const char *remoteIP() { return "192.0.2.1"; }
  uint16_t remotePort() { return 1234; }
};

template<class Socket = FakeClient>
class FakeServer
{
public:
  std::deque<SocketState *> pending;
  bool noDelay = false;
  size_t accepts = 0;
  Socket accept()
  {
    ++accepts;
    if (pending.empty())
      return Socket();
    Socket result(pending.front());
    pending.pop_front();
    return result;
  }
  void setNoDelay(bool enabled) { noDelay = enabled; }
};

class PlainClient : public FakeClient
{
public:
  using FakeClient::FakeClient;
  void setNoDelay(bool) = delete;
  void setConnectionTimeout(uint16_t) = delete;
};

class PlainServer : public FakeServer<PlainClient>
{
public:
  void setNoDelay(bool) = delete;
};

class NoAddressClient : public FakeClient
{
public:
  using FakeClient::FakeClient;
  void remoteIP() = delete;
};

class NoPortClient : public FakeClient
{
public:
  using FakeClient::FakeClient;
  void remotePort() = delete;
};

class NoPeerClient : public NoAddressClient
{
public:
  using NoAddressClient::NoAddressClient;
  void remotePort() = delete;
};

class UnprintablePeerClient : public FakeClient
{
public:
  using FakeClient::FakeClient;
  struct Address {};
  Address remoteIP() { return {}; }
};

class Capture : public Print
{
public:
  std::string text;
  size_t write(uint8_t b) override { text += static_cast<char>(b); return 1; }
};

class FakeStream : public Stream
{
public:
  SocketState data;
  FakeClient channel{&data};
  unsigned int flushes = 0;
  int available() override { return channel.available(); }
  int read() override { return channel.read(); }
  int peek() override { return channel.peek(); }
  size_t write(uint8_t b) override { return channel.write(b); }
  size_t write(const uint8_t *data, size_t size) override { return channel.write(data, size); }
  void flush() override { ++flushes; }
};

class PacketProbe : public Blaeck
{
public:
  void sendPacket(size_t size)
  {
    _bufAllocate();
    _framePos = 0;
    assert(_bufEnsure(size));
    memset(_frameBuf, 'x', size);
    _framePos = size;
    _sendBuffered();
  }
};

class ReportingProbe : public Blaeck
{
public:
  const SignalReporting *reporting(int index) const { return Signals[index].Reporting; }
};

class ConfigurationProbe : public Blaeck
{
public:
#if BLAECK_ENABLE_SIGNAL_META
  const SignalMeta &signalMeta(int index) const { return *Signals[index].Meta; }
#endif
  const blaeck::blaeck_detail::CommandHandlerEntry &commandMeta(int index) const { return _commandHandlers[index]; }
#if BLAECK_ENABLE_STATE_CHANNELS
  const blaeck::blaeck_detail::StateChannelEntry &stateMeta(int index) const { return _stateChannels[index]; }
  int stateIndex(blaeck::BlaeckString name) const { return _findStateChannel(0, name); }
#endif
#if BLAECK_ENABLE_EVENTS
  const blaeck::blaeck_detail::EventChannelEntry &eventMeta(int index) const { return _eventChannels[index]; }
  const blaeck::blaeck_detail::EventTypeEntry &eventType(int index) const { return _eventTypes[index]; }
  int eventIndex(const char *name) const { return _findEventChannel(0, name); }
#endif
  void cleanCatalogs()
  {
    _commandCatalogDirty = false;
#if BLAECK_ENABLE_SIGNAL_META
    _signalConfigDirty = false;
#endif
#if BLAECK_ENABLE_STATE_CHANNELS
    _stateCatalogDirty = false;
#endif
#if BLAECK_ENABLE_EVENTS
    _eventCatalogDirty = false;
#endif
  }
  bool dirtyCatalogs() const
  {
    bool dirty = _commandCatalogDirty;
#if BLAECK_ENABLE_SIGNAL_META
    dirty |= _signalConfigDirty;
#endif
#if BLAECK_ENABLE_STATE_CHANNELS
    dirty |= _stateCatalogDirty;
#endif
#if BLAECK_ENABLE_EVENTS
    dirty |= _eventCatalogDirty;
#endif
    return dirty;
  }
};

static_assert(std::is_same<
    decltype(std::declval<Blaeck &>().begin(std::declval<FakeStream &>())),
    BlaeckBeginRef>::value, "Stream begin returns the unified handle");
static_assert(std::is_same<
    decltype(std::declval<Blaeck &>().begin(std::declval<FakeServer<> &>())),
    BlaeckBeginRef>::value, "Server begin returns the unified handle");

static std::vector<byte> opened, closed;
static void onOpen(byte slot) { opened.push_back(slot); }
static void onClose(byte slot) { closed.push_back(slot); }
static Blaeck *callbackDevice;
static void detachInCallback(byte) { callbackDevice->end(); }
static std::vector<std::string> pings;
static void onPing(const char *, const char *const *params, byte count)
{
  pings.push_back(count > 0 ? params[0] : "");
}

static void assertLibraryIdentity(const std::string &frames)
{
  const std::string identity = std::string("blaeck") + '\0' + BLAECK_VERSION + '\0';
  assert(frames.find(identity) != std::string::npos);
  assert(frames.find("BlaeckSerial") == std::string::npos);
  assert(frames.find("BlaeckTCP") == std::string::npos);
}

static void sessionBehavior(bool buffered)
{
  opened.clear();
  closed.clear();
  pings.clear();
  FakeServer<> server;
  SocketState host1, host2, terminal, excess, replacement;
  server.pending = {&host1, &host2, &terminal};
  Capture debug;
  Blaeck device;
  device.setBufferedWrites(buffered);
  device.begin(server).withClients(3).withSignals(1).withDebugStream(&debug);
  assert(device.isBufferedWrites() == buffered);
  assert(server.noDelay == BLAECK_TCP_NO_DELAY_DEFAULT);
  device.setClientConnectedCallback(onOpen);
  device.setClientDisconnectedCallback(onClose);
  device.onCommand("Ping", onPing);
  float value = 12.5f;
  device.addSignal(F("Value"), &value);
  device.read();
  device.read();
  device.read();
  assert((opened == std::vector<byte>{0, 1, 2}));
  assert(host1.noDelay == BLAECK_TCP_NO_DELAY_DEFAULT);
  assert(host1.stopTimeout == BLAECK_TCP_STOP_TIMEOUT_MS);
  assert(host1.input.empty()); // Silent connections are accepted.
  assert(debug.text.find("192.0.2.1:1234") != std::string::npos);
  device.Terminal.print("hello");
  assert(host1.output == "hello" && host2.output == "hello" && terminal.output == "hello");

  host1.output.clear();
  host2.output.clear();
  terminal.output.clear();
  host1.input = "<BLAECK.GET_DEVICES>";
  device.read();
  device.read(); // Consume the one-time restart notification.
  assert(!host1.output.empty() && host2.output.empty() && terminal.output.empty());
  assertLibraryIdentity(host1.output);
  assert(debug.text.find("Client #0 is the host\r\n") != std::string::npos);

  // A second host takes over; the first is closed, as a disconnect.
  host1.output.clear();
  debug.text.clear();
  host2.input = "<BLAECK.GET_DEVICES>";
  device.read();
  assert(!host1.open && host1.stops == 1 && host1.output.empty());
  assert(!host2.output.empty() && terminal.output.empty());
  assert((closed == std::vector<byte>{0}));
  assert(debug.text == "Client #1 is the host\r\n"
                       "Client #0 disconnected: replaced as host\r\n"
                       "<BLAECK.GET_DEVICES>\r\n");
  host2.output.clear();
  device.Terminal.print("terminal");
  assert(host2.output.empty() && terminal.output == "terminal");
  terminal.output.clear();
  device.writeAll();
  assert(!host2.output.empty() && terminal.output.empty());

  // A terminal's command runs unacknowledged, with no failed-write warning.
  host2.output.clear();
  debug.text.clear();
  terminal.input = "<Ping,t>";
  device.read();
  assert((pings == std::vector<std::string>{"t"}));
  assert(terminal.output.empty() && host2.output.empty());
  assert(debug.text.find("Incomplete transport write") == std::string::npos);

  // Round-robin fairness and independent parsers.
  pings.clear();
  terminal.input = "<Ping,t";
  host2.input = "<Ping,a><Ping,b>";
  device.read();
  assert((pings == std::vector<std::string>{"a"}));
  terminal.input += "1>";
  device.read();
  assert((pings == std::vector<std::string>{"a", "t1"}));
  assert(host2.input == "<Ping,b>");
  device.read();
  assert((pings == std::vector<std::string>{"a", "t1", "b"}));
  assert(terminal.output.empty() && !host2.output.empty());

  size_t before = allocations;
  server.pending.push_back(&replacement);
  device.read();
  assert(allocations == before); // No adapter/client-array allocation per connection.
  assert(opened.back() == 0);
  host2.output.clear();
  device.Terminal.print("new terminal");
  assert(replacement.output == "new terminal"); // Reused slot has no old host state.
  assert(host2.output.empty());

  server.pending.push_back(&excess);
  device.read();
  assert(!excess.open && excess.stops == 1 && opened.size() == 4);
  assert(excess.stopTimeout == BLAECK_TCP_STOP_TIMEOUT_MS);

  host2.open = false;
  device.read();
  assert((closed == std::vector<byte>{0, 1}));
  terminal.output.clear();
  replacement.output.clear();
  device.writeAll();
  assert(terminal.output.empty() && replacement.output.empty()); // No host, no frames.

  device.end();
  assert(!terminal.open && !replacement.open);
  assert(device.transportError() == Blaeck::TransportError::NotStarted);
  size_t accepts = server.accepts;
  device.read();
  assert(server.accepts == accepts);
}

template<class Socket>
static void optionalPeerDiagnostics(const std::string &expected)
{
  for (bool buffered : {false, true})
  {
    FakeServer<Socket> server;
    SocketState client;
    Capture debug;
    Blaeck device;
    device.begin(server).withClients(1).withDebugStream(&debug);
    device.setBufferedWrites(buffered);
    server.pending.push_back(&client);
    device.read();
    assert(debug.text == expected);
    assert(device.transportError() == Blaeck::TransportError::None);
    device.Terminal.print("terminal");
    assert(client.output == "terminal");
    client.output.clear();
    client.input = "<BLAECK.GET_DEVICES>";
    device.read();
    assertLibraryIdentity(client.output);
    assert(debug.text.find("Client #0 is the host\r\n") != std::string::npos);
    device.end();
    assert(!client.open);
  }
}

static void lifecycleAndErrors()
{
  PlainServer first;
  FakeServer<> second;
  SocketState a, b;
  Capture debug;
  {
    Blaeck one, two;
    first.pending.push_back(&a);
    second.pending.push_back(&b);
    auto handle = one.begin(first).withClients(1).withDebugStream(&debug);
    two.begin(second).withClients(1);
    one.read();
    two.read();
    one.Terminal.print("one");
    two.Terminal.print("two");
    assert(a.output == "one" && b.output == "two");
    handle.withClients(2);
    assert(one.transportError() == Blaeck::TransportError::ClientLimitLocked);
    assert(debug.text.find("already set up") != std::string::npos);
  }
  assert(!a.open && !b.open);

  // Adapter allocation, session-array allocation, and client-array allocation failures.
  for (int stage = 0; stage < 3; ++stage)
  {
    FakeServer<> server;
    Blaeck device;
    debug.text.clear();
    if (stage == 0)
      failAfter = 0;
    auto handle = device.begin(server).withClients(2).withDebugStream(&debug);
    if (stage > 0)
      failAfter = stage - 1;
    device.read();
    failAfter = -1;
    assert(device.transportError() == Blaeck::TransportError::OutOfMemory);
    assert(server.accepts == 0 && debug.text.find("allocation failed") != std::string::npos);
    handle.withClients(0);
    assert(device.transportError() == Blaeck::TransportError::OutOfMemory);
    size_t before = allocations;
    device.read();
    assert(before == allocations); // Failure is latched, not a tight allocation retry loop.
    device.begin(server).withClients(1);
    device.read();
    assert(device.transportError() == Blaeck::TransportError::OutOfMemory);
    assert(before == allocations && server.accepts == 0);
    FakeStream stream;
    device.begin(stream);
    device.read();
    assert(device.transportError() == Blaeck::TransportError::OutOfMemory);
    assert(stream.data.output.empty() && before == allocations);
    device.end();
    device.begin(stream);
    assert(device.transportError() == Blaeck::TransportError::BeginAlreadyCalled);
  }
  Blaeck device;
  device.begin(first).withClients(0).withDebugStream(&debug);
  assert(device.transportError() == Blaeck::TransportError::InvalidClientCount);
  Blaeck manyClients;
  manyClients.begin(second).withClients(255);
  manyClients.read(); // Regression: byte-sized round-robin counter would loop forever at 255.
  assert(manyClients.transportError() == Blaeck::TransportError::None);
}

static void detachFromCallbacks()
{
  FakeServer<> server;
  SocketState a, b;
  Blaeck device;
  callbackDevice = &device;
  device.setClientConnectedCallback(detachInCallback);
  device.begin(server);
  server.pending.push_back(&a);
  device.read();
  assert(!a.open && device.transportError() == Blaeck::TransportError::NotStarted);
  Blaeck disconnected;
  callbackDevice = &disconnected;
  disconnected.setClientDisconnectedCallback(detachInCallback);
  disconnected.begin(server);
  server.pending.push_back(&b);
  disconnected.read();
  b.open = false;
  disconnected.read();
  assert(disconnected.transportError() == Blaeck::TransportError::NotStarted);

  // Detaching from the disconnect callback of a host takeover.
  SocketState first, second;
  Blaeck takeover;
  callbackDevice = &takeover;
  takeover.setClientDisconnectedCallback(detachInCallback);
  takeover.begin(server);
  server.pending = {&first, &second};
  takeover.read();
  takeover.read();
  first.input = "<BLAECK.GET_DEVICES>";
  takeover.read();
  first.output.clear();
  second.input = "<BLAECK.WRITE_SYMBOLS>";
  takeover.read();
  assert(!first.open && !second.open && second.output.empty());
  assert(takeover.transportError() == Blaeck::TransportError::NotStarted);
  callbackDevice = nullptr;
}

static void unifiedConnections()
{
  FakeStream stream;
  FakeServer<> server;
  SocketState host;
  Blaeck device;
  size_t before = allocations;
  auto handle = device.begin(stream);
  assert(allocations == before); // Stream attachment creates no TCP adapter or client array.
  assert(device.isBufferedWrites() == BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT);
  stream.data.input = "<BLAECK.GET_DEVICES><BLAECK.GET_DEVICES>";
  device.read();
  assert(stream.data.input == "<BLAECK.GET_DEVICES>");
  assertLibraryIdentity(stream.data.output);
  device.read();
  stream.data.output.clear();
  device.Terminal.print("text");
  assert(stream.data.output == "text");

  {
    Blaeck tcp;
    tcp.begin(server).withClients(1);
    assert(tcp.isBufferedWrites() == BLAECK_TCP_BUFFERED_WRITES_DEFAULT);
    server.pending.push_back(&host);
    tcp.read();
  }
  assert(!host.open && stream.data.open);

  for (bool buffered : {false, true})
  {
    Blaeck serial, tcp;
    serial.setBufferedWrites(buffered);
    tcp.setBufferedWrites(buffered);
    serial.begin(stream);
    tcp.begin(server);
    assert(serial.isBufferedWrites() == buffered);
    assert(tcp.isBufferedWrites() == buffered);
  }
  handle.withClients(1);
  assert(device.transportError() == Blaeck::TransportError::NotServer);
  device.end();
  assert(stream.data.open);
  stream.data.output.clear();
  device.Terminal.print("detached");
  assert(stream.data.output.empty());

  FakeStream usb;
  PacketProbe probe;
  probe.begin(usb);
  probe.sendPacket(BLAECK_USB_PACKET_BYTES);
  assert(usb.data.output == std::string(BLAECK_USB_PACKET_BYTES, 'x') + "\n");
  assert(usb.flushes == 1);
  usb.data.output.clear();
  probe.sendPacket(BLAECK_USB_PACKET_BYTES - 1);
  assert(usb.data.output == std::string(BLAECK_USB_PACKET_BYTES - 1, 'x'));
  assert(usb.flushes == 2);
}

static void diagnosticMessages()
{
  Capture debug;
  Blaeck unattached;
  assert(unattached.printTransportError(&debug));
  assert(debug.text.find("begin(stream) or begin(server)") != std::string::npos);

  auto handler = [](const char *, const char *const *, byte) {};
  for (bool tcp : {false, true})
  {
    FakeStream stream;
    FakeServer<> server;
    Blaeck device;
    auto setup = tcp ? device.begin(server) : device.begin(stream);
    setup.withSignals(0).withCommands(1).withDebugStream(&debug);
    debug.text.clear();
    assert(!device.printRejections(&debug) && debug.text.empty());

    float value = 0;
    device.addSignal("RAM", &value);
    device.addSignal(F("Flash"), &value);
    assert(debug.text.find("Dropped 'RAM': table full at 0.") != std::string::npos);
    assert(debug.text.find("Dropped 'Flash': table full at 0.") != std::string::npos);
    assert(debug.text.find("Increase .withSignals() on the original begin() chain")
           != std::string::npos);
    assert(debug.text.find("begin(Serial)") == std::string::npos);
    device.onCommand("BLAECK.RESERVED", handler);
    assert(device.getRejectedCommandCount() == 1);

    debug.text.clear();
    assert(device.printRejections(&debug));
    assert(debug.text.find("2 signal registrations rejected; table capacity: 0.")
           != std::string::npos);
    assert(debug.text.find("1 command registrations rejected; table capacity: 1.")
           != std::string::npos);
    assert(debug.text.find("invalid or conflicting names") != std::string::npos);
    assert(debug.text.find("Increase") == std::string::npos);
    assert(debug.text.find("begin(Serial)") == std::string::npos);
  }

  // Every table can fail allocation without being full.
  for (int table = 0; table < 5; ++table)
  {
    FakeStream stream;
    Blaeck device;
    device.begin(stream).withSignals(1).withCommands(1).withStateChannels(1)
        .withEventChannels(1).withEventTypes(1).withDebugStream(&debug);
    float value = 0;
    debug.text.clear();
    failAfter = table == 4 ? 1 : 0; // For event types, allocate the channel first.
    switch (table)
    {
    case 0: device.addSignal(F("Value"), &value); break;
    case 1: device.onCommand("COMMAND", handler); break;
    case 2: device.addStateChannel(F("Value"), &value); break;
    case 3:
    case 4: device.addEventChannel(F("Activity"), F("started")); break;
    }
    failAfter = -1;
    assert(device.hasRejections());
    assert(debug.text.find("No RAM") != std::string::npos);
    assert(debug.text.find("table full") == std::string::npos);
    assert(debug.text.find("Increase") == std::string::npos);
  }
}

static void crc32Behavior()
{
  blaeck::detail::BlaeckCRC32 crc;
  const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  assert(crc.calc() == 0);
  crc.add(nullptr, 0);
  assert(crc.calc() == 0);
  crc.add(digits, sizeof(digits));
  assert(crc.calc() == 0xCBF43926UL);
  assert(crc.calc() == 0xCBF43926UL); // Reading the checksum does not finalize the state.

  for (size_t split = 0; split <= sizeof(digits); ++split)
  {
    crc.restart();
    assert(crc.calc() == 0);
    crc.add(digits, split);
    crc.calc();
    crc.add(digits + split, sizeof(digits) - split);
    assert(crc.calc() == 0xCBF43926UL);
  }

  crc.restart();
  for (uint8_t value : digits)
    crc.add(value);
  assert(crc.calc() == 0xCBF43926UL);

  uint8_t binary[256];
  for (size_t i = 0; i < sizeof(binary); ++i)
    binary[i] = static_cast<uint8_t>(i);
  blaeck::detail::BlaeckCRC32 other;
  other.add(binary, sizeof(binary));
  assert(other.calc() == 0x29058C73UL);
  assert(crc.calc() == 0xCBF43926UL);
}

struct DataFrame
{
  std::vector<int> ids;
  std::vector<std::string> values;
  uint64_t timestamp = 0;
  byte mode = 0;
  byte flags = 0;
  byte status = 0;
  std::string statusPayload;
  uint16_t schemaHash = 0;
};

static std::vector<DataFrame> takeData(std::string &output, const std::vector<int> &widths)
{
  std::vector<DataFrame> result;
  const std::string marker = std::string("<BLAECK:") + char(0xD2) + ':';
  size_t start = 0;
  while ((start = output.find(marker, start)) != std::string::npos)
  {
    const size_t end = output.find("/BLAECK>\r\n", start);
    assert(end != std::string::npos && end >= start + 31);
    size_t p = start + marker.size() + 4;
    assert(output[p++] == ':');
    DataFrame frame;
    frame.flags = static_cast<byte>(output[p++]);
    assert(output[p++] == ':');
    frame.schemaHash = static_cast<uint16_t>(static_cast<byte>(output[p]) |
                                             (static_cast<byte>(output[p + 1]) << 8));
    p += 2;
    assert(output[p++] == ':');
    frame.mode = static_cast<byte>(output[p++]);
    if (frame.mode != BLAECK_NO_TIMESTAMP)
    {
      memcpy(&frame.timestamp, output.data() + p, 8);
      p += 8;
    }
    assert(output[p++] == ':');
    while (p < end - 9)
    {
      uint16_t id;
      memcpy(&id, output.data() + p, 2);
      p += 2;
      assert(id < widths.size());
      const size_t size = widths[id] < 0 ? static_cast<byte>(output[p++]) : widths[id];
      assert(p + size <= end - 9);
      frame.ids.push_back(id);
      frame.values.push_back(output.substr(p, size));
      p += size;
    }
    assert(p == end - 9);
    frame.status = static_cast<byte>(output[end - 9]);
    frame.statusPayload = output.substr(end - 8, 4);
    uint32_t actual;
    memcpy(&actual, output.data() + end - 4, 4);
    blaeck::detail::BlaeckCRC32 crc;
    crc.add(reinterpret_cast<const byte *>(output.data() + start + 8), end - 4 - start - 8);
    assert(crc.calc() == actual);
    result.push_back(frame);
    start = end + 10;
  }
  output.clear();
  return result;
}

static void expectData(FakeStream &stream, const std::vector<int> &widths, const std::vector<int> &ids)
{
  const auto frames = takeData(stream.data.output, widths);
  if (ids.empty())
    assert(frames.empty());
  else
    assert(frames.size() == 1 && frames[0].ids == ids);
}

static void command(Blaeck &device, FakeStream &stream, const char *text)
{
  stream.data.input = text;
  device.read();
}

static std::string commandFramePayload(const std::string &output, byte key, uint32_t messageId)
{
  const std::string marker = std::string("<BLAECK:") + char(key) + ':';
  const size_t start = output.find(marker);
  assert(start != std::string::npos);
  size_t p = start + marker.size();
  assert(p + 5 <= output.size());
  uint32_t actualId;
  memcpy(&actualId, output.data() + p, 4);
  assert(actualId == messageId);
  p += 4;
  assert(output[p++] == ':');
  const size_t end = output.find("/BLAECK>\r\n", p);
  assert(end != std::string::npos);
  assert(output.find(marker, end) == std::string::npos);
  return output.substr(p, end - p);
}

static uint32_t commandHash(const std::string &value)
{
  uint32_t hash = 2166136261UL;
  for (unsigned char c : value)
    hash = (hash ^ c) * 16777619UL;
  return hash;
}

static void commandBufferBoundaries(bool tcp, bool buffered)
{
  const size_t capacity = BLAECK_COMMAND_MAX_CHARS_DEFAULT;
  assert(capacity >= 32);
  FakeStream stream;
  FakeServer<> server;
  SocketState host;
  Blaeck device;
  if (tcp)
  {
    device.begin(server).withClients(1);
    server.pending.push_back(&host);
  }
  else
    device.begin(stream);
  device.setBufferedWrites(buffered);
  device.onCommand("Ping", onPing);
  SocketState &io = tcp ? host : stream.data;
  const auto receive = [&](const std::string &input)
  {
    io.input += input;
    device.read();
    assert(io.input.empty());
  };
  const auto expectAck = [&](const std::string &payload, uint32_t id, byte reason)
  {
    const std::string ack = commandFramePayload(io.output, 0xA5, id);
    assert(ack.size() == 10);
    uint32_t actualHash, nameHash;
    memcpy(&actualHash, ack.data(), 4);
    memcpy(&nameHash, ack.data() + 4, 4);
    assert(actualHash == commandHash(payload));
    assert(nameHash == commandHash("Ping"));
    assert(static_cast<byte>(ack[8]) == (reason == BLAECK_ACK_OK ? 0 : 1));
    assert(static_cast<byte>(ack[9]) == reason);
    io.output.clear();
  };
  receive("<BLAECK.GET_DEVICES>");
  device.read();
  io.output.clear();
#if BLAECK_ENABLE_COMMAND_META
  receive("<BLAECK.WRITE_COMMANDS>");
  const std::string catalog = commandFramePayload(io.output, 0xA0, 0);
  assert(catalog.size() >= 4);
  uint16_t advertised;
  memcpy(&advertised, catalog.data() + 2, 2);
  assert(advertised == capacity - 1);
  io.output.clear();
#endif

  for (size_t length : {capacity - 1, capacity, capacity + 257})
  {
    pings.clear();
    const std::string payload = "Ping," + std::string(length - 5, 'x');
    const size_t split = capacity > 255 ? 255 : capacity / 2;
    receive("<" + payload.substr(0, split));
    assert(pings.empty() && io.output.empty());
    receive(payload.substr(split) + ">");
    if (length < capacity)
    {
      assert(pings == std::vector<std::string>({payload.substr(5)}));
      expectAck(payload, 0, BLAECK_ACK_OK);
    }
    else
    {
      assert(pings.empty());
      expectAck(payload.substr(0, capacity - 1), 0, BLAECK_ACK_TRUNCATED);
    }
    pings.clear();
    receive("<Ping,next>");
    assert(pings == std::vector<std::string>({"next"}));
    expectAck("Ping,next", 0, BLAECK_ACK_OK);
  }

  // Restarting an oversized, unfinished command also clears its overflow verdict.
  pings.clear();
  receive("<Ping," + std::string(capacity + 257, 'x'));
  assert(pings.empty() && io.output.empty());
  receive("<Ping,recovered>");
  assert(pings == std::vector<std::string>({"recovered"}));
  expectAck("Ping,recovered", 0, BLAECK_ACK_OK);

  std::string prefix;
  while (prefix.size() + 3 + 11 < capacity)
    prefix += "#1:";
  prefix += "#42:";
  if (capacity >= 300)
    assert(prefix.size() > 255);
  pings.clear();
  receive("<" + prefix + "Ping,ok>");
  assert(pings == std::vector<std::string>({"ok"}));
  expectAck("Ping,ok", 42, BLAECK_ACK_OK);

  pings.clear();
  const std::string oversized = "Ping," + std::string(capacity, 'x');
  receive("<" + prefix + oversized + ">");
  assert(pings.empty());
  expectAck(oversized.substr(0, capacity - 1 - prefix.size()), 42, BLAECK_ACK_TRUNCATED);
}

static void flashSignalText(bool buffered)
{
  for (int naming = 0; naming < 3; ++naming)
  for (bool timestamped : {false, true})
  for (bool tracking : {false, true})
  {
    FakeStream stream;
    Capture debug;
    Blaeck device;
    device.begin(stream).withSignals(1).withDebugStream(&debug);
    device.setBufferedWrites(buffered);
    device.setTimestampMode(BLAECK_MICROS);
    hostMillis() = 0;
    hostMicros() = 99;
    char ram[32] = "Idle";
    auto signal = naming == 0 ? device.addSignal("Status", F("Idle"))
                             : device.addSignal(F("Status"), F("Idle"));
    signal.writeAtInterval(BLAECK_OFF);
    if (tracking)
      signal.writeOnChange(BLAECK_ANY_CHANGE, 0);
    assert(device.findSignalIndex("Status") == 0);
    assert(device.findSignalIndex(F("Status")) == 0);
    assert(device.findSignalIndex(F("Missing")) == -1);
    assert(device.findSignalIndex(nullptr) == -1);
    const auto expectText = [&](const std::string &expected, uint64_t timestamp)
    {
      const auto frames = takeData(stream.data.output, {-1});
      assert(frames.size() == 1 && frames[0].values == std::vector<std::string>({expected}));
      assert(frames[0].timestamp == timestamp);
    };
    device.writeAll();
    expectText("Idle", 99);
    const __FlashStringHelper *flash = F("Running");
    const uint64_t explicitTime = timestamped ? 123456 : 99;
    if (naming == 0)
    {
      if (timestamped) device.write("Status", flash, explicitTime);
      else device.write("Status", flash);
    }
    else if (naming == 1)
    {
      if (timestamped) device.write(F("Status"), flash, explicitTime);
      else device.write(F("Status"), flash);
    }
    else
    {
      if (timestamped) device.write(0, flash, explicitTime);
      else device.write(0, flash);
    }
    expectText("Running", explicitTime);
    device.writeIfDue();
    expectData(stream, {-1}, {});

    strcpy(ram, "Running");
    if (naming == 0)
    {
      if (timestamped) device.write("Status", ram, explicitTime);
      else device.write("Status", ram);
    }
    else if (naming == 1)
    {
      if (timestamped) device.write(F("Status"), ram, explicitTime);
      else device.write(F("Status"), ram);
    }
    else
    {
      if (timestamped) device.write(0, ram, explicitTime);
      else device.write(0, ram);
    }
    expectText("Running", explicitTime);
    device.writeIfDue();
    expectData(stream, {-1}, {});
    strcpy(ram, "Changed");
    if (tracking)
    {
      device.writeIfDue();
      expectText("Changed", 99);
    }
    device.write(F("Status"), F("Running"));
    expectText("Running", 99);
    strcpy(ram, "Not retained now");
    device.writeIfDue();
    expectData(stream, {-1}, {});
    command(device, stream, "<BLAECK.WRITE_DATA>");
    expectText("Running", 99);

    device.write(F("Status"), nullptr);
    expectText("", 99);
    device.write("Status", nullptr, 456);
    expectText("", 456);
    device.write(0, nullptr);
    expectText("", 99);
    device.write(0, nullptr, 457);
    expectText("", 457);
    device.write(F("Status"), F(""));
    expectText("", 99);
    device.write(nullptr, F("Ignored"));
    device.write(F("Missing"), 42);
    expectData(stream, {-1}, {});

    signal.writeAtInterval(BLAECK_ON_CHANGE, BLAECK_ANY_CHANGE);
    command(device, stream, "<BLAECK.ACTIVATE,1000>");
    device.writeIfDue();
    expectText("", 99);
    command(device, stream, "<BLAECK.PAUSE_WRITES,FOREVER>");
    device.write(F("Status"), F("Paused"));
    expectData(stream, {-1}, {});
    command(device, stream, "<BLAECK.RESUME_WRITES>");
    hostMillis() = 1000;
    device.writeIfDue();
    expectText("Paused", 99);
    command(device, stream, "<BLAECK.PAUSE_WRITES,FOREVER>");
    strcpy(ram, "Paused");
    device.write(F("Status"), ram);
    command(device, stream, "<BLAECK.RESUME_WRITES>");
    hostMillis() = 2000;
    device.writeIfDue();
    expectData(stream, {-1}, {}); // Equal RAM text matches the previous flash snapshot.
    command(device, stream, "<BLAECK.PAUSE_WRITES,FOREVER>");
    device.write(F("Status"), F("Paused"));
    command(device, stream, "<BLAECK.RESUME_WRITES>");
    hostMillis() = 3000;
    device.writeIfDue();
    expectData(stream, {-1}, {}); // Equal flash text does not compare pointer addresses.
    assert(!device.hasRejections());

    device.clearAllSignals();
    strcpy(ram, "Reused RAM");
    device.addSignal(F("Status"), ram);
    device.writeAll();
    expectText("Reused RAM", 99); // Reused slots clear the flash flag.
  }
}

static void flashNamesAndFailures()
{
  FakeStream stream;
  Capture debug;
  Blaeck device;
  device.begin(stream).withSignals(2).withDebugStream(&debug);
  float value = 0;
  device.addSignal(F("VeryLongSignalNameBeyondAnyTemporaryNameBuffer_"), &value).withNameSuffix(255);
  assert(device.findSignalIndex(F("VeryLongSignalNameBeyondAnyTemporaryNameBuffer_255")) == 0);
  assert(device.findSignalIndex(F("VeryLongSignalNameBeyondAnyTemporaryNameBuffer_25")) == -1);
  device.write(F("VeryLongSignalNameBeyondAnyTemporaryNameBuffer_255"), 3.5f);
  assert(value == 3.5f);
  device.write(F("VeryLongSignalNameBeyondAnyTemporaryNameBuffer_255"), false, 99);
  assert(value == 0);
  device.write(nullptr, 42);
  device.write(nullptr, 42, 99);
  takeData(stream.data.output, {4});
  auto text = device.addSignal("Text", F("a"));
  text.writeAtInterval(BLAECK_OFF).writeOnChange(BLAECK_ANY_CHANGE, 0);
  device.writeIfDue();
  expectData(stream, {4, -1}, {1});
  failAfter = 0;
  device.write(F("Text"), F("Longer flash text"));
  failAfter = -1;
  expectData(stream, {4, -1}, {});
  assert(debug.text.find("No RAM for signal text snapshot") != std::string::npos);
  device.writeIfDue();
  auto frames = takeData(stream.data.output, {4, -1});
  assert(frames.size() == 1 && frames[0].values[0] == "Longer flash text");
  const __FlashStringHelper *longText = F(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "tail");
  device.write(1, longText);
  frames = takeData(stream.data.output, {4, -1});
  assert(frames.size() == 1 && frames[0].values[0] == std::string(255, 'a'));
  text.writeOnChange(BLAECK_OFF); // Also exercise direct flash reads without a snapshot.
  device.write(1, longText);
  frames = takeData(stream.data.output, {4, -1});
  assert(frames.size() == 1 && frames[0].values[0] == std::string(255, 'a'));
#if BLAECK_ENABLE_STATE_CHANNELS
  device.addStateChannel(F("Long"), BlaeckText);
  debug.text.clear();
  device.writeState(F("Long"), longText);
  auto state = commandFramePayload(stream.data.output, 0x95, 0);
  assert(state.size() == 261 && static_cast<byte>(state[5]) == 255);
  assert(state.substr(6) == std::string(255, 'a'));
  assert(debug.text.find("State text truncated") != std::string::npos);
  stream.data.output.clear();
  debug.text.clear();
  device.writeState("Long", longText);
  state = commandFramePayload(stream.data.output, 0x95, 0);
  assert(state.substr(6) == std::string(255, 'a'));
  assert(debug.text.empty()); // Truncation is still warned only once.
#endif
}

template<class T>
static void flashNumericWrites()
{
  FakeStream stream;
  Blaeck device;
  device.begin(stream);
  T value = 0;
  device.addSignal(F("Value"), &value);
  device.write(F("Value"), static_cast<T>(1));
  assert(value == 1);
  device.write(F("Value"), static_cast<T>(0), 123);
  assert(value == 0);
  device.addStateChannel(F("State"), &value);
  device.writeState(F("State"), static_cast<T>(1));
#if BLAECK_ENABLE_STATE_CHANNELS
  assert(value == 1);
#else
  assert(value == 0);
#endif
}

static const char *flashTestGetter() { return "Getter"; }

static void flashStateText(bool buffered)
{
  FakeStream stream;
  Capture debug;
  Blaeck device;
  device.begin(stream).withStateChannels(8).withDebugStream(&debug);
  device.setBufferedWrites(buffered);
  device.addStateChannel("Bound", F("Initial"));
  device.addStateChannel(F("FlashBound"), F("Initial"));
  device.addStateChannel(F("Pushed"), BlaeckText);
  device.addStateChannel(F("Getter"), BlaeckText).withStateText(flashTestGetter);
  device.addStateChannel(F("LongStateChannelNameThatExceedsTheNormalRamNameLimit"), F("Long"));
  float number = 0;
  device.addStateChannel(F("Numeric"), &number);
  const auto expectText = [&](const std::string &value)
  {
#if BLAECK_ENABLE_STATE_CHANNELS
    const auto payload = commandFramePayload(stream.data.output, 0x95, 0);
    assert(payload.size() == 6 + value.size());
    assert(static_cast<byte>(payload[5]) == value.size());
    assert(payload.substr(6) == value);
#else
    assert(stream.data.output.empty());
    (void)value;
#endif
    stream.data.output.clear();
  };
  device.writeState(F("Bound"));
  expectText("Initial");
  device.writeState("FlashBound");
  expectText("Initial");
  device.writeState(F("LongStateChannelNameThatExceedsTheNormalRamNameLimit"));
  expectText("Long");
  device.writeState(F("Getter"));
  expectText("Getter");
  device.writeState("Pushed", F("Flash"));
  expectText("Flash");
  device.writeState(F("Pushed"), F("Flash"));
  expectText("Flash");
  char ram[] = "RAM";
  device.writeState(F("Pushed"), ram);
  expectText("RAM");
  device.writeState(F("Pushed")); // A push does not bind or retain a value.
  assert(stream.data.output.empty());
  device.writeState("Pushed", nullptr);
  device.writeState(F("Pushed"), nullptr);
  assert(stream.data.output.empty());
  device.writeState(F("Pushed"), F(""));
  expectText("");
  device.writeState(F("Bound"), F("Cannot replace bound text"));
  device.writeState(F("Getter"), F("Cannot replace a getter"));
  assert(stream.data.output.empty());
  device.addStateChannel(F("Bound"), ram);
  device.writeState("Bound");
  expectText("RAM");
  device.addStateChannel("Bound", F("Again"));
  device.writeState(F("Bound"));
  expectText("Again");
  device.writeState(F("Numeric"), 12.5);
#if BLAECK_ENABLE_STATE_CHANNELS
  assert(number == 12.5f);
  stream.data.output.clear();
  device.writeStateChannels();
  const auto catalog = commandFramePayload(stream.data.output, 0x90, 0);
  assert(catalog.find("Again") != std::string::npos && catalog.find("Initial") != std::string::npos);
  stream.data.output.clear();
#endif
  device.onTextCommand("SET_TEXT", onPing).withOwnState(F("Bound"), ram);
  device.writeCommandState(F("SET_TEXT"));
#if BLAECK_ENABLE_STATE_CHANNELS && BLAECK_ENABLE_COMMAND_META
  expectText("RAM"); // Taking over a flash-bound slot resets its storage flag.
  device.writeCommandState("SET_TEXT");
  expectText("RAM");
#else
  assert(stream.data.output.empty());
#endif
  device.writeCommandState(nullptr);
  device.writeCommandState(F("Missing"));
  device.clearAllCommandHandlers();
  device.clearAllStateChannels();
  device.addStateChannel(F("Reused"), F("New"));
  device.writeState(F("Reused"));
  expectText("New");
}

static void storedConfigurationStrings()
{
  using blaeck::BlaeckString;
  using blaeck::detail::StoredString;
  StoredString saved;
  {
    char text[] = "Temporary";
    assert(saved.set(text));
    text[0] = 'X';
  }
  assert(BlaeckString(saved) == "Temporary");
  StoredString shared = saved;
  const size_t before = allocations;
  assert(saved.set(F("Temporary"))); // Equal contents preserve backing storage.
  assert(allocations == before);
  failAfter = 0;
  assert(!saved.set("Replacement"));
  assert(BlaeckString(saved) == "Temporary");
  assert(saved.set(F("Flash")));
  assert(allocations == before + 1); // Only the failed RAM copy tried to allocate.
  failAfter = -1;
  assert(BlaeckString(saved) == "Flash");
  assert(BlaeckString(shared) == F("Temporary"));
  // Self-assignment, through a reference so clang's -Wself-assign-overloaded allows it.
  const auto &self = shared;
  shared = self;
  saved = shared;
  shared = nullptr;
  assert(BlaeckString(saved) == "Temporary");
  assert(saved.set(""));
  assert(BlaeckString(saved) == F(""));
  saved = nullptr;
  assert(saved == nullptr);
}

static void ordinaryConfiguration(bool buffered)
{
  using blaeck::BlaeckString;
  FakeStream stream;
  ConfigurationProbe device;
  device.begin(stream).withSignals(2).withCommands(4).withStateChannels(5)
      .withEventChannels(2).withEventTypes(5);
  device.setBufferedWrites(buffered);
  float value = 1;
  byte selected = 1;
  char unit[] = "V", icon[] = "mdi:pulse", label[] = "Voltage";
  char options[] = "Low,High", ownName[] = "Selected", signalName[] = "Value";
  char payload[] = "1,2", eventTypes[] = "start,stop", extraType[] = "reset";
  char deviceClass[] = "voltage";
  auto signal = device.addSignal("Value", &value);
  signal.withUnit(unit).withDeviceClass(deviceClass).withIcon(icon).withDisplayName(label);
  device.addSignal("Level", "Low").withDeviceClass("enum").withOptions(options);
  auto number = device.onNumberCommand("SET", onPing).withRange(0, 10, 1);
  number.withUnit(unit).withDeviceClass(deviceClass).withIcon(icon).withDisplayName(label)
      .withStateFromSignal(signalName);
  auto select = device.onSelectCommand("SELECT", onPing).withOptions(options);
  select.withOwnState(ownName, &selected);
  device.onButtonCommand("PRESS", onPing).withPressPayload(payload).withIcon(icon);
  device.onTextCommand("TEXT", onPing).withOwnState("Getter", flashTestGetter);
  auto state = device.addStateChannel("Voltage", &value);
  state.withUnit(unit).withDeviceClass(deviceClass).withIcon(icon);
  device.addStateChannel("LevelState", "Low").withDeviceClass("enum").withOptions(options);
  auto event = device.addEventChannel("Action", eventTypes);
  event.withIcon(icon).withDeviceClass("button");
  const bool added = device.addEventType("Action", extraType);
  assert(added == bool(BLAECK_ENABLE_EVENTS));

  unit[0] = icon[0] = label[0] = options[0] = ownName[0] = signalName[0] =
      payload[0] = eventTypes[0] = extraType[0] = deviceClass[0] = 'X';
  device.writeSignalConfig();
#if BLAECK_ENABLE_SIGNAL_META
  for (const char *text : {"V", "voltage", "mdi:pulse", "Voltage", "Low,High"})
    assert(stream.data.output.find(std::string(text) + '\0') != std::string::npos);
#endif
  stream.data.output.clear();
  device.writeCommands();
#if BLAECK_ENABLE_COMMAND_META
  for (const char *text : {"V", "voltage", "mdi:pulse", "Voltage", "Low,High", "Value", "Selected", "1,2", "Getter"})
    assert(stream.data.output.find(std::string(text) + '\0') != std::string::npos ||
           (!BLAECK_ENABLE_STATE_CHANNELS && (std::string(text) == "Selected" || std::string(text) == "Getter")));
  char option[8];
  assert(device.getSelectOptionNameAt("SELECT", 1, option, sizeof(option)));
  assert(strcmp(option, "High") == 0);
  assert(device.getSelectOptionIndexOf("SELECT", "Low") == 0);
  assert(device.getSelectOptionIndexOf("SELECT", "Missing") == -1);
#endif
  stream.data.output.clear();
  device.writeStateChannels();
#if BLAECK_ENABLE_STATE_CHANNELS
  for (const char *text : {"V", "voltage", "mdi:pulse", "Low,High"})
    assert(stream.data.output.find(std::string(text) + '\0') != std::string::npos);
#endif
  stream.data.output.clear();
  device.writeEventChannels();
#if BLAECK_ENABLE_EVENTS
  const auto catalog = commandFramePayload(stream.data.output, 0x80, 0);
  for (const char *text : {"Action", "mdi:pulse", "button", "start", "stop", "reset"})
    assert(catalog.find(std::string(text) + '\0') != std::string::npos);
  assert(BlaeckString(device.eventType(0).text).data() == BlaeckString(device.eventType(1).text).data());
  const size_t beforeEvent = allocations;
  stream.data.output.clear();
  device.writeEvent("Action", "stop");
  const auto ordinaryEvent = stream.data.output;
  stream.data.output.clear();
  device.writeEvent(F("Action"), F("stop"));
  assert(stream.data.output == ordinaryEvent && !ordinaryEvent.empty());
  assert(allocations == beforeEvent);
  assert(!device.addEventType(F("Action"), F("reset")));
#endif
  stream.data.output.clear();
#if BLAECK_ENABLE_COMMAND_META && BLAECK_ENABLE_STATE_CHANNELS
  device.writeCommandState("SELECT");
  assert(commandFramePayload(stream.data.output, 0x95, 0).substr(6) == "High");
  stream.data.output.clear();
  // Replacing the command's copy must not invalidate a channel sharing its old options.
  device.onSelectCommand("SELECT", onPing).withOptions("New,Other");
  assert(BlaeckString(device.stateMeta(device.stateIndex("Selected")).options) == "Low,High");
#endif
  device.cleanCatalogs();
  const size_t beforeSame = allocations;
  signal.withUnit("V").withIcon(F("mdi:pulse"));
  number.withUnit(F("V")).withStateFromSignal("Value");
  state.withUnit(F("V")).withIcon("mdi:pulse");
  event.withIcon(F("mdi:pulse"));
  assert(!device.dirtyCatalogs() && allocations == beforeSame);
  signal.withUnit("").withIcon(nullptr);
  number.withUnit(nullptr);
  state.withUnit("").withIcon(nullptr);
  event.withIcon("");
#if BLAECK_ENABLE_SIGNAL_META
  assert(device.signalMeta(0).Unit == nullptr && device.signalMeta(0).Icon == nullptr);
#endif
  assert(!device.hasRejections());
  device.clearAllCommandHandlers();
  device.clearAllStateChannels();
  device.clearAllEventChannels();
  device.clearAllSignals();
#if BLAECK_ENABLE_COMMAND_META
  assert(device.commandMeta(0).unit == nullptr && device.commandMeta(0).displayName == nullptr);
  assert(device.commandMeta(2).pressPayload == nullptr);
#endif
#if BLAECK_ENABLE_STATE_CHANNELS
  assert(device.stateMeta(0).options == nullptr && device.stateMeta(0).unit == nullptr);
#endif
#if BLAECK_ENABLE_EVENTS
  assert(device.eventMeta(0).deviceClass == nullptr && device.eventType(0).text == nullptr);
  assert(device.eventType(2).text == nullptr);
#endif
}

static void configurationAllocationFailures()
{
  using blaeck::BlaeckString;
  FakeStream stream;
  Capture debug;
  ConfigurationProbe device;
  device.begin(stream).withDebugStream(&debug).withCommands(2).withStateChannels(3);
  float value = 0;
  auto signal = device.addSignal("Signal", &value).withUnit(F("V"));
  auto number = device.onNumberCommand("SET", onPing).withRange(0, 10, 1)
      .withUnit(F("V")).withStateFromSignal(F("Signal"));
  auto state = device.addStateChannel(F("State"), &value).withUnit(F("V"));
  auto event = device.addEventChannel(F("Event"), F("start")).withIcon(F("mdi:pulse"));
  device.cleanCatalogs();
  const size_t before = allocations;
  failAfter = 0;
  signal.withUnit("Replacement");
  number.withUnit("Replacement").withStateFromSignal("Other");
  state.withUnit("Replacement");
  event.withIcon("Replacement");
  device.addEventChannel(F("Rejected"), "start,stop");
  assert(!device.addEventType("Event", "stop"));
  number.withOwnState("RejectedState", &value);
  failAfter = -1;
  assert(!device.dirtyCatalogs());
#if BLAECK_ENABLE_SIGNAL_META
  assert(BlaeckString(device.signalMeta(0).Unit) == "V");
#endif
#if BLAECK_ENABLE_COMMAND_META
  assert(BlaeckString(device.commandMeta(0).unit) == "V");
  assert(BlaeckString(device.commandMeta(0).stateSignal) == "Signal");
#endif
#if BLAECK_ENABLE_STATE_CHANNELS
  assert(BlaeckString(device.stateMeta(0).unit) == "V");
  assert(device.stateIndex("RejectedState") == -1);
#endif
#if BLAECK_ENABLE_EVENTS
  assert(BlaeckString(device.eventMeta(0).icon) == "mdi:pulse");
  assert(device.eventIndex("Rejected") == -1);
#endif
  if (allocations != before)
  {
    assert(device.hasRejections());
    assert(debug.text.find("No RAM for configuration text") != std::string::npos);
    Capture rejections;
    assert(device.printRejections(&rejections));
    assert(rejections.text.find("configuration string updates rejected") != std::string::npos);
  }
#if BLAECK_ENABLE_COMMAND_META && BLAECK_ENABLE_STATE_CHANNELS
  FakeStream freshStream;
  ConfigurationProbe fresh;
  fresh.begin(freshStream).withStateChannels(1);
  auto freshNumber = fresh.onNumberCommand("SET", onPing).withRange(0, 10, 1)
      .withStateFromSignal(F("Signal"));
  // The command name copy succeeds, but allocating the state table fails.
  failAfter = 1;
  freshNumber.withOwnState("RejectedState", &value);
  failAfter = -1;
  assert(BlaeckString(fresh.commandMeta(0).stateSignal) == "Signal");
  assert(fresh.stateIndex("RejectedState") == -1);
  number.withOwnState("AcceptedState", &value);
  assert(device.stateIndex("AcceptedState") >= 0);
  assert(BlaeckString(device.commandMeta(0).stateSignal) == "AcceptedState");
#endif
}

static void beginOnlyOnce()
{
  for (bool tcp : {false, true})
  {
    FakeStream stream;
    FakeServer<> server;
    SocketState host;
    Blaeck device;
    auto setup = tcp ? device.begin(server) : device.begin(stream);
    if (tcp)
      setup.withClients(1);
    const size_t before = allocations;
    if (tcp)
      device.begin(server).withClients(0);
    else
      device.begin(stream).withClients(0);
    assert(allocations == before);
    assert(device.transportError() == Blaeck::TransportError::BeginAlreadyCalled);
    SocketState &io = tcp ? host : stream.data;
    if (tcp)
      server.pending.push_back(&host);
    io.input = "<BLAECK.GET_DEVICES>";
    device.read(); // Rejection before the first read must not block initial setup.
    assert(io.output.find("blaeck") != std::string::npos);
    device.end();
    Capture debug;
    assert(device.printTransportError(&debug));
    assert(debug.text.find("transport ended") != std::string::npos);
  }

  for (bool tcp : {false, true})
  for (bool nextTcp : {false, true})
  for (bool ended : {false, true})
  for (bool buffered : {false, true})
  {
    hostMillis() = 0;
    FakeStream stream, otherStream;
    FakeServer<> server, otherServer;
    SocketState host;
    Capture debug, ignoredDebug;
    Blaeck device;
    device.end(); // Teardown before initialization does not consume begin().
    auto setup = tcp ? device.begin(server) : device.begin(stream);
    setup.withSignals(3).withDebugStream(&debug);
    if (tcp)
    {
      setup.withClients(2);
      server.pending.push_back(&host);
    }
    device.setBufferedWrites(buffered);
    float periodic = 10, change = 20, extra = 30;
    device.addSignal(F("Interval"), &periodic);
    device.addSignal(F("OnChange"), &change)
        .writeAtInterval(BLAECK_OFF).writeOnChange(1);
    SocketState &io = tcp ? host : stream.data;
    const auto receive = [&](const char *text)
    {
      io.input += text;
      device.read();
    };
    receive("<BLAECK.GET_DEVICES>");
    device.read(); // Send the boot notice before testing that it is not repeated.
    receive("<BLAECK.ACTIVATE,1000>");
    device.writeIfDue();
    auto frames = takeData(io.output, {4, 4});
    assert(frames.size() == 1 && frames[0].ids == std::vector<int>({0, 1}));
    receive("<BLAECK.PAUSE_WRITES,FOREVER>");
    receive("<BLAECK.RESUME_");
    io.output.clear();

    if (ended)
    {
      device.end();
      device.end();
    }
    const size_t before = allocations;
    auto rejected = nextTcp ? device.begin(otherServer) : device.begin(otherStream);
    rejected.withClients(1).withSignals(1).withStateChannels(0)
        .withEventChannels(0).withEventTypes(0).withCommands(0)
        .withDebugStream(&ignoredDebug);
    assert(allocations == before);
    assert(device.transportError() == Blaeck::TransportError::BeginAlreadyCalled);
    assert(debug.text.find("only once per instance") != std::string::npos);
    assert(ignoredDebug.text.empty() && !device.hasRejections());
    assert(device.SignalCount == 2 && device.findSignalIndex("OnChange") == 1);
    assert(device.isTimedDataActive() && device.getIntervalMs() == 1000);
    assert(device.isBufferedWrites() == buffered);
    device.writeIfDue();
    assert(io.output.empty());
    assert(otherStream.data.output.empty() && otherServer.accepts == 0);
    assert(!otherServer.noDelay);
    if (ended)
    {
      device.read();
      assert(io.output.empty() && otherServer.accepts == 0);
      assert(tcp ? !host.open : stream.data.open);
      continue;
    }

    assert(io.open);
    receive("WRITES>"); // The original receiver's partial command survived.
    device.writeIfDue();
    assert(otherStream.data.output.empty());
    assert(takeData(io.output, {4, 4}).empty()); // No baseline reset.
    hostMillis() = 1000;
    device.writeIfDue();
    frames = takeData(io.output, {4, 4});
    assert(frames.size() == 1 && frames[0].ids == std::vector<int>({0}));
    assert(frames[0].flags == 0x04); // Still active, no new boot flag.
    change = 21;
    device.writeIfDue();
    frames = takeData(io.output, {4, 4});
    assert(frames.size() == 1 && frames[0].ids == std::vector<int>({1}));
    device.addSignal(F("Extra"), &extra).writeAtInterval(BLAECK_OFF);
    assert(device.SignalCount == 3 && !device.hasRejections());

    if (tcp)
    {
      SocketState terminal;
      server.pending.push_back(&terminal);
      device.read();
      assert(terminal.open && server.pending.empty()); // Original client limit retained.
      device.end(); // Release before the socket double goes out of scope.
    }
  }
}

// The two ownership bytes of the catalog record that holds `name`: master/slave and slave ID.
// They sit right before the name, or before the payload length in a command record.
static std::string ownerOf(const std::string &payload, const char *name, size_t gap = 0)
{
  const std::string key = std::string(name) + '\0';
  const size_t at = payload.find(key);
  assert(at != std::string::npos && at >= 2 + gap);
  return payload.substr(at - 2 - gap, 2);
}

static std::string owner(byte config, byte id)
{
  return std::string(1, static_cast<char>(config)) + static_cast<char>(id);
}

// One B7 record: DeviceID, ParentID 0, DeviceFlags 0, DeviceState, then the three names.
static std::string deviceRecord(byte id, byte state, const char *name, const char *hw, const char *fw)
{
  return std::string(1, static_cast<char>(id)) + '\0' + '\0' + '\0' + static_cast<char>(state) +
         name + '\0' + hw + '\0' + fw + '\0';
}

// A B7 payload: library name and version, the record count, then the records.
static std::string deviceList(byte count, const std::string &records)
{
  return std::string("blaeck") + '\0' + BLAECK_VERSION + '\0' + static_cast<char>(count) + records;
}

// A C1 payload.
static std::string notice(byte id, byte event)
{
  return std::string(1, static_cast<char>(id)) + static_cast<char>(event);
}

static void noDeviceOwnership()
{
  FakeStream stream;
  Blaeck device;
  device.begin(stream);
  device.DeviceName = "Solo";
  device.DeviceHWVersion = "Mega";
  float value = 1;
  device.addSignal(F("Value"), &value);
  device.read();
  stream.data.output.clear();
  command(device, stream, "<BLAECK.GET_DEVICES>");
  // Without devices the list holds the board alone; its restart went out as C1 in read().
  assert(commandFramePayload(stream.data.output, 0xB7, 0) ==
         deviceList(1, deviceRecord(0, 0, "Solo", "Mega", "n/a")));
  stream.data.output.clear();
  command(device, stream, "<BLAECK.WRITE_SYMBOLS>");
  assert(ownerOf(commandFramePayload(stream.data.output, 0xB0, 0), "Value") == owner(0x00, 0));
}

static void subDevices(bool buffered)
{
  hostMillis() = 0;
  auto handler = [](const char *, const char *const *, byte) {};
  FakeStream stream;
  Capture debug;
  Blaeck device;
  device.begin(stream).withDevices(2).withDebugStream(&debug);
  device.setBufferedWrites(buffered);
  device.DeviceName = "Board";
  device.DeviceHWVersion = "Mega";

  float boardValue = 1, flow = 2, pressure = 3, orphan = 4;
  byte speed = 0;
  bool pumpOn = false;
  device.addSignal(F("BoardValue"), &boardValue);
  BlaeckDeviceRef pump = device.addDevice(F("Pump")).withHWVersion("Nano").withFWVersion(F("1.2"));
  BlaeckDeviceRef fan = device.addDevice("Fan");
  assert(!pump.isMissing() && !fan.isMissing());

  // A duplicate name, a full table and an empty name are rejected; their handles do nothing.
  debug.text.clear();
  BlaeckDeviceRef duplicate = device.addDevice(F("Pump"));
  assert(debug.text.find("duplicate device name") != std::string::npos);
  BlaeckDeviceRef third = device.addDevice(F("Third"));
  assert(debug.text.find("Dropped 'Third': table full at 2") != std::string::npos);
  BlaeckDeviceRef empty = device.addDevice("");
  BlaeckDeviceRef unset;
  third.markMissing();
  empty.writeRestarted();
  assert(!third.isMissing() && !unset.isMissing() && !duplicate.isMissing());
  debug.text.clear();
  assert(device.printRejections(&debug));
  assert(debug.text.find("3 device registrations rejected; table capacity: 2.") != std::string::npos);

  pump.addSignal(F("Flow"), &flow);
  pump.addSignal(F("Pressure"), &pressure);
  // A rejected or unset handle registers nothing, and counts nothing as rejected.
  third.addSignal(F("Lost"), &orphan).withUnit(F("V"));
  unset.addSignal("Lost", &orphan);
  unset.onCommand("LOST", handler);
  unset.addStateChannel(F("Lost"), BlaeckText);
  unset.addEventChannel(F("Lost"), F("x"));
  unset.write("Lost", 1.0f);
  unset.writeState(F("Lost"), "x");
  unset.writeEvent(F("Lost"), F("x"));
  assert(device.SignalCount == 3 && unset.findSignalIndex("Lost") == -1);
  assert(!device.hasRejectedSignals() && !device.hasRejectedCommands());
  device.addSignal(F("Orphan"), &orphan);

  // A command registered through a device's handle takes its own state channel along.
  pump.onNumberCommand("PUMP_SPEED", handler).withRange(0.0f, 100.0f, 1.0f)
      .withOwnState(F("PumpSpeedState"), &speed);
  pump.onSwitchCommand("PUMP_ON", handler).withOwnState(F("PumpOnState"), &pumpOn);
  device.onButtonCommand("BOARD_RESET", handler);
  pump.addStateChannel(F("PumpStatus"), BlaeckText);
  fan.addEventChannel(F("FanAlarm"), F("stall"));

  // The board's restart notice is a C1 for device 0.
  device.read();
  assert(commandFramePayload(stream.data.output, 0xC1, 0) == notice(0, 0x01));
  stream.data.output.clear();

  command(device, stream, "<BLAECK.GET_DEVICES>");
  assert(commandFramePayload(stream.data.output, 0xB7, 0) ==
         deviceList(3, deviceRecord(0, 0, "Board", "Mega", "n/a") +
                       deviceRecord(1, 0, "Pump", "Nano", "1.2") +
                       deviceRecord(2, 0, "Fan", "n/a", "n/a")));
  stream.data.output.clear();

  command(device, stream, "<BLAECK.WRITE_SYMBOLS>");
  std::string payload = commandFramePayload(stream.data.output, 0xB0, 0);
  assert(ownerOf(payload, "BoardValue") == owner(0x01, 0));
  assert(ownerOf(payload, "Flow") == owner(0x02, 1));
  assert(ownerOf(payload, "Pressure") == owner(0x02, 1));
  assert(ownerOf(payload, "Orphan") == owner(0x01, 0));
  stream.data.output.clear();

  command(device, stream, "<BLAECK.WRITE_COMMANDS>");
  payload = commandFramePayload(stream.data.output, 0xA0, 0);
  assert(ownerOf(payload, "PUMP_SPEED", 2) == owner(0x02, 1));
  assert(ownerOf(payload, "PUMP_ON", 2) == owner(0x02, 1));
  assert(ownerOf(payload, "BOARD_RESET", 2) == owner(0x01, 0));
  stream.data.output.clear();

  command(device, stream, "<BLAECK.WRITE_STATE_CHANNELS>");
  payload = commandFramePayload(stream.data.output, 0x90, 0);
  assert(ownerOf(payload, "PumpStatus") == owner(0x02, 1));
  assert(ownerOf(payload, "PumpSpeedState") == owner(0x02, 1));
  assert(ownerOf(payload, "PumpOnState") == owner(0x02, 1));
  stream.data.output.clear();

  command(device, stream, "<BLAECK.WRITE_EVENT_CHANNELS>");
  assert(ownerOf(commandFramePayload(stream.data.output, 0x80, 0), "FanAlarm") == owner(0x02, 2));
  stream.data.output.clear();

  // Names are found within the handle's own device only.
  device.writeState(F("PumpStatus"), "ok");
  device.writeEvent(F("FanAlarm"), F("stall"));
  assert(stream.data.output.empty());
  pump.writeState(F("PumpStatus"), "ok");
  assert(commandFramePayload(stream.data.output, 0x95, 0).substr(0, 2) == owner(0x02, 1));
  stream.data.output.clear();
  fan.writeEvent(F("FanAlarm"), F("stall"));
  assert(commandFramePayload(stream.data.output, 0x85, 0).substr(0, 2) == owner(0x02, 2));
  stream.data.output.clear();

  // A device restart is reported for that device only.
  pump.writeRestarted();
  assert(commandFramePayload(stream.data.output, 0xC1, 0) == notice(1, 0x01));
  stream.data.output.clear();

  const std::vector<int> widths = {4, 4, 4, 4};
  command(device, stream, "<BLAECK.ACTIVATE,0>");
  stream.data.output.clear();
  device.writeIfDue();
  auto frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{0, 1, 2, 3}));
  assert(frames[0].status == 0 && frames[0].statusPayload == std::string(4, '\0'));

  // Going missing is a C1 at once, sent only on a real change; the device's signals leave the
  // data frames, whose status stays normal.
  pump.markMissing();
  assert(commandFramePayload(stream.data.output, 0xC1, 0) == notice(1, 0x02));
  stream.data.output.clear();
  pump.markMissing();
  assert(stream.data.output.empty());
  assert(pump.isMissing());
  device.writeIfDue();
  frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{0, 3}));
  assert(frames[0].status == 0 && frames[0].statusPayload == std::string(4, '\0'));
  device.writeAll();
  frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{0, 3}) && frames[0].status == 0);
  pump.writeAll();
  assert(takeData(stream.data.output, widths).empty());

  // write() to a missing device's signal is dropped, noted on the debug stream once.
  debug.text.clear();
  pump.write("Flow", 5.0f);
  pump.write("Flow", 6.0f);
  assert(takeData(stream.data.output, widths).empty());
  const std::string note = "write() dropped for 'Flow': 'Pump' is marked missing (once until markPresent()).";
  assert(debug.text.find(note) != std::string::npos);
  assert(debug.text.find(note) == debug.text.rfind(note));
  assert(!device.hasRejectedSignals()); // a dropped write is no rejection

  // The list reports the state too.
  command(device, stream, "<BLAECK.GET_DEVICES>");
  assert(commandFramePayload(stream.data.output, 0xB7, 0).find(deviceRecord(1, 0x01, "Pump", "Nano", "1.2")) !=
         std::string::npos);
  stream.data.output.clear();

  // Back again: a C1, and the note may come once more in the next missing phase.
  pump.markPresent();
  assert(commandFramePayload(stream.data.output, 0xC1, 0) == notice(1, 0x03));
  stream.data.output.clear();
  pump.markMissing();
  stream.data.output.clear();
  debug.text.clear();
  pump.write("Flow", 7.0f);
  assert(debug.text.find(note) != std::string::npos);
  pump.markPresent();
  stream.data.output.clear();

  // pump.writeAll() sends the pump's signals only.
  pump.writeAll();
  frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{1, 2}));

  // A device without signals changes nothing in the data when it goes missing.
  fan.markMissing();
  assert(commandFramePayload(stream.data.output, 0xC1, 0) == notice(2, 0x02));
  stream.data.output.clear();
  device.writeIfDue();
  frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{0, 1, 2, 3}) && frames[0].status == 0);
  fan.markPresent();
  stream.data.output.clear();

  // After a gap, a changed-only signal is sent again even if its value did not change.
  command(device, stream, "<BLAECK.DEACTIVATE>");
  stream.data.output.clear();
  Blaeck changes;
  FakeStream changesStream;
  changes.begin(changesStream).withDevices(1);
  float level = 7;
  BlaeckDeviceRef tank = changes.addDevice(F("Tank"));
  tank.addSignal(F("Level"), &level).writeAtInterval(BLAECK_OFF).writeOnChange(BLAECK_ANY_CHANGE);
  changes.read();
  changesStream.data.output.clear();
  changes.writeIfDue();
  assert(takeData(changesStream.data.output, {4}).size() == 1);
  changes.writeIfDue();
  assert(takeData(changesStream.data.output, {4}).empty());
  tank.markMissing();
  tank.markPresent();
  changes.writeIfDue();
  frames = takeData(changesStream.data.output, {4});
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{0}));

  // The device table holds at most 254, so the list's count byte covers them and the board.
  Blaeck big;
  FakeStream bigStream;
  debug.text.clear();
  big.begin(bigStream).withDebugStream(&debug).withDevices(300);
  assert(debug.text.find("withDevices(300): clamped to 254") != std::string::npos);
  char name[8];
  for (int i = 0; i < 255; ++i)
  {
    snprintf(name, sizeof(name), "D%d", i);
    big.addDevice(name);
  }
  assert(big.hasRejections()); // the 255th
  big.read();
  bigStream.data.output.clear();
  command(big, bigStream, "<BLAECK.GET_DEVICES>");
  const std::string list = commandFramePayload(bigStream.data.output, 0xB7, 0);
  assert(static_cast<byte>(list[std::string("blaeck").size() + 1 + std::string(BLAECK_VERSION).size() + 1]) == 255);
  assert(list.find(deviceRecord(254, 0, "D253", "n/a", "n/a")) != std::string::npos);
}

// Over TCP a host can receive frames only once it sent a BLAECK. command, normally
// GET_DEVICES. Notices held back until then are answered by the list itself.
static void deviceNoticesBeforeHost()
{
  FakeServer<> server;
  SocketState host;
  Blaeck device;
  device.begin(server).withDevices(2);
  device.DeviceName = "Board";
  device.DeviceHWVersion = "Mega";
  BlaeckDeviceRef pump = device.addDevice(F("Pump"));
  BlaeckDeviceRef fan = device.addDevice(F("Fan"));
  pump.markMissing();
  pump.writeRestarted();
  fan.markMissing();
  fan.markPresent(); // back before anyone heard: no change to report
  device.read();
  server.pending = {&host};
  device.read();
  assert(host.output.empty());

  host.input = "<BLAECK.GET_DEVICES>";
  device.read();
  assert(host.output.find(std::string("<BLAECK:") + char(0xC1)) == std::string::npos);
  assert(commandFramePayload(host.output, 0xB7, 0) ==
         deviceList(3, deviceRecord(0, 0x02, "Board", "Mega", "n/a") +
                       deviceRecord(1, 0x03, "Pump", "n/a", "n/a") +
                       deviceRecord(2, 0, "Fan", "n/a", "n/a")));
  host.output.clear();

  // Everything was reported: no C1 follows, and the next list shows no restart.
  device.read();
  assert(host.output.empty());
  host.input = "<BLAECK.GET_DEVICES>";
  device.read();
  assert(commandFramePayload(host.output, 0xB7, 0) ==
         deviceList(3, deviceRecord(0, 0, "Board", "Mega", "n/a") +
                       deviceRecord(1, 0x01, "Pump", "n/a", "n/a") +
                       deviceRecord(2, 0, "Fan", "n/a", "n/a")));
}

// CRC16-CCITT (init 0, poly 0x1021), as a host computes the schema hash.
static uint16_t crc16(const std::string &data)
{
  uint16_t crc = 0;
  for (unsigned char b : data)
  {
    crc ^= static_cast<uint16_t>(b << 8);
    for (int i = 0; i < 8; ++i)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

// Owner bytes of every catalog entry with this name, in catalog order.
static std::vector<std::string> ownersOf(const std::string &payload, const char *name, size_t gap = 0)
{
  std::vector<std::string> owners;
  const std::string key = std::string(name) + '\0';
  for (size_t at = payload.find(key); at != std::string::npos; at = payload.find(key, at + 1))
  {
    assert(at >= 2 + gap);
    owners.push_back(payload.substr(at - 2 - gap, 2));
  }
  return owners;
}

static void sameNamesAcrossDevices()
{
  hostMillis() = 0;
  auto handler = [](const char *, const char *const *, byte) {};
  FakeStream stream;
  Capture debug;
  Blaeck device;
  device.begin(stream).withDevices(2).withDebugStream(&debug);
  BlaeckDeviceRef zoneA = device.addDevice(F("Zone A"));
  BlaeckDeviceRef zoneB = device.addDevice("Zone B");

  float boardTemp = 1, aTemp = 2, bTemp = 3;
  device.addSignal("Temperature", &boardTemp);
  zoneA.addSignal(F("Temperature"), &aTemp);
  zoneB.addSignal("Temperature", &bTemp);
  assert(device.SignalCount == 3);
  assert(device.findSignalIndex("Temperature") == 0 && zoneA.findSignalIndex(F("Temperature")) == 1 &&
         zoneB.findSignalIndex("Temperature") == 2);

  // The same state channel name on each; declaring it again within one device reuses its slot.
  device.addStateChannel(F("Status"), BlaeckText);
  zoneA.addStateChannel("Status", BlaeckText);
  zoneA.addStateChannel(F("Status"), BlaeckText).withIcon(F("mdi:pump"));

  // Event channels and their types are per device, too.
  device.addEventChannel(F("Alarm"), F("overheated"));
  zoneA.addEventChannel("Alarm", F("dry_run"));
  assert(zoneA.addEventType(F("Alarm"), F("blocked")));

  // Command names stay unique per board, but their own state channels are per device.
  byte aSpeed = 10, bSpeed = 20;
  zoneA.onNumberCommand("SET_A_SPEED", handler).withRange(0.0f, 100.0f, 1.0f).withOwnState(F("Speed"), &aSpeed);
  zoneB.onNumberCommand("SET_B_SPEED", handler).withRange(0.0f, 100.0f, 1.0f).withOwnState(F("Speed"), &bSpeed);
  assert(!device.hasRejections());
  assert(!zoneB.addEventType(F("Alarm"), F("blocked"))); // zone B has no Alarm

  device.read();
  stream.data.output.clear();

  command(device, stream, "<BLAECK.WRITE_SYMBOLS>");
  std::string payload = commandFramePayload(stream.data.output, 0xB0, 0);
  assert((ownersOf(payload, "Temperature") == std::vector<std::string>{owner(0x01, 0), owner(0x02, 1), owner(0x02, 2)}));
  stream.data.output.clear();

#if BLAECK_ENABLE_STATE_CHANNELS
  command(device, stream, "<BLAECK.WRITE_STATE_CHANNELS>");
  payload = commandFramePayload(stream.data.output, 0x90, 0);
  assert((ownersOf(payload, "Status") == std::vector<std::string>{owner(0x01, 0), owner(0x02, 1)}));
  assert((ownersOf(payload, "Speed") == std::vector<std::string>{owner(0x02, 1), owner(0x02, 2)}));
  stream.data.output.clear();

  // Each handle reaches its own channel: the 95 frame carries owner and channel index.
  device.writeState(F("Status"), "board");
  std::string state = commandFramePayload(stream.data.output, 0x95, 0);
  assert(state.substr(0, 2) == owner(0x01, 0) && state.find("board") != std::string::npos);
  stream.data.output.clear();
  zoneA.writeState("Status", "zone a");
  state = commandFramePayload(stream.data.output, 0x95, 0);
  assert(state.substr(0, 2) == owner(0x02, 1) && state.find("zone a") != std::string::npos);
  stream.data.output.clear();
  zoneB.writeState(F("Status"), "none"); // zone B has no Status
  assert(stream.data.output.empty());

  // A command's own state is the channel of its own device.
  zoneB.writeCommandState("SET_B_SPEED");
  const std::string speed = commandFramePayload(stream.data.output, 0x95, 0);
  // Channels in order: Status (board), Status (zone A), Speed (zone A), Speed (zone B).
  assert(speed.substr(0, 2) == owner(0x02, 2));
  assert(speed[2] == 3 && speed[3] == 0 && static_cast<byte>(speed.back()) == 20);
  stream.data.output.clear();
#endif

#if BLAECK_ENABLE_EVENTS
  zoneA.writeEvent(F("Alarm"), F("blocked"));
  assert(commandFramePayload(stream.data.output, 0x85, 0).substr(0, 2) == owner(0x02, 1));
  stream.data.output.clear();
  debug.text.clear();
  device.writeEvent("Alarm", F("dry_run")); // a type of zone A's Alarm, not the board's
  assert(stream.data.output.empty());
  assert(debug.text.find("type not declared") != std::string::npos);
#endif

  // Explicit writes by name go to the handle's signal.
  const std::vector<int> widths = {4, 4, 4};
  zoneB.write("Temperature", 30.0f);
  auto frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{2}));
  device.write(F("Temperature"), 10.0f);
  frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{0}));

  // The schema hash covers a device's signal as "<device name>/<signal name>".
  const char f = static_cast<char>(0x08); // float
  assert(frames[0].schemaHash ==
         crc16(std::string("Temperature") + f + "Zone A/Temperature" + f + "Zone B/Temperature" + f));

  const uint16_t hash = frames[0].schemaHash;

  // Firmware that registers zone B's signal before zone A's sends the same names in another
  // order. Without the device names the hash would match, and a host would file zone A's
  // values under zone B.
  FakeStream swappedStream;
  Blaeck swapped;
  swapped.begin(swappedStream).withDevices(2);
  BlaeckDeviceRef swappedA = swapped.addDevice(F("Zone A"));
  BlaeckDeviceRef swappedB = swapped.addDevice("Zone B");
  swapped.addSignal("Temperature", &boardTemp);
  swappedB.addSignal("Temperature", &bTemp);
  swappedA.addSignal("Temperature", &aTemp);
  swapped.read();
  swappedStream.data.output.clear();
  swapped.writeAll();
  frames = takeData(swappedStream.data.output, widths);
  assert(frames.size() == 1 && frames[0].schemaHash != hash);
  assert(frames[0].schemaHash ==
         crc16(std::string("Temperature") + f + "Zone B/Temperature" + f + "Zone A/Temperature" + f));

  // A board without devices hashes exactly as before: names and type codes only.
  FakeStream plainStream;
  Blaeck plain;
  plain.begin(plainStream);
  plain.addSignal("Temperature", &boardTemp);
  plain.read();
  plainStream.data.output.clear();
  plain.writeAll();
  frames = takeData(plainStream.data.output, {4});
  assert(frames.size() == 1 && frames[0].schemaHash == crc16(std::string("Temperature") + f));
}

static void ackResult(const std::string &output, uint32_t messageId, const char *bare, byte status)
{
  const std::string ack = commandFramePayload(output, 0xA5, messageId);
  uint32_t hash, nameHash;
  memcpy(&hash, ack.data(), 4);
  memcpy(&nameHash, ack.data() + 4, 4);
  const std::string name = std::string(bare).substr(0, std::string(bare).find(','));
  assert(hash == commandHash(bare) && nameHash == commandHash(name));
  assert(static_cast<byte>(ack[8]) == status);
}

static void deviceCommands()
{
  pings.clear();
  FakeStream stream;
  Blaeck device;
  device.begin(stream).withDevices(2);
  BlaeckDeviceRef pump = device.addDevice(F("Pump"));
  device.addDevice(F("Fan"));
  pump.onNumberCommand("SET_PUMP_SPEED", onPing).withRange(0.0f, 100.0f, 1.0f);
  device.onCommand("BOARD_PING", onPing);
  device.read();
  stream.data.output.clear();

  // A device's command is sent like the board's: its name alone finds it.
  command(device, stream, "<#7:SET_PUMP_SPEED,40>");
  assert((pings == std::vector<std::string>{"40"}));
  ackResult(stream.data.output, 7, "SET_PUMP_SPEED,40", 0);
  stream.data.output.clear();

  // '@' is no prefix: the command is unknown and nothing runs.
  command(device, stream, "<@1:#8:SET_PUMP_SPEED,41>");
  assert(pings.size() == 1);
  const std::string ack = commandFramePayload(stream.data.output, 0xA5, 0);
  assert(static_cast<byte>(ack[8]) == 1 && static_cast<byte>(ack[9]) == BLAECK_ACK_UNKNOWN);
  stream.data.output.clear();

  // While the pump is missing its command is refused and the handler doesn't run.
  pump.markMissing();
  stream.data.output.clear();
  command(device, stream, "<#9:SET_PUMP_SPEED,42>");
  assert(pings.size() == 1);
  const std::string refused = commandFramePayload(stream.data.output, 0xA5, 9);
  assert(static_cast<byte>(refused[8]) == 1 && static_cast<byte>(refused[9]) == BLAECK_ACK_DEVICE_NOT_RESPONDING);
  stream.data.output.clear();
  command(device, stream, "<#10:BOARD_PING,1>"); // the board's own commands still run
  ackResult(stream.data.output, 10, "BOARD_PING,1", 0);
  stream.data.output.clear();
  pump.markPresent();
  stream.data.output.clear();
  command(device, stream, "<SET_PUMP_SPEED,43>");
  assert(pings.back() == "43");
}

static void reportingPolicies(bool buffered)
{
  hostMillis() = 0;
  FakeStream stream;
  Blaeck device;
  device.begin(stream).withSignals(4);
  device.setBufferedWrites(buffered);
  float periodic = 0, filtered = 20, change = 20, combined = 20;
  device.addSignal(F("Periodic"), &periodic);
  device.addSignal(F("Filtered"), &filtered).writeAtInterval(BLAECK_ON_CHANGE, 0.5);
  device.addSignal(F("Change"), &change).writeAtInterval(BLAECK_OFF).writeOnChange(1);
  device.addSignal(F("Combined"), &combined).writeOnChange(1);
  const std::vector<int> widths(4, 4);
  device.tick();
  expectData(stream, widths, {2, 3}); // no activation; first values bypass rate limit
  device.tick();
  expectData(stream, widths, {});
  command(device, stream, "<BLAECK.ACTIVATE,1000>");
  device.writeIfDue();
  expectData(stream, widths, {0, 1, 3});
  hostMillis() = 99;
  change = combined = 21;
  device.writeIfDue();
  expectData(stream, widths, {});
  hostMillis() = 100;
  device.writeIfDue();
  expectData(stream, widths, {2, 3});
  filtered = 20.25f;
  hostMillis() = 1000;
  device.writeIfDue();
  expectData(stream, widths, {0, 3});
  filtered = 20.5f; // accumulated drift and exact threshold equality
  change = combined = 22;
  hostMillis() = 2000;
  device.writeIfDue();
  expectData(stream, widths, {0, 1, 2, 3}); // one frame, no duplicate combined signal
  filtered = 22;
  filtered = 20.5f; // excursion between snapshots is not remembered
  command(device, stream, "<BLAECK.DEACTIVATE>");
  change = 23;
  hostMillis() = 2100;
  device.writeIfDue();
  expectData(stream, widths, {2});
  command(device, stream, "<BLAECK.PAUSE_WRITES,FOREVER>");
  stream.data.output.clear();
  change = 24;
  hostMillis() = 4000;
  device.tick();
  expectData(stream, widths, {});
  command(device, stream, "<BLAECK.RESUME_WRITES>");
  device.writeIfDue();
  expectData(stream, widths, {2});
  command(device, stream, "<BLAECK.WRITE_DATA>");
  expectData(stream, widths, {0, 1, 2, 3});
  change = 25;
  hostMillis() = 4099;
  device.writeIfDue();
  expectData(stream, widths, {});
  hostMillis() = 4100;
  device.writeIfDue();
  expectData(stream, widths, {2});
}

static void reportingToggle(bool buffered)
{
  for (BlaeckIntervalMode mode : {BLAECK_OFF, BLAECK_ALWAYS, BLAECK_ON_CHANGE})
  {
    hostMillis() = 0;
    FakeStream stream;
    ReportingProbe device;
    device.begin(stream).withSignals(3);
    device.setBufferedWrites(buffered);
    float value = 0;
    bool flag = false;
    char text[] = "a";
    auto number = device.addSignal(F("Number"), &value);
    auto boolean = device.addSignal(F("Bool"), &flag);
    auto string = device.addSignal(F("Text"), text);
    const std::vector<int> widths{4, 1, -1};
    size_t before = allocations;
    number.writeOnChange(BLAECK_OFF).writeOnChange(BLAECK_OFF);
    boolean.writeOnChange(BLAECK_OFF);
    string.writeOnChange(BLAECK_OFF);
    assert(allocations == before);
    for (int i = 0; i < 3; ++i)
      assert(device.reporting(i) == nullptr);

    number.writeAtInterval(mode, 0.5).writeOnChange(0);
    boolean.writeAtInterval(mode, BLAECK_ANY_CHANGE).writeOnChange(BLAECK_ANY_CHANGE);
    string.writeAtInterval(mode, BLAECK_ANY_CHANGE).writeOnChange(BLAECK_ANY_CHANGE);
    command(device, stream, "<BLAECK.ACTIVATE,1000>");
    device.writeIfDue();
    expectData(stream, widths, {0, 1, 2});
    assert(device.reporting(2)->text != nullptr);

    before = allocations;
    const SignalReporting *retained = device.reporting(0);
    const char *retainedText = device.reporting(2)->text;
    failAfter = 0;
    number.writeOnChange(BLAECK_OFF);
    boolean.writeOnChange(BLAECK_OFF);
    string.writeOnChange(BLAECK_OFF).writeOnChange(BLAECK_OFF);
    failAfter = -1;
    assert(allocations == before && !device.hasRejections());
    for (int i = 0; i < 3; ++i)
    {
      const SignalReporting *r = device.reporting(i);
      if (mode == BLAECK_ON_CHANGE)
        assert(r != nullptr && !r->immediate && r->valid);
      else
        assert(r == nullptr);
    }
    if (mode == BLAECK_ON_CHANGE)
    {
      assert(device.reporting(0) == retained && retained->intervalDelta == 0.5);
      assert(device.reporting(2)->text == retainedText);
    }

    value = 0.25f;
    flag = true;
    text[0] = 'b';
    hostMillis() = 100;
    device.writeIfDue();
    expectData(stream, widths, {});
    hostMillis() = 1000;
    device.writeIfDue();
    if (mode == BLAECK_ALWAYS)
      expectData(stream, widths, {0, 1, 2});
    else if (mode == BLAECK_ON_CHANGE)
      expectData(stream, widths, {1, 2});
    else
      expectData(stream, widths, {});
    value = 0.5f;
    hostMillis() = 2000;
    device.writeIfDue();
    if (mode == BLAECK_ALWAYS)
      expectData(stream, widths, {0, 1, 2});
    else if (mode == BLAECK_ON_CHANGE)
      expectData(stream, widths, {0});
    else
      expectData(stream, widths, {});

    command(device, stream, "<BLAECK.DEACTIVATE>");
    device.write("Number", 3.0f);
    expectData(stream, widths, {0});
    device.writeAll();
    expectData(stream, widths, {0, 1, 2});
    number.writeOnChange(0.1, 250);
    boolean.writeOnChange(BLAECK_ANY_CHANGE, 250);
    string.writeOnChange(BLAECK_ANY_CHANGE, 250);
    device.writeIfDue();
    if (mode == BLAECK_ON_CHANGE)
      expectData(stream, widths, {});
    else
      expectData(stream, widths, {0, 1, 2});
    value = 4;
    flag = false;
    text[0] = 'c';
    hostMillis() = 2249;
    device.writeIfDue();
    expectData(stream, widths, {});
    hostMillis() = 2250;
    device.writeIfDue();
    expectData(stream, widths, {0, 1, 2});

    number.writeOnChange(BLAECK_OFF).writeAtInterval(BLAECK_ALWAYS);
    boolean.writeAtInterval(BLAECK_ALWAYS).writeOnChange(BLAECK_OFF);
    string.writeOnChange(BLAECK_OFF).writeAtInterval(BLAECK_ALWAYS);
    for (int i = 0; i < 3; ++i)
      assert(device.reporting(i) == nullptr);
    assert(!device.hasRejections());
  }
}

static void reportingToggleFailures()
{
  hostMillis() = 0;
  FakeStream stream;
  Capture debug;
  ReportingProbe device;
  device.begin(stream).withSignals(1).withDebugStream(&debug);
  float value = 0;
  auto signal = device.addSignal(F("Value"), &value);
  signal.writeAtInterval(BLAECK_OFF).writeOnChange(BLAECK_ANY_CHANGE, 0);
  device.writeIfDue();
  expectData(stream, {4}, {0});
  for (BlaeckIntervalMode mode : {BLAECK_ALWAYS, BLAECK_ON_CHANGE,
                                 static_cast<BlaeckIntervalMode>(255)})
  {
    debug.text.clear();
    signal.writeOnChange(mode);
    assert(debug.text.find("Invalid change reporting mode") != std::string::npos);
    assert(debug.text.find("previous policy retained") != std::string::npos);
    value += 1;
    device.writeIfDue();
    expectData(stream, {4}, {0});
  }
  signal.writeOnChange(BLAECK_OFF);
  assert(device.reporting(0) == nullptr);
  debug.text.clear();
  failAfter = 0;
  signal.writeOnChange(BLAECK_ANY_CHANGE);
  failAfter = -1;
  assert(device.reporting(0) == nullptr && debug.text.find("No RAM") != std::string::npos);
  value += 1;
  device.writeIfDue();
  expectData(stream, {4}, {});
  signal.writeOnChange(0, 0);
  device.writeIfDue();
  expectData(stream, {4}, {0});

  signal.writeAtInterval(BLAECK_ON_CHANGE, BLAECK_ANY_CHANGE).writeOnChange(BLAECK_OFF);
  const size_t before = allocations;
  failAfter = 0;
  signal.writeOnChange(BLAECK_ANY_CHANGE, 0);
  failAfter = -1;
  assert(allocations == before); // Interval filtering retained the tracking allocation.
  device.writeIfDue();
  expectData(stream, {4}, {});
  value += 0.25f;
  device.writeIfDue();
  expectData(stream, {4}, {0});

  auto rejected = device.addSignal(F("Overflow"), &value);
  debug.text.clear();
  rejected.writeOnChange(BLAECK_OFF).writeOnChange(BLAECK_ANY_CHANGE)
      .writeOnChange(BLAECK_ALWAYS);
  assert(debug.text.empty()); // Invalid handles remain inert for both overloads.
}

static void reportingActivationSnapshot(bool buffered)
{
  hostMillis() = 0;
  FakeStream stream;
  Blaeck device;
  device.begin(stream).withSignals(7);
  device.setBufferedWrites(buffered);
  float periodic = 0, filtered = 0, change = 0, combined = 0, explicitValue = 0;
  bool flag = false;
  char text[] = "same";
  device.addSignal(F("Interval"), &periodic);
  device.addSignal(F("IntervalOnChange"), &filtered).writeAtInterval(BLAECK_ON_CHANGE, 0.5);
  device.addSignal(F("OnChange"), &change).writeAtInterval(BLAECK_OFF).writeOnChange(1);
  device.addSignal(F("Combined"), &combined)
      .writeAtInterval(BLAECK_ON_CHANGE, 0.5).writeOnChange(1);
  device.addSignal(F("Explicit"), &explicitValue).writeAtInterval(BLAECK_OFF);
  device.addSignal(F("Bool"), &flag).writeAtInterval(BLAECK_ON_CHANGE);
  device.addSignal(F("Text"), text).writeAtInterval(BLAECK_ON_CHANGE);
  const std::vector<int> widths = {4, 4, 4, 4, 4, 1, -1};
  device.writeAll();
  expectData(stream, widths, {0, 1, 2, 3, 4, 5, 6});

  const auto expectInitial = [&]()
  {
    const auto frames = takeData(stream.data.output, widths);
    assert(frames.size() == 1);
    const auto &frame = frames[0];
    assert(frame.ids == std::vector<int>({0, 1, 3, 5, 6}));
    assert(frame.flags == 0x04);
    assert(frame.values[1] == std::string(reinterpret_cast<const char *>(&filtered), 4));
    assert(frame.values[2] == std::string(reinterpret_cast<const char *>(&combined), 4));
    assert(frame.values[3] == std::string(1, '\0') && frame.values[4] == "same");
  };
  filtered = combined = 0.25f;
  hostMillis() = 10;
  command(device, stream, "<BLAECK.ACTIVATE,1000>");
  device.writeIfDue();
  expectInitial(); // bypass thresholds and the combined signal's rate limit
  device.writeIfDue();
  expectData(stream, widths, {});

  change = 1;
  combined = 1.25f;
  hostMillis() = 99;
  device.writeIfDue();
  expectData(stream, widths, {});
  hostMillis() = 100;
  device.writeIfDue();
  expectData(stream, widths, {2}); // activation did not reset the independent clock
  hostMillis() = 109;
  device.writeIfDue();
  expectData(stream, widths, {});
  hostMillis() = 110;
  device.writeIfDue();
  expectData(stream, widths, {3}); // initial report updated the shared baseline and clock

  hostMillis() = 1010;
  device.writeIfDue();
  expectData(stream, widths, {0});
  filtered = 0.75f;
  hostMillis() = 2010;
  device.writeIfDue();
  expectData(stream, widths, {0, 1}); // normal interval filtering uses the initial value

  command(device, stream, "<BLAECK.DEACTIVATE>");
  hostMillis() = 2050;
  command(device, stream, "<BLAECK.ACTIVATE,1000>");
  device.writeIfDue();
  expectInitial();
  hostMillis() = 2060;
  command(device, stream, "<BLAECK.ACTIVATE,500>");
  device.writeIfDue();
  expectInitial(); // changing an active interval also establishes initial values
  hostMillis() = 2560;
  device.writeIfDue();
  expectData(stream, widths, {0});
  command(device, stream, "<BLAECK.ACTIVATE,0>");
  device.writeIfDue();
  expectInitial();
  device.writeIfDue();
  expectData(stream, widths, {0}); // zero interval does not keep forcing filtered signals

  command(device, stream, "<BLAECK.PAUSE_WRITES,FOREVER>");
  command(device, stream, "<BLAECK.ACTIVATE,500>");
  device.writeIfDue();
  expectData(stream, widths, {});
  hostMillis() = 3000;
  device.writeIfDue();
  expectData(stream, widths, {});
  command(device, stream, "<BLAECK.RESUME_WRITES>");
  device.writeIfDue();
  expectInitial(); // a paused pass must not consume the initial report
  device.writeIfDue();
  expectData(stream, widths, {});
}

static void sharedBaselineAndClock()
{
  hostMillis() = 0;
  FakeStream stream;
  Blaeck device;
  device.begin(stream);
  float value = 10;
  device.addSignal(F("V"), &value).writeAtInterval(BLAECK_ON_CHANGE, 0.5).writeOnChange(1);
  command(device, stream, "<BLAECK.ACTIVATE,1000>");
  device.writeIfDue();
  expectData(stream, {4}, {0});
  value = 10.5f;
  hostMillis() = 999;
  device.writeIfDue();
  expectData(stream, {4}, {});
  hostMillis() = 1000;
  device.writeIfDue();
  expectData(stream, {4}, {0});
  hostMillis() = 1950;
  device.write("V", 20.0f);
  expectData(stream, {4}, {0});
  value = 20.5f;
  hostMillis() = 2000;
  device.writeIfDue();
  expectData(stream, {4}, {0}); // direct write did not shift host cadence
  value = 21.5f;
  hostMillis() = 2099;
  device.writeIfDue();
  expectData(stream, {4}, {});
  hostMillis() = 2100;
  device.writeIfDue();
  expectData(stream, {4}, {0});
  hostMillis() = 3000;
  device.writeIfDue();
  expectData(stream, {4}, {}); // immediate report is also the interval baseline

  // Millisecond subtraction crosses rollover; ordinary interval cadence does too.
  hostMillis() = UINT32_MAX - 50;
  device.write("V", 30.0f);
  expectData(stream, {4}, {0});
  command(device, stream, "<BLAECK.ACTIVATE,100>");
  device.writeIfDue();
  expectData(stream, {4}, {0});
  value = 32;
  hostMillis() = 48;
  device.writeIfDue();
  expectData(stream, {4}, {});
  hostMillis() = 49;
  device.writeIfDue();
  expectData(stream, {4}, {0});
}

static void reportingTypesAndFailures(bool buffered)
{
  hostMillis() = 0;
  FakeStream stream;
  Capture debug;
  Blaeck device;
  device.begin(stream).withSignals(5).withDebugStream(&debug);
  device.setBufferedWrites(buffered);
  char text[300] = "";
  bool flag = false;
  float floating = 0;
  long signedValue = INT32_MIN;
  unsigned long unsignedValue = UINT32_MAX;
  device.addSignal(F("Text"), text).writeAtInterval(BLAECK_OFF).writeOnChange(999, 0);
  device.addSignal(F("Flag"), &flag).writeAtInterval(BLAECK_OFF).writeOnChange(999, 0);
  auto number = device.addSignal(F("Float"), &floating);
  number.writeAtInterval(BLAECK_OFF).writeOnChange(0, 0);
  device.addSignal(F("Signed"), &signedValue).writeAtInterval(BLAECK_OFF).writeOnChange(1, 0);
  device.addSignal(F("Unsigned"), &unsignedValue).writeAtInterval(BLAECK_OFF).writeOnChange(1, 0);
  const std::vector<int> widths{-1, 1, 4, 4, 4};
  device.writeIfDue();
  expectData(stream, widths, {0, 1, 2, 3, 4});
  device.writeIfDue();
  expectData(stream, widths, {});
  strcpy(text, "running");
  flag = true;
  signedValue = INT32_MAX;
  --unsignedValue;
  device.writeIfDue();
  expectData(stream, widths, {0, 1, 3, 4});
  floating = NAN;
  device.writeIfDue();
  expectData(stream, widths, {2});
  device.writeIfDue();
  expectData(stream, widths, {});
  floating = INFINITY;
  device.writeIfDue();
  expectData(stream, widths, {2});
  device.writeIfDue();
  expectData(stream, widths, {});
  floating = -INFINITY;
  device.writeIfDue();
  expectData(stream, widths, {2});
  floating = 0;
  device.writeIfDue();
  expectData(stream, widths, {2});
  number.writeOnChange(-1);
  number.writeAtInterval(BLAECK_ON_CHANGE, NAN);
  assert(device.hasRejections());
  assert(debug.text.find("previous policy retained") != std::string::npos);
  floating = 1;
  device.writeIfDue();
  expectData(stream, widths, {2});

  memset(text, 'a', 299);
  text[299] = '\0';
  device.writeIfDue();
  const auto frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && frames[0].values[0].size() == 255);
  text[280] = 'b';
  device.writeIfDue();
  expectData(stream, widths, {}); // only transmitted text is compared
  text[254] = 'b';
  device.writeIfDue();
  expectData(stream, widths, {0});

  floating = 2;
  stream.data.writeLimit = 0;
  device.writeIfDue();
  assert(debug.text.find("Incomplete transport write") != std::string::npos);
  stream.data.writeLimit = SIZE_MAX;
  device.writeIfDue();
  expectData(stream, widths, {2}); // failed frame must not suppress retry
  device.clearAllSignals();
  assert(!device.hasRejections());
  device.addSignal(F("Reused"), &floating).writeAtInterval(BLAECK_OFF).writeOnChange(0);
  device.writeIfDue();
  expectData(stream, {4}, {0});
}

static unsigned int beforeWriteCalls = 0;
static float callbackValue = 0;
static void sampleBeforeWrite() { ++beforeWriteCalls; callbackValue += 1; }
static unsigned long long fakeUnix() { return 1893456000000000ULL; }
static void writeDuringRefresh()
{
  hostMillis() = 1050;
  callbackDevice->write("V", 10.0f);
  callbackValue = 11;
}

static void reportingCallbacksAndTimestamps()
{
  hostMillis() = 0;
  FakeStream stream;
  Blaeck device;
  device.begin(stream);
  callbackValue = 0;
  beforeWriteCalls = 0;
  device.addSignal(F("Value"), &callbackValue)
      .writeAtInterval(BLAECK_ON_CHANGE, 10).writeOnChange(1, 0);
  device.setBeforeWriteCallback(sampleBeforeWrite);
  device.writeIfDue();
  expectData(stream, {4}, {0});
  assert(beforeWriteCalls == 0);
  command(device, stream, "<BLAECK.ACTIVATE,1000>");
  device.writeIfDue();
  expectData(stream, {4}, {0}); // initial interval merges with the callback's immediate change
  assert(beforeWriteCalls == 1);
  hostMillis() = 1;
  device.writeIfDue();
  expectData(stream, {4}, {});
  assert(beforeWriteCalls == 1);
  device.write("Value", 5.0f);
  expectData(stream, {4}, {0});
  assert(beforeWriteCalls == 1);
  device.writeAll();
  expectData(stream, {4}, {0});
  assert(beforeWriteCalls == 2);
  device.setBeforeWriteCallback(nullptr);
  device.setTimestampMode(BLAECK_MICROS);
  hostMicros() = UINT32_MAX - 5;
  device.writeIfDue(); // quiet polls must still extend the clock
  expectData(stream, {4}, {});
  hostMicros() = 10;
  device.writeIfDue();
  callbackValue += 1;
  device.writeIfDue();
  auto frames = takeData(stream.data.output, {4});
  assert(frames.size() == 1 && frames[0].timestamp == (1ULL << 32) + 10);
  device.setTimestampCallback(fakeUnix);
  device.setTimestampMode(BLAECK_UNIX);
  callbackValue += 1;
  device.writeIfDue();
  frames = takeData(stream.data.output, {4});
  assert(frames.size() == 1 && frames[0].timestamp == fakeUnix());
  callbackValue += 1;
  device.writeIfDue(123456789ULL);
  frames = takeData(stream.data.output, {4});
  assert(frames.size() == 1 && frames[0].timestamp == 123456789ULL);
  device.setTimestampMode(BLAECK_NO_TIMESTAMP);
  callbackValue += 1;
  device.writeIfDue();
  frames = takeData(stream.data.output, {4});
  assert(frames.size() == 1 && frames[0].mode == 0);
  hostMillis() = hostMicros() = 0;
}

static void reportingAllocationAndReconnect()
{
  hostMillis() = 0;
  FakeStream stream;
  Capture debug;
  Blaeck device;
  device.begin(stream).withDebugStream(&debug);
  float value = 0;
  auto handle = device.addSignal(F("Value"), &value);
  failAfter = 0;
  handle.writeOnChange(0);
  failAfter = -1;
  assert(device.hasRejections() && debug.text.find("No RAM") != std::string::npos);
  device.writeIfDue();
  expectData(stream, {4}, {});
  handle.writeAtInterval(BLAECK_OFF).writeOnChange(0);
  device.writeIfDue();
  expectData(stream, {4}, {0});
  device.clearAllSignals();
  char text[32] = "";
  device.addSignal(F("Text"), text).writeAtInterval(BLAECK_OFF).writeOnChange(0, 0);
  failAfter = 0;
  device.writeIfDue();
  failAfter = -1;
  expectData(stream, {-1}, {});
  assert(device.hasRejections());
  device.writeIfDue();
  expectData(stream, {-1}, {0});
  strcpy(text, "longer");
  failAfter = 0;
  device.writeIfDue();
  failAfter = -1;
  expectData(stream, {-1}, {});
  device.writeIfDue();
  expectData(stream, {-1}, {0});
  FakeServer<> server;
  SocketState first, replacement;
  Blaeck tcp;
  tcp.begin(server).withClients(1);
  tcp.addSignal(F("V"), &value).writeAtInterval(BLAECK_OFF).writeOnChange(0);
  tcp.tick(); // no host: do not consume initial value
  server.pending.push_back(&first);
  first.input = "<BLAECK.GET_DEVICES>";
  tcp.tick();
  auto frames = takeData(first.output, {4});
  assert(frames.size() == 1);
  first.open = false;
  tcp.read();
  server.pending.push_back(&replacement);
  replacement.input = "<BLAECK.GET_DEVICES>";
  tcp.tick();
  frames = takeData(replacement.output, {4});
  assert(frames.size() == 1); // reconnect receives unchanged initial value
}

static void reportingReconfiguration()
{
  hostMillis() = 0;
  FakeStream stream;
  Blaeck device;
  device.begin(stream);
  float value = 0;
  auto signal = device.addSignal(F("V"), &value);
  signal.writeAtInterval(BLAECK_ON_CHANGE);
  device.writeIfDue();
  expectData(stream, {4}, {});
  command(device, stream, "<BLAECK.ACTIVATE,0>");
  device.writeIfDue();
  expectData(stream, {4}, {0});
  device.writeIfDue();
  expectData(stream, {4}, {});
  signal.writeAtInterval(BLAECK_ALWAYS);
  device.writeIfDue();
  expectData(stream, {4}, {0});
  device.writeIfDue(); // ACTIVATE,0 still means every pass, not disabled
  expectData(stream, {4}, {0});
  signal.writeAtInterval(BLAECK_OFF);
  device.writeIfDue();
  expectData(stream, {4}, {});
  signal.writeOnChange(0, 0).writeAtInterval(BLAECK_ON_CHANGE);
  device.writeIfDue();
  expectData(stream, {4}, {0}); // re-enabling tracking starts a new baseline
  signal.writeAtInterval(BLAECK_OFF);
  value = 1;
  device.writeIfDue();
  expectData(stream, {4}, {0}); // interval OFF does not cancel immediate changes
  value = 2;
  device.writeAll();
  expectData(stream, {4}, {0});
  device.writeIfDue();
  expectData(stream, {4}, {});
  device.clearAllSignals();
  callbackValue = 0;
  beforeWriteCalls = 0;
  device.addSignal(F("Callback"), &callbackValue).writeAtInterval(BLAECK_ON_CHANGE, 10);
  device.setBeforeWriteCallback(sampleBeforeWrite);
  device.writeIfDue();
  expectData(stream, {4}, {0});
  device.writeIfDue();
  expectData(stream, {4}, {});
  assert(beforeWriteCalls == 2); // refresh even when filtering suppresses the frame
  command(device, stream, "<BLAECK.WRITE_DATA>");
  auto frames = takeData(stream.data.output, {4});
  assert(frames.size() == 1 && (frames[0].flags & 2) != 0 && beforeWriteCalls == 3);
}

template<class T>
static void reportingIntegerType(int width)
{
  FakeStream stream;
  Blaeck device;
  device.begin(stream);
  // Arduino long is 32 bits, including when the host running this suite uses 64-bit long.
  const T minimum = width == 4 && std::numeric_limits<T>::is_signed
      ? static_cast<T>(INT32_MIN) : std::numeric_limits<T>::min();
  const T maximum = width == 4
      ? static_cast<T>(std::numeric_limits<T>::is_signed ? INT32_MAX : UINT32_MAX)
      : std::numeric_limits<T>::max();
  T value = minimum;
  auto signal = device.addSignal(F("V"), &value);
  signal.writeAtInterval(BLAECK_OFF).writeOnChange(1.5, 0);
  device.writeIfDue();
  expectData(stream, {width}, {0});
  ++value;
  device.writeIfDue();
  expectData(stream, {width}, {});
  ++value;
  device.writeIfDue();
  expectData(stream, {width}, {0}); // fractional thresholds round up for integer distances
  value = maximum;
  device.writeIfDue();
  const auto frames = takeData(stream.data.output, {width});
  assert(frames.size() == 1);
  assert(memcmp(frames[0].values[0].data(), &value, width) == 0);
  signal.writeOnChange(ldexp(1.0, sizeof(unsigned long) * CHAR_BIT), 0);
  value = minimum;
  device.writeIfDue();
  expectData(stream, {width}, {});
}

template<class T>
static void reportingFloatingType()
{
  FakeStream stream;
  Blaeck device;
  device.begin(stream);
  T value = 0;
  auto signal = device.addSignal(F("V"), &value);
  signal.writeAtInterval(BLAECK_OFF).writeOnChange(0, 0);
  device.writeIfDue();
  expectData(stream, {sizeof(T)}, {0});
  value = -static_cast<T>(0.0);
  device.writeIfDue();
  expectData(stream, {sizeof(T)}, {0}); // representation change at zero threshold
  signal.writeOnChange(100, 0);
  value = 0;
  device.writeIfDue();
  expectData(stream, {sizeof(T)}, {});
  value = std::numeric_limits<T>::denorm_min();
  device.writeIfDue();
  expectData(stream, {sizeof(T)}, {0});
  device.writeIfDue();
  expectData(stream, {sizeof(T)}, {});
  value = 1;
  device.writeIfDue();
  expectData(stream, {sizeof(T)}, {0}); // transition out of subnormal also qualifies
  value = 100;
  device.writeIfDue();
  expectData(stream, {sizeof(T)}, {});
  value = 101;
  device.writeIfDue();
  expectData(stream, {sizeof(T)}, {0});
}

static void reportingPartialFrames()
{
  for (bool buffered : {false, true})
  {
    FakeServer<> server;
    SocketState socket;
    Capture debug;
    Blaeck device;
    device.begin(server).withDebugStream(&debug);
    device.setBufferedWrites(buffered);
    char value[256];
    memset(value, 'x', 255);
    value[255] = 0;
    device.addSignal(F("V"), value).writeAtInterval(BLAECK_OFF).writeOnChange(0);
    server.pending.push_back(&socket);
    socket.input = "<BLAECK.GET_DEVICES>";
    device.read();
    socket.output.clear();
    socket.writeLimit = 1;
    device.writeIfDue();
    assert(debug.text.find("Incomplete transport write") != std::string::npos);
    socket.output.clear(); // discard the intentionally incomplete frame
    socket.writeLimit = SIZE_MAX;
    device.writeIfDue();
    auto frames = takeData(socket.output, {-1});
    assert(frames.size() == 1 && frames[0].values[0] == value);
    device.writeIfDue();
    assert(takeData(socket.output, {-1}).empty());
  }

  FakeStream stream;
  Capture debug;
  Blaeck device;
  device.begin(stream).withDebugStream(&debug);
  device.setBufferedWrites(true);
  char value[256];
  memset(value, 'x', 255);
  value[255] = 0;
  device.addSignal(F("V"), value).writeAtInterval(BLAECK_OFF).writeOnChange(0);
  // Snapshot and initial frame buffer succeed; frame-buffer growth fails.
  failAfter = 2;
  device.writeIfDue();
  failAfter = -1;
  expectData(stream, {-1}, {});
  assert(debug.text.find("frame dropped") != std::string::npos);
  device.writeIfDue();
  expectData(stream, {-1}, {0});
}

static void reportingCallbackWriteClock()
{
  FakeStream stream;
  Blaeck device;
  device.begin(stream);
  callbackValue = 0;
  callbackDevice = &device;
  hostMillis() = 0;
  device.addSignal(F("V"), &callbackValue).writeAtInterval(BLAECK_OFF).writeOnChange(0);
  device.writeIfDue();
  expectData(stream, {4}, {0});
  device.setBeforeWriteCallback(writeDuringRefresh);
  command(device, stream, "<BLAECK.ACTIVATE,1000>");
  hostMillis() = 1000;
  device.writeIfDue();
  expectData(stream, {4}, {0}); // only the explicit write inside the refresh callback
  hostMillis() = 1149;
  device.writeIfDue();
  expectData(stream, {4}, {});
  hostMillis() = 1150;
  device.writeIfDue();
  expectData(stream, {4}, {0});
  callbackDevice = nullptr;
}

static void reportingFrameClassification(bool buffered)
{
  hostMillis() = 0;
  FakeStream stream;
  Blaeck device;
  device.begin(stream);
  device.setBufferedWrites(buffered);
  float value = 0;
  auto signal = device.addSignal(F("V"), &value);
  signal.writeAtInterval(BLAECK_ON_CHANGE, 0.5).writeOnChange(1, 0);
  const auto expectFlags = [&](byte flags)
  {
    const auto frames = takeData(stream.data.output, {4});
    assert(frames.size() == 1 && frames[0].flags == flags);
  };
  device.writeIfDue();
  expectFlags(0x01); // spontaneous first report, with independent restart flag
  device.write("V", 1.0f);
  expectFlags(0);
  device.writeAll();
  expectFlags(0);
  command(device, stream, "<#42:BLAECK.WRITE_DATA>");
  expectFlags(0x02);
  command(device, stream, "<BLAECK.ACTIVATE,1000>");
  device.writeIfDue();
  expectFlags(0x04); // initial interval bypasses the previously requested baseline
  value = 1.5f;
  hostMillis() = 1000;
  device.writeIfDue();
  expectFlags(0x04);
  value = 2.5f;
  hostMillis() = 1100;
  device.writeIfDue();
  expectFlags(0); // being activated is not enough to make a frame an interval report
  value = 3.5f;
  hostMillis() = 2000;
  device.writeIfDue();
  expectFlags(0x04); // merged interval and immediate report
  signal.writeAtInterval(BLAECK_ON_CHANGE, 2);
  value = 4.5f;
  hostMillis() = 3000;
  device.writeIfDue();
  expectFlags(0); // interval due, but only the immediate path qualifies
  device.write("V", 10.0f);
  expectFlags(0);
  device.writeAll();
  expectFlags(0);
  command(device, stream, "<BLAECK.WRITE_DATA>");
  expectFlags(0x02);
  value = 12.5f;
  hostMillis() = 4000;
  device.writeIfDue();
  expectFlags(0x04);
  command(device, stream, "<BLAECK.DEACTIVATE>");
  value = 14;
  device.writeIfDue();
  expectFlags(0);

  FakeStream firstStream;
  Blaeck first;
  first.begin(firstStream);
  first.setBufferedWrites(buffered);
  first.addSignal(F("V"), &value);
  command(first, firstStream, "<BLAECK.ACTIVATE,1000>");
  first.writeIfDue();
  const auto initial = takeData(firstStream.data.output, {4});
  assert(initial.size() == 1 && initial[0].flags == 0x05);
}

int main()
{
  storedConfigurationStrings();
  ordinaryConfiguration(false);
  ordinaryConfiguration(true);
  configurationAllocationFailures();
  flashSignalText(false);
  flashSignalText(true);
  flashNamesAndFailures();
  flashStateText(false);
  flashStateText(true);
  flashNumericWrites<bool>();
  flashNumericWrites<byte>();
  flashNumericWrites<short>();
  flashNumericWrites<unsigned short>();
  flashNumericWrites<int>();
  flashNumericWrites<unsigned int>();
  flashNumericWrites<long>();
  flashNumericWrites<unsigned long>();
  flashNumericWrites<float>();
  flashNumericWrites<double>();
  if (!BLAECK_TEST_REPORTING_ONLY)
  {
    for (bool tcp : {false, true})
      for (bool buffered : {false, true})
        commandBufferBoundaries(tcp, buffered);
  }
  if (BLAECK_TEST_COMMAND_BUFFER_ONLY)
  {
    std::cout << "PASS: command buffer size " << BLAECK_COMMAND_MAX_CHARS_DEFAULT
              << ", capacity, long prefixes, acknowledgements and recovery on Serial/TCP\n";
    return 0;
  }
  if (!BLAECK_TEST_REPORTING_ONLY)
  {
    diagnosticMessages();
    crc32Behavior();
    sessionBehavior(false);
    sessionBehavior(true);
    optionalPeerDiagnostics<FakeClient>("Client #0 connected: 192.0.2.1:1234\r\n");
    optionalPeerDiagnostics<NoAddressClient>("Client #0 connected\r\n");
    optionalPeerDiagnostics<NoPortClient>("Client #0 connected\r\n");
    optionalPeerDiagnostics<NoPeerClient>("Client #0 connected\r\n");
    optionalPeerDiagnostics<UnprintablePeerClient>("Client #0 connected\r\n");
    lifecycleAndErrors();
    detachFromCallbacks();
    unifiedConnections();
    beginOnlyOnce();
    noDeviceOwnership();
    subDevices(false);
    subDevices(true);
    deviceCommands();
    deviceNoticesBeforeHost();
    sameNamesAcrossDevices();
  }
  reportingPolicies(false);
  reportingPolicies(true);
  reportingToggle(false);
  reportingToggle(true);
  reportingToggleFailures();
  reportingActivationSnapshot(false);
  reportingActivationSnapshot(true);
  sharedBaselineAndClock();
  reportingTypesAndFailures(false);
  reportingTypesAndFailures(true);
  reportingCallbacksAndTimestamps();
  reportingAllocationAndReconnect();
  reportingReconfiguration();
  reportingIntegerType<byte>(1);
  reportingIntegerType<short>(2);
  reportingIntegerType<unsigned short>(2);
  reportingIntegerType<int>(4);
  reportingIntegerType<unsigned int>(4);
  reportingIntegerType<long>(4);
  reportingIntegerType<unsigned long>(4);
  reportingFloatingType<float>();
  reportingFloatingType<double>();
  reportingPartialFrames();
  reportingCallbackWriteClock();
  reportingFrameClassification(false);
  reportingFrameClassification(true);
  std::cout << "PASS: protocol/transport and signal policies, shared baselines, clocks, strings, failures and reconnect\n";
}
