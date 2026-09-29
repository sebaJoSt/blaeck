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

// tick() is a sketch's only loop call. The tests call its two halves apart, to see what each
// one does.
class TestBlaeck : public Blaeck
{
public:
  using Blaeck::read;
  using Blaeck::writeIfDue;
};

static_assert(!std::is_polymorphic<Blaeck>::value, "Blaeck needs no virtual transport hooks");
static_assert(!std::is_copy_constructible<Blaeck>::value, "Blaeck owns its allocations");
static_assert(!std::is_copy_assignable<Blaeck>::value, "Blaeck owns its allocations");
static_assert(std::is_same<
    decltype(std::declval<BlaeckBeginRef &>()
                 .withDebugStream(nullptr).withClients(1)),
    BlaeckBeginRef &>::value, "Every begin option must preserve the same handle");

static int failAfter = -1;
static size_t allocations = 0;

static_assert(BLAECK_ANY_CHANGE == 0, "Any-change is a zero numeric threshold");
static_assert(std::is_same<decltype(BLAECK_ANY_CHANGE), const double>::value,
              "Any-change must not select the mode overload");
static_assert(std::is_same<
    decltype(std::declval<BlaeckSignalRef &>()
                 .writeOnChange(0).writeOnChange(0, 0)
                 .writeOnChange(BLAECK_ANY_CHANGE, 250).writeOnChange(BLAECK_OFF)),
    BlaeckSignalRef &>::value, "Reporting overloads chain on the handle");

template <class Handle>
static auto acceptsModeWithInterval(int) -> decltype(
    std::declval<Handle &>().writeOnChange(BLAECK_OFF, 100), std::true_type{});
template <class>
static std::false_type acceptsModeWithInterval(...);

static_assert(!decltype(acceptsModeWithInterval<BlaeckSignalRef>(0))::value,
              "An enum mode plus rate limit must not become a numeric threshold");

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

class PacketProbe : public TestBlaeck
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

class ReportingProbe : public TestBlaeck
{
public:
  const ReportingState *reporting(int index) const { return Signals[index].Reporting; }
};

class ConfigurationProbe : public TestBlaeck
{
public:
  const blaeck::blaeck_detail::CommandHandlerEntry &commandMeta(int index) const { return _commandHandlers[index]; }
  const blaeck::blaeck_detail::PropertyEntry &propertyMeta(int index) const { return _properties[index]; }
  int propertyIndex(blaeck::BlaeckString name) const { return _findProperty(name); }
  void sendEntities() { writeEntities(0); }
#if BLAECK_ENABLE_IOT
  const blaeck::blaeck_detail::EventChannelEntry &eventMeta(int index) const { return _eventChannels[index]; }
  const blaeck::blaeck_detail::EventTypeEntry &eventType(int index) const { return _eventTypes[index]; }
  int eventIndex(const char *name) const { return _findEventChannel(0, name); }
#endif
  void cleanCatalogs() { _entityCatalogDirty = false; }
  bool dirtyCatalogs() const { return _entityCatalogDirty; }
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
static int presses;
static void onPress() { ++presses; }

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
  TestBlaeck device;
  device.setBufferedWrites(buffered);
  device.begin(server).withClients(3).withDebugStream(&debug);
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
    TestBlaeck device;
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
    TestBlaeck one, two;
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
    TestBlaeck device;
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
  TestBlaeck device;
  device.begin(first).withClients(0).withDebugStream(&debug);
  assert(device.transportError() == Blaeck::TransportError::InvalidClientCount);
  TestBlaeck manyClients;
  manyClients.begin(second).withClients(255);
  manyClients.read(); // Regression: byte-sized round-robin counter would loop forever at 255.
  assert(manyClients.transportError() == Blaeck::TransportError::None);
}

static void detachFromCallbacks()
{
  FakeServer<> server;
  SocketState a, b;
  TestBlaeck device;
  callbackDevice = &device;
  device.setClientConnectedCallback(detachInCallback);
  device.begin(server);
  server.pending.push_back(&a);
  device.read();
  assert(!a.open && device.transportError() == Blaeck::TransportError::NotStarted);
  TestBlaeck disconnected;
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
  TestBlaeck takeover;
  callbackDevice = &takeover;
  takeover.setClientDisconnectedCallback(detachInCallback);
  takeover.begin(server);
  server.pending = {&first, &second};
  takeover.read();
  takeover.read();
  first.input = "<BLAECK.GET_DEVICES>";
  takeover.read();
  first.output.clear();
  second.input = "<BLAECK.WRITE_DATA>";
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
  TestBlaeck device;
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
    TestBlaeck tcp;
    tcp.begin(server).withClients(1);
    assert(tcp.isBufferedWrites() == BLAECK_TCP_BUFFERED_WRITES_DEFAULT);
    server.pending.push_back(&host);
    tcp.read();
  }
  assert(!host.open && stream.data.open);

  for (bool buffered : {false, true})
  {
    TestBlaeck serial, tcp;
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
  TestBlaeck unattached;
  assert(unattached.printTransportError(&debug));
  assert(debug.text.find("begin(stream) or begin(server)") != std::string::npos);

  auto handler = [](const char *, const char *const *, byte) {};
  for (bool tcp : {false, true})
  {
    FakeStream stream;
    FakeServer<> server;
    TestBlaeck device;
    auto setup = tcp ? device.begin(server) : device.begin(stream);
    setup.withDebugStream(&debug);
    debug.text.clear();
    assert(!device.printRejections(&debug) && debug.text.empty());

    float value = 0;
    failAfter = 0;
    device.addSignal("RAM", &value);
    device.addSignal(F("Flash"), &value);
    failAfter = -1;
    assert(debug.text.find("Dropped 'RAM': no room for another signal. The board is out of RAM.")
           != std::string::npos);
    assert(debug.text.find("Dropped 'Flash': no room for another signal.") != std::string::npos);
    assert(debug.text.find("begin(Serial)") == std::string::npos);
    device.onCommand("BLAECK.RESERVED", handler);

    debug.text.clear();
    assert(device.printRejections(&debug));
    assert(debug.text.find("2 signal registrations rejected.") != std::string::npos);
    assert(debug.text.find("1 command and button registrations rejected.") != std::string::npos);
    assert(debug.text.find("invalid or conflicting names") != std::string::npos);
    assert(debug.text.find("begin(Serial)") == std::string::npos);
  }

  // Every table reports a chunk it couldn't allocate.
  for (int table = 0; table < 5; ++table)
  {
    FakeStream stream;
    TestBlaeck device;
    device.begin(stream).withDebugStream(&debug);
    float value = 0;
    debug.text.clear();
    failAfter = table == 4 ? 1 : 0; // For event types, allocate the channel first.
    switch (table)
    {
    case 0: device.addSignal(F("Value"), &value); break;
    case 1: device.onCommand("COMMAND", handler); break;
    case 2: device.addSensor(F("Value"), &value); break;
    case 3:
    case 4: device.addEvent(F("Activity"), F("started")); break;
    }
    failAfter = -1;
    assert(device.hasRejections());
    assert(debug.text.find("no room for another") != std::string::npos);
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
  uint16_t schemaHash = 0;
};

// The frames in a device's output as the readers below take them: unescaped, between
// "<BLAECK:" and "/BLAECK>\r\n". Checks each frame on the way: nothing between its markers may
// read as a frame's start or a line's end, and it ends in "/>" and LF.
static std::string unescaped(const std::string &wire)
{
  std::string out;
  size_t p = 0;
  for (;;)
  {
    const size_t start = wire.find("<blaeck:", p);
    if (start == std::string::npos)
    {
      out.append(wire, p, std::string::npos);
      return out;
    }
    out.append(wire, p, start - p);
    out += "<BLAECK:";
    size_t q = start + 8;
    for (;;)
    {
      assert(q < wire.size());
      char c = wire[q++];
      assert(c != '<' && c != '\r');
      if (c == '\n')
      {
        assert(out.size() >= 2 && out.compare(out.size() - 2, 2, "/>") == 0);
        out.erase(out.size() - 2);
        out += "/BLAECK>\r\n";
        break;
      }
      if (c == '\\')
      {
        assert(q < wire.size());
        const char escaped = static_cast<char>(wire[q++] ^ 0x20);
        assert(escaped == '<' || escaped == '\\' || escaped == '\r' || escaped == '\n');
        c = escaped;
      }
      out += c;
    }
    p = q;
  }
}

static std::vector<DataFrame> takeData(std::string &wire, const std::vector<int> &widths)
{
  std::string output = unescaped(wire);
  wire.clear();
  std::vector<DataFrame> result;
  const std::string marker = std::string("<BLAECK:") + char(0xD3) + ':';
  size_t start = 0;
  while ((start = output.find(marker, start)) != std::string::npos)
  {
    const size_t end = output.find("/BLAECK>\r\n", start);
    assert(end != std::string::npos && end >= start + 22);
    size_t p = start + marker.size() + 4;
    assert(output[p++] == ':');
    DataFrame frame;
    frame.flags = static_cast<byte>(output[p++]);
    frame.schemaHash = static_cast<uint16_t>(static_cast<byte>(output[p]) |
                                             (static_cast<byte>(output[p + 1]) << 8));
    p += 2;
    frame.mode = static_cast<byte>(output[p++]);
    if (frame.mode != BLAECK_NO_TIMESTAMP)
    {
      memcpy(&frame.timestamp, output.data() + p, 8);
      p += 8;
    }
    while (p < end - 4)
    {
      uint16_t id;
      memcpy(&id, output.data() + p, 2);
      p += 2;
      assert(id < widths.size());
      const size_t size = widths[id] < 0 ? static_cast<byte>(output[p++]) : widths[id];
      assert(p + size <= end - 4);
      frame.ids.push_back(id);
      frame.values.push_back(output.substr(p, size));
      p += size;
    }
    assert(p == end - 4);
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

static void command(TestBlaeck &device, FakeStream &stream, const char *text)
{
  stream.data.input = text;
  device.read();
}

// Tables grow a chunk at a time, past any chunk boundary, and keep their chunks when cleared.
static int chunkCommandCalls = 0;
static void chunkedTables()
{
  FakeStream stream;
  Capture debug;
  TestBlaeck device;
  device.begin(stream).withDebugStream(&debug);

  float values[20] = {};
  std::string names[20];
  for (int i = 0; i < 20; ++i)
  {
    names[i] = "S" + std::to_string(i);
    device.addSignal(names[i].c_str(), &values[i]);
  }
  assert(device.SignalCount == 20);
  for (int i = 0; i < 20; ++i)
    assert(device.findSignalIndex(names[i].c_str()) == i);

  // Adding as many signals again after clearing allocates no chunk.
  device.clearAllSignals();
  const size_t before = allocations;
  for (int i = 0; i < 20; ++i)
    device.addSignal(names[19 - i].c_str(), &values[i]);
  assert(allocations == before);
  assert(device.SignalCount == 20);
  assert(device.findSignalIndex("S19") == 0 && device.findSignalIndex("S0") == 19);

  auto handler = [](const char *, const char *const *, byte) { ++chunkCommandCalls; };
  for (int i = 0; i < 20; ++i)
    device.onCommand(("C" + std::to_string(i)).c_str(), handler);
  command(device, stream, "<C0>");
  command(device, stream, "<C19>");
  assert(chunkCommandCalls == 2);

  for (int i = 0; i < 10; ++i)
    device.addDevice(("D" + std::to_string(i)).c_str());

  device.addEvent(F("Activity"), F("a,b,c,d,e,f,g,h,i,j"));
  for (int i = 0; i < 10; ++i)
    device.addEventType(F("Activity"), ("extra" + std::to_string(i)).c_str());
  assert(!device.hasRejections());
  assert(debug.text.find("no room") == std::string::npos);
}

static std::string commandFramePayload(const std::string &wire, byte key, uint32_t messageId)
{
  const std::string output = unescaped(wire);
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
  TestBlaeck device;
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

// Within a frame, bytes that would read as a frame's start, an escape or a line's end go escaped,
// and read back unchanged; '/' goes as it is. The frame is one line ending in "/>" and LF.
static void frameEscaping(bool buffered)
{
  FakeStream stream;
  TestBlaeck device;
  device.begin(stream);
  device.setBufferedWrites(buffered);
  static char text[] = "a<b\\c\rd\ne/f";
  device.addSignal("Text", text).writeAtInterval(BLAECK_OFF);
  device.writeAll();

  const std::string wire = stream.data.output;
  assert(wire.compare(0, 8, "<blaeck:") == 0);
  assert(wire.compare(wire.size() - 3, 3, "/>\n") == 0);
  assert(wire.find('\n') == wire.size() - 1);
  assert(wire.find('<', 1) == std::string::npos);
  assert(wire.find('\r') == std::string::npos);
  for (const char *escaped : {"\\\x1c", "\\\x7c", "\\\x2d", "\\\x2a"})
    assert(wire.find(escaped) != std::string::npos);
  assert(wire.find("e/f") != std::string::npos);

  const auto frames = takeData(stream.data.output, {-1});
  assert(frames.size() == 1 && frames[0].values == std::vector<std::string>({"a<b\\c\rd\ne/f"}));
}

static void flashSignalText(bool buffered)
{
  for (int naming = 0; naming < 3; ++naming)
  for (bool timestamped : {false, true})
  for (bool tracking : {false, true})
  {
    FakeStream stream;
    Capture debug;
    TestBlaeck device;
    device.begin(stream).withDebugStream(&debug);
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
  TestBlaeck device;
  device.begin(stream).withDebugStream(&debug);
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
  assert(debug.text.find("No RAM for a text snapshot") != std::string::npos);
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
}

template<class T>
static void flashNumericWrites()
{
  FakeStream stream;
  TestBlaeck device;
  device.begin(stream);
  T value = 0;
  device.addSignal(F("Value"), &value);
  device.write(F("Value"), static_cast<T>(1));
  assert(value == 1);
  device.write(F("Value"), static_cast<T>(0), 123);
  assert(value == 0);
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

#if BLAECK_ENABLE_IOT
static const char *flashTestGetter() { return "Getter"; }

static void ordinaryConfiguration(bool buffered)
{
  using blaeck::BlaeckString;
  FakeStream stream;
  ConfigurationProbe device;
  device.begin(stream);
  device.setBufferedWrites(buffered);
  float value = 1;
  byte selected = 1;
  char unit[] = "V", icon[] = "mdi:pulse", label[] = "Voltage";
  char options[] = "Low,High";
  char eventTypes[] = "start,stop", extraType[] = "reset";
  char deviceClass[] = "voltage";
  device.addSignal("Value", &value);
  auto number = device.addNumberInput("SET", &value).withRange(0, 10, 1);
  number.withUnit(unit).withDeviceClass(deviceClass).withIcon(icon).withDisplayName(label);
  device.addSelect("SELECT", &selected, options);
  device.addButton("PRESS", onPress).withIcon(icon);
  device.addSensor("Getter", flashTestGetter);
  auto sensor = device.addSensor("Voltage", &value);
  sensor.withUnit(unit).withDeviceClass(deviceClass).withIcon(icon);
  device.addSensor("LevelState", &selected, options).withDeviceClass("enum");
  auto event = device.addEvent("Action", eventTypes);
  event.withIcon(icon).withDeviceClass("button");
  const bool added = device.addEventType("Action", extraType);
  assert(added);
  assert(!device.hasRejections());

  unit[0] = icon[0] = label[0] = options[0] = eventTypes[0] = extraType[0] =
      deviceClass[0] = 'X';
  stream.data.output.clear();
  device.sendEntities();
  for (const char *text : {"SET", "V", "voltage", "mdi:pulse", "Voltage", "Low,High", "SELECT", "Getter",
                           "PRESS", "Action", "button", "start", "stop", "reset"})
    assert(stream.data.output.find(std::string(text) + '\0') != std::string::npos);
  char option[8];
  assert(device.getSelectOptionNameAt("SELECT", 1, option, sizeof(option)));
  assert(strcmp(option, "High") == 0);
  assert(device.getSelectOptionNameAt(F("LevelState"), 0, option, sizeof(option)));
  assert(strcmp(option, "Low") == 0);
  assert(device.getSelectOptionIndexOf("SELECT", "Low") == 0);
  assert(device.getSelectOptionIndexOf("SELECT", "Missing") == -1);
  assert(device.getSelectOptionIndexOf("SET", "Low") == -1);
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
  stream.data.output.clear();
  device.cleanCatalogs();
  const size_t beforeSame = allocations;
  number.withUnit(F("V")).withRange(0, 10, 1);
  sensor.withUnit(F("V")).withIcon("mdi:pulse");
  event.withIcon(F("mdi:pulse"));
  assert(!device.dirtyCatalogs() && allocations == beforeSame);
  number.withUnit(nullptr);
  sensor.withUnit("").withIcon(nullptr);
  event.withIcon("");
  assert(device.propertyMeta(0).presentation->unit == nullptr);
  assert(device.propertyMeta(3).presentation->unit == nullptr && device.propertyMeta(3).presentation->icon == nullptr);
  assert(!device.hasRejections());
  device.clearAllCommandHandlers();
  device.clearAllEvents();
  device.clearAllSignals();
  assert(device.commandMeta(0).press == nullptr && device.commandMeta(0).icon == nullptr);
  assert(device.eventMeta(0).deviceClass == nullptr && device.eventType(0).text == nullptr);
  assert(device.eventType(2).text == nullptr);
}

#else
// BLAECK_ENABLE_IOT=0: inputs, sensors, events and buttons store nothing and are no rejection;
// the entity list goes out empty, and a button's name is unknown. Signals and plain commands work.
static void iotOff()
{
  FakeStream stream;
  ConfigurationProbe device;
  device.begin(stream);
  float value = 1;
  device.addSignal("Value", &value);
  device.onCommand("Ping", onPing);
  device.addNumberInput("SET", &value);
  device.addSensor("Voltage", &value);
  device.addButton("PRESS", onPress);
  device.addEvent(F("Action"), F("start"));
  assert(!device.addEventType("Action", "stop"));
  assert(!device.hasRejections());
  assert(device.propertyIndex("SET") == -1);
  stream.data.output.clear();
  device.sendEntities();
  assert(commandFramePayload(stream.data.output, 0x90, 0).empty());
  stream.data.output.clear();
  device.writeEvent("Action", "start");
  assert(stream.data.output.empty());
  presses = 0;
  command(device, stream, "<PRESS>");
  assert(presses == 0);
  assert(static_cast<byte>(commandFramePayload(stream.data.output, 0xA5, 0)[9]) == BLAECK_ACK_UNKNOWN);
  pings.clear();
  command(device, stream, "<Ping,1>");
  assert(pings.size() == 1);
}
#endif

#if BLAECK_ENABLE_IOT
static void configurationAllocationFailures()
{
  using blaeck::BlaeckString;
  FakeStream stream;
  Capture debug;
  ConfigurationProbe device;
  device.begin(stream).withDebugStream(&debug);
  float value = 0;
  auto number = device.addNumberInput("SET", &value).withRange(0, 10, 1).withUnit(F("V"));
  auto sensor = device.addSensor(F("State"), &value).withUnit(F("V"));
  auto event = device.addEvent(F("Event"), F("start")).withIcon(F("mdi:pulse"));
  device.cleanCatalogs();
  const size_t before = allocations;
  failAfter = 0;
  number.withUnit("Replacement");
  sensor.withUnit("Replacement");
  event.withIcon("Replacement");
  device.addEvent(F("Rejected"), "start,stop");
  assert(!device.addEventType("Event", "stop"));
  device.addSensor(F("RejectedSensor"), &value);
  failAfter = -1;
  assert(!device.dirtyCatalogs());
  assert(BlaeckString(device.propertyMeta(0).presentation->unit) == "V");
  assert(BlaeckString(device.propertyMeta(1).presentation->unit) == "V");
  assert(device.propertyIndex("RejectedSensor") == -1);
  assert(BlaeckString(device.eventMeta(0).icon) == "mdi:pulse");
  assert(device.eventIndex("Rejected") == -1);
  assert(allocations != before || device.hasRejections());
  assert(device.hasRejections());
  Capture rejections;
  assert(device.printRejections(&rejections));
  assert(rejections.text.find("input and sensor registrations rejected") != std::string::npos);
}
#endif

static void beginOnlyOnce()
{
  for (bool tcp : {false, true})
  {
    FakeStream stream;
    FakeServer<> server;
    SocketState host;
    TestBlaeck device;
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
    TestBlaeck device;
    device.end(); // Teardown before initialization does not consume begin().
    auto setup = tcp ? device.begin(server) : device.begin(stream);
    setup.withDebugStream(&debug);
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
    rejected.withClients(1).withDebugStream(&ignoredDebug);
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

// The DeviceID byte of the catalog record that holds `name`. It sits right before the name,
// or before the payload length in a command record.
static std::string ownerOf(const std::string &payload, const char *name, size_t gap = 0)
{
  const std::string key = std::string(name) + '\0';
  const size_t at = payload.find(key);
  assert(at != std::string::npos && at >= 1 + gap);
  return payload.substr(at - 1 - gap, 1);
}

// The DeviceID an entry carries: 0 for the board, 1 and up for a device from addDevice().
static std::string owner(byte deviceId)
{
  return std::string(1, static_cast<char>(deviceId));
}

// One B7 record: DeviceID, ParentID 0, DeviceFlags 0, DeviceState, then the three names.
static std::string deviceRecord(byte id, byte state, const char *name, const char *hw, const char *fw)
{
  return std::string(1, static_cast<char>(id)) + '\0' + '\0' + '\0' + static_cast<char>(state) +
         name + '\0' + hw + '\0' + fw + '\0';
}

// The signals after a B7 record: a 2-byte count, then each name and type code.
static std::string signalList(const std::vector<std::pair<std::string, byte>> &signals = {})
{
  std::string out;
  out += static_cast<char>(signals.size() & 0xFF);
  out += static_cast<char>((signals.size() >> 8) & 0xFF);
  for (const auto &signal : signals)
    out += signal.first + '\0' + static_cast<char>(signal.second);
  return out;
}

// A B7 payload: library name and version, the longest command, the record count, then the
// records, each followed by its signals.
static std::string deviceList(byte count, const std::string &records)
{
  const uint16_t payloadMax = BLAECK_COMMAND_MAX_CHARS_DEFAULT - 1;
  return std::string("blaeck") + '\0' + BLAECK_VERSION + '\0' +
         static_cast<char>(payloadMax & 0xFF) + static_cast<char>((payloadMax >> 8) & 0xFF) +
         static_cast<char>(count) + records;
}

// A C1 payload.
static std::string notice(byte id, byte event)
{
  return std::string(1, static_cast<char>(id)) + static_cast<char>(event);
}

static void noDeviceOwnership()
{
  FakeStream stream;
  TestBlaeck device;
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
         deviceList(1, deviceRecord(0, 0, "Solo", "Mega", "n/a") + signalList({{"Value", 8}})));
}

static void subDevices(bool buffered)
{
  hostMillis() = 0;
  auto handler = [](const char *, const char *const *, byte) {};
  FakeStream stream;
  Capture debug;
  TestBlaeck device;
  device.begin(stream).withDebugStream(&debug);
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

  // Duplicate and empty names are rejected; their handles do nothing.
  debug.text.clear();
  BlaeckDeviceRef duplicate = device.addDevice(F("Pump"));
  assert(debug.text.find("duplicate device name") != std::string::npos);
  BlaeckDeviceRef third = device.addDevice(F("Fan"));
  assert(debug.text.find("duplicate device name: Fan") != std::string::npos);
  BlaeckDeviceRef empty = device.addDevice("");
  BlaeckDeviceRef unset;
  third.markMissing();
  empty.writeRestarted();
  assert(!third.isMissing() && !unset.isMissing() && !duplicate.isMissing());
  debug.text.clear();
  assert(device.printRejections(&debug));
  assert(debug.text.find("3 device registrations rejected.") != std::string::npos);

  pump.addSignal(F("Flow"), &flow);
  pump.addSignal(F("Pressure"), &pressure);
  // A rejected or unset handle registers nothing, and counts nothing as rejected.
  third.addSignal(F("Lost"), &orphan).writeOnChange(1.0);
  unset.addSignal("Lost", &orphan);
  unset.onCommand("LOST", handler);
  unset.addSensor(F("Lost"), &orphan);
  unset.addEvent(F("Lost"), F("x"));
  unset.write("Lost", 1.0f);
  unset.writeProperty(F("Lost"));
  unset.writeEvent(F("Lost"), F("x"));
  assert(device.SignalCount == 3 && unset.findSignalIndex("Lost") == -1);
  debug.text.clear();
  assert(device.printRejections(&debug));
  assert(debug.text.find("signal registrations") == std::string::npos);
  assert(debug.text.find("command and button registrations") == std::string::npos);
  device.addSignal(F("Orphan"), &orphan);

  // Inputs and sensors registered through a device's handle belong to that device.
  char pumpStatus[8] = "idle";
  pump.addNumberInput("PUMP_SPEED", &speed).withRange(0.0f, 100.0f, 1.0f);
  pump.addSwitch("PUMP_ON", &pumpOn);
  device.addButton("BOARD_RESET", onPress);
  pump.addSensor(F("PumpStatus"), pumpStatus, sizeof(pumpStatus));
  fan.addEvent(F("FanAlarm"), F("stall"));

  // The board's restart notice is a C1 for device 0.
  device.read();
  assert(commandFramePayload(stream.data.output, 0xC1, 0) == notice(0, 0x01));
  stream.data.output.clear();

  // Each device lists its own signals: Orphan, added after the pump's, still goes with the board.
  command(device, stream, "<BLAECK.GET_DEVICES>");
  assert(commandFramePayload(stream.data.output, 0xB7, 0) ==
         deviceList(3, deviceRecord(0, 0, "Board", "Mega", "n/a") +
                           signalList({{"BoardValue", 8}, {"Orphan", 8}}) +
                       deviceRecord(1, 0, "Pump", "Nano", "1.2") +
                           signalList({{"Flow", 8}, {"Pressure", 8}}) +
                       deviceRecord(2, 0, "Fan", "n/a", "n/a") + signalList()));
  stream.data.output.clear();

  // In the entity list, the entry kind sits between the DeviceID and the name.
  command(device, stream, "<BLAECK.WRITE_ENTITIES>");
  std::string payload = commandFramePayload(stream.data.output, 0x90, 0);
  assert(ownerOf(payload, "PUMP_SPEED", 1) == owner(1));
  assert(ownerOf(payload, "PUMP_ON", 1) == owner(1));
  assert(ownerOf(payload, "PumpStatus", 1) == owner(1));
  assert(ownerOf(payload, "FanAlarm", 1) == owner(2));
  assert(ownerOf(payload, "BOARD_RESET", 1) == owner(0));
  stream.data.output.clear();

  // Names are found within the handle's own device only. A 0x95 carries the property's index.
  device.writeProperty(F("PumpStatus"));
  device.writeEvent(F("FanAlarm"), F("stall"));
  assert(stream.data.output.empty());
  pump.writeProperty(F("PumpStatus"));
  assert(commandFramePayload(stream.data.output, 0x95, 0).substr(0, 2) == std::string("\x02\x00", 2));
  stream.data.output.clear();
  // A 0x85 carries the event's index among the events and its type's index.
  fan.writeEvent(F("FanAlarm"), F("stall"));
  assert(commandFramePayload(stream.data.output, 0x85, 0) == std::string(4, '\0'));
  stream.data.output.clear();

  // A device restart is reported for that device only.
  pump.writeRestarted();
  assert(commandFramePayload(stream.data.output, 0xC1, 0) == notice(1, 0x01));
  stream.data.output.clear();

  const std::vector<int> widths = {4, 4, 4, 4};
  command(device, stream, "<BLAECK.ACTIVATE,0>");
  stream.data.output.clear();
  device.writeIfDue();
  // Numbered in device list order: the board's BoardValue 0 and Orphan 1, the pump's Flow 2 and
  // Pressure 3.
  auto frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{0, 1, 2, 3}));

  // Going missing is a C1 at once, sent only on a real change; the device's signals leave the
  // data frames.
  pump.markMissing();
  assert(commandFramePayload(stream.data.output, 0xC1, 0) == notice(1, 0x02));
  stream.data.output.clear();
  pump.markMissing();
  assert(stream.data.output.empty());
  assert(pump.isMissing());
  device.writeIfDue();
  frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{0, 1}));
  device.writeAll();
  frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{0, 1}));
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
  // A dropped write is no rejection.
  debug.text.clear();
  device.printRejections(&debug);
  assert(debug.text.find("signal registrations") == std::string::npos);

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
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{2, 3}));

  // A device without signals changes nothing in the data when it goes missing.
  fan.markMissing();
  assert(commandFramePayload(stream.data.output, 0xC1, 0) == notice(2, 0x02));
  stream.data.output.clear();
  device.writeIfDue();
  frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{0, 1, 2, 3}));
  fan.markPresent();
  stream.data.output.clear();

  // After a gap, a changed-only signal is sent again even if its value did not change.
  command(device, stream, "<BLAECK.DEACTIVATE>");
  stream.data.output.clear();
  TestBlaeck changes;
  FakeStream changesStream;
  changes.begin(changesStream);
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

  // A board holds at most 254 devices, so the list's count byte covers them and the board.
  TestBlaeck big;
  FakeStream bigStream;
  debug.text.clear();
  big.begin(bigStream).withDebugStream(&debug);
  char name[8];
  for (int i = 0; i < 255; ++i)
  {
    snprintf(name, sizeof(name), "D%d", i);
    big.addDevice(name);
  }
  assert(big.hasRejections()); // the 255th
  assert(debug.text.find("Dropped 'D254': a board has at most 254 devices.") != std::string::npos);
  big.read();
  bigStream.data.output.clear();
  command(big, bigStream, "<BLAECK.GET_DEVICES>");
  const std::string list = commandFramePayload(bigStream.data.output, 0xB7, 0);
  assert(static_cast<byte>(list[std::string("blaeck").size() + 1 + std::string(BLAECK_VERSION).size() + 1 + 2]) == 255);
  assert(list.find(deviceRecord(254, 0, "D253", "n/a", "n/a")) != std::string::npos);
}

// Over TCP a host can receive frames only once it sent a BLAECK. command, normally
// GET_DEVICES. Notices held back until then are answered by the list itself.
static void deviceNoticesBeforeHost()
{
  FakeServer<> server;
  SocketState host;
  TestBlaeck device;
  device.begin(server);
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
  assert(host.output.find(std::string("<blaeck:") + char(0xC1)) == std::string::npos);
  assert(commandFramePayload(host.output, 0xB7, 0) ==
         deviceList(3, deviceRecord(0, 0x02, "Board", "Mega", "n/a") + signalList() +
                       deviceRecord(1, 0x03, "Pump", "n/a", "n/a") + signalList() +
                       deviceRecord(2, 0, "Fan", "n/a", "n/a") + signalList()));
  host.output.clear();

  // Everything was reported: no C1 follows, and the next list shows no restart.
  device.read();
  assert(host.output.empty());
  host.input = "<BLAECK.GET_DEVICES>";
  device.read();
  assert(commandFramePayload(host.output, 0xB7, 0) ==
         deviceList(3, deviceRecord(0, 0, "Board", "Mega", "n/a") + signalList() +
                       deviceRecord(1, 0x01, "Pump", "n/a", "n/a") + signalList() +
                       deviceRecord(2, 0, "Fan", "n/a", "n/a") + signalList()));
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
    assert(at >= 1 + gap);
    owners.push_back(payload.substr(at - 1 - gap, 1));
  }
  return owners;
}

// A left-out timestamp is taken from the timestamp mode when the value is sent; 0 is a timestamp too.
static void defaultTimestamps()
{
  FakeStream stream;
  TestBlaeck device;
  device.begin(stream);
  device.setTimestampMode(BLAECK_MICROS);
  hostMillis() = 0;
  hostMicros() = 1234;
  float value = 1;
  BlaeckDeviceRef pump = device.addDevice(F("Pump"));
  pump.addSignal(F("Flow"), &value);
  device.read();
  stream.data.output.clear();
  const std::vector<int> widths = {4};

  pump.write(F("Flow"), 2.0f);
  auto frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && frames[0].timestamp == 1234);

  pump.write("Flow", 3.0f, 0ULL);
  frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && frames[0].timestamp == 0);

  device.write(0, "text", 77ULL); // a text value on a float signal is ignored
  assert(takeData(stream.data.output, widths).empty());

  hostMicros() = 5678;
  pump.writeAll();
  frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && frames[0].timestamp == 5678);
  device.writeAll(42ULL);
  frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && frames[0].timestamp == 42);

  // tick() passes its timestamp to the due interval report.
  command(device, stream, "<BLAECK.ACTIVATE,1000>");
  stream.data.output.clear();
  device.tick(99ULL);
  frames = takeData(stream.data.output, widths);
  assert(frames.size() == 1 && frames[0].timestamp == 99);
}

static void sameNamesAcrossDevices()
{
  hostMillis() = 0;
  auto handler = [](const char *, const char *const *, byte) {};
  FakeStream stream;
  Capture debug;
  TestBlaeck device;
  device.begin(stream).withDebugStream(&debug);
  BlaeckDeviceRef zoneA = device.addDevice(F("Zone A"));
  BlaeckDeviceRef zoneB = device.addDevice("Zone B");

  float boardTemp = 1, aTemp = 2, bTemp = 3;
  device.addSignal("Temperature", &boardTemp);
  zoneA.addSignal(F("Temperature"), &aTemp);
  zoneB.addSignal("Temperature", &bTemp);
  assert(device.SignalCount == 3);
  assert(device.findSignalIndex("Temperature") == 0 && zoneA.findSignalIndex(F("Temperature")) == 1 &&
         zoneB.findSignalIndex("Temperature") == 2);

  // Input and sensor names are the board's, like command names: a host sets a value by name
  // alone. A second Status is refused on any device.
  char boardStatus[8] = "board", zoneStatus[8] = "zone";
  device.addSensor(F("Status"), boardStatus, sizeof(boardStatus));
  debug.text.clear();
  zoneA.addSensor("Status", zoneStatus, sizeof(zoneStatus));
  assert(debug.text.find("Dropped 'Status': an input or sensor has the name already.") != std::string::npos);
  assert(device.hasRejections());

  // Events and their types are per device, too.
  device.addEvent(F("Alarm"), F("overheated"));
  zoneA.addEvent("Alarm", F("dry_run"));
  assert(zoneA.addEventType(F("Alarm"), F("blocked")));

  byte aSpeed = 10, bSpeed = 20;
  zoneA.addNumberInput("SpeedA", &aSpeed).withRange(0.0f, 100.0f, 1.0f);
  zoneB.addNumberInput("SpeedB", &bSpeed).withRange(0.0f, 100.0f, 1.0f);
  assert(!zoneB.addEventType(F("Alarm"), F("blocked"))); // zone B has no Alarm
  (void)handler;

  device.read();
  stream.data.output.clear();

  // Each device lists its own Temperature.
  command(device, stream, "<BLAECK.GET_DEVICES>");
  std::string payload = commandFramePayload(stream.data.output, 0xB7, 0);
  const std::string temperature = signalList({{"Temperature", 8}});
  size_t at = 0;
  for (int i = 0; i < 3; ++i)
  {
    at = payload.find(temperature, at);
    assert(at != std::string::npos);
    at += temperature.size();
  }
  stream.data.output.clear();

  command(device, stream, "<BLAECK.WRITE_ENTITIES>");
  payload = commandFramePayload(stream.data.output, 0x90, 0);
  assert(ownerOf(payload, "Status", 1) == owner(0));
  assert(ownerOf(payload, "SpeedA", 1) == owner(1) && ownerOf(payload, "SpeedB", 1) == owner(2));
  assert((ownersOf(payload, "Alarm", 1) == std::vector<std::string>{owner(0), owner(1)}));
  stream.data.output.clear();

  // Each handle reaches its own device's properties only: Status, SpeedA, SpeedB are 0, 1, 2.
  zoneB.writeProperty("SpeedA");
  assert(stream.data.output.empty());
  zoneB.writeProperty("SpeedB");
  const std::string speed = commandFramePayload(stream.data.output, 0x95, 0);
  assert(speed[0] == 2 && speed[1] == 0 && static_cast<byte>(speed.back()) == 20);
  stream.data.output.clear();

  // Zone A's Alarm is the second event, and blocked its second type.
  zoneA.writeEvent(F("Alarm"), F("blocked"));
  assert(commandFramePayload(stream.data.output, 0x85, 0) == std::string("\x01\x00\x01\x00", 4));
  stream.data.output.clear();
  debug.text.clear();
  device.writeEvent("Alarm", F("dry_run")); // a type of zone A's Alarm, not the board's
  assert(stream.data.output.empty());
  assert(debug.text.find("Event dropped, type not added with addEvent() or addEventType(): Alarm")
         != std::string::npos);

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

  // Signals are numbered and hashed in device list order, so registering zone B's signal
  // before zone A's changes nothing a host sees.
  FakeStream reorderedStream;
  TestBlaeck reordered;
  reordered.begin(reorderedStream);
  BlaeckDeviceRef reorderedA = reordered.addDevice(F("Zone A"));
  BlaeckDeviceRef reorderedB = reordered.addDevice("Zone B");
  reordered.addSignal("Temperature", &boardTemp);
  reorderedB.addSignal("Temperature", &bTemp);
  reorderedA.addSignal("Temperature", &aTemp);
  reordered.read();
  reorderedStream.data.output.clear();
  reorderedB.write("Temperature", 30.0f);
  frames = takeData(reorderedStream.data.output, widths);
  assert(frames.size() == 1 && (frames[0].ids == std::vector<int>{2}) && frames[0].schemaHash == hash);

  // Firmware that adds zone B before zone A lists them the other way round. Without the device
  // names the hash would match, and a host would file zone A's values under zone B.
  FakeStream swappedStream;
  TestBlaeck swapped;
  swapped.begin(swappedStream);
  BlaeckDeviceRef swappedB = swapped.addDevice("Zone B");
  BlaeckDeviceRef swappedA = swapped.addDevice(F("Zone A"));
  swapped.addSignal("Temperature", &boardTemp);
  swappedA.addSignal("Temperature", &aTemp);
  swappedB.addSignal("Temperature", &bTemp);
  swapped.read();
  swappedStream.data.output.clear();
  swapped.writeAll();
  frames = takeData(swappedStream.data.output, widths);
  assert(frames.size() == 1 && frames[0].schemaHash != hash);
  assert(frames[0].schemaHash ==
         crc16(std::string("Temperature") + f + "Zone B/Temperature" + f + "Zone A/Temperature" + f));

  // A board without devices hashes exactly as before: names and type codes only.
  FakeStream plainStream;
  TestBlaeck plain;
  plain.begin(plainStream);
  plain.addSignal("Temperature", &boardTemp);
  plain.read();
  plainStream.data.output.clear();
  plain.writeAll();
  frames = takeData(plainStream.data.output, {4});
  assert(frames.size() == 1 && frames[0].schemaHash == crc16(std::string("Temperature") + f));
}

// The ack of the one command in output: its status and reason.
static std::pair<byte, byte> ackOf(const std::string &output, uint32_t messageId = 0)
{
  const std::string ack = commandFramePayload(output, 0xA5, messageId);
  return {static_cast<byte>(ack[8]), static_cast<byte>(ack[9])};
}

static int propertyCallbacks = 0;
static void onPropertySet() { ++propertyCallbacks; }
static float gaugeValue = 1.5f;
static float readGauge() { return gaugeValue; }
static bool readRunning() { return true; }
static const char *readStatus() { return "ok"; }

// Inputs and sensors: the entity list, a host's writes and their checks, change reports and names.
// Events and buttons in the entity list, after the properties; a press; the 0x85 layout.
static void eventsAndButtons()
{
  FakeStream stream;
  Capture debug;
  ConfigurationProbe device;
  device.begin(stream).withDebugStream(&debug);
  float value = 0;
  device.addSensor(F("Level"), &value);
  device.addEvent(F("Door"), F("open,closed")).withIcon(F("mdi:door")).diagnostic();
  device.addButton("STATUS", onPress)
      .withDisplayName(F("Status"))
      .withDeviceClass(F("identify"))
      .config()
      .disabledByDefault();
  device.onCommand("PLAIN", onPing);
  assert(!device.hasRejections());

  stream.data.output.clear();
  device.sendEntities();
  const std::string list = commandFramePayload(stream.data.output, 0x90, 0);
  const std::string event = std::string("\x00\x01" "Door\0" "\x03\x00" "mdi:door\0" "\x02\x00" "open\0" "closed\0", 32);
  const std::string button = std::string("\x00\x02" "STATUS\0" "\x2D\x00" "Status\0" "identify\0", 27);
  assert(list.size() > event.size() + button.size());
  assert(list.substr(list.size() - event.size() - button.size()) == event + button);
  assert(list.find("PLAIN") == std::string::npos);

  // A press runs the function; parameters sent with it are ignored.
  presses = 0;
  command(device, stream, "<STATUS>");
  assert(presses == 1);
  assert(static_cast<byte>(commandFramePayload(stream.data.output, 0xA5, 0)[9]) == BLAECK_ACK_OK);
  stream.data.output.clear();
  command(device, stream, "<STATUS,1>");
  assert(presses == 2);
  stream.data.output.clear();

  // A button's name is taken for inputs and sensors; a command of that name replaces it.
  device.addSensor(F("STATUS"), &value);
  assert(device.propertyIndex("STATUS") == -1);
  assert(debug.text.find("Dropped 'STATUS': a button or command has the name already.") != std::string::npos);
  device.cleanCatalogs();
  device.onCommand("STATUS", onPing);
  assert(device.dirtyCatalogs());
  device.sendEntities();
  assert(commandFramePayload(stream.data.output, 0x90, 0).find("STATUS") == std::string::npos);
  stream.data.output.clear();

  // Without a function a button is refused.
  device.addButton("NOTHING", nullptr);
  assert(device.hasRejections());

  device.writeEvent(F("Door"), F("closed"));
  assert(commandFramePayload(stream.data.output, 0x85, 0) == std::string("\x00\x00\x01\x00", 4));
}

// long long signals and properties: DTYPE 0x0B, eight bytes, and inputs read exactly.
static long long bigGetterValue = -5;
static long long bigGetter() { return bigGetterValue; }

static void longLongValues()
{
  FakeStream stream;
  ConfigurationProbe device;
  device.begin(stream);
  device.DeviceName = "Big";
  // 2^53 + 1: a double would round it.
  long long counter = 9007199254740993LL;
  device.addSignal(F("Counter"), &counter);
  long long setpoint = 0;
  device.addNumberInput(F("Setpoint"), &setpoint);
  device.addSensor(F("Remote"), bigGetter);
  device.read();
  stream.data.output.clear();

  command(device, stream, "<BLAECK.GET_DEVICES>");
  const std::string list = commandFramePayload(stream.data.output, 0xB7, 0);
  assert(list.find(signalList({{"Counter", 0x0B}})) != std::string::npos);
  stream.data.output.clear();

  command(device, stream, "<BLAECK.WRITE_DATA>");
  auto frames = takeData(stream.data.output, {8});
  assert(frames.size() == 1 && frames[0].values[0] == std::string(reinterpret_cast<const char *>(&counter), 8));
  device.write(F("Counter"), -1LL);
  assert(counter == -1);
  stream.data.output.clear();

  const auto reason = [&]() { return static_cast<byte>(commandFramePayload(stream.data.output, 0xA5, 0)[9]); };
  command(device, stream, "<Setpoint,9223372036854775807>");
  assert(reason() == BLAECK_ACK_OK && setpoint == 9223372036854775807LL);
  stream.data.output.clear();
  command(device, stream, "<Setpoint,-9223372036854775808>");
  assert(reason() == BLAECK_ACK_OK && setpoint == (-9223372036854775807LL - 1));
  stream.data.output.clear();
  command(device, stream, "<Setpoint,9223372036854775808>");
  assert(reason() == BLAECK_ACK_OUT_OF_RANGE && setpoint == (-9223372036854775807LL - 1));
  stream.data.output.clear();
  command(device, stream, "<Setpoint,1.5>");
  assert(reason() == BLAECK_ACK_NOT_AN_INTEGER);
  stream.data.output.clear();
  command(device, stream, "<Setpoint,2e3>");
  assert(reason() == BLAECK_ACK_OK && setpoint == 2000);
  stream.data.output.clear();

  // The entity list carries the DTYPE and eight bytes of each value.
  device.sendEntities();
  const std::string entities = commandFramePayload(stream.data.output, 0x90, 0);
  const std::string remote = std::string("Remote") + '\0' + char(BLAECK_VALUE_NUMBER);
  const size_t at = entities.find(remote);
  assert(at != std::string::npos);
  assert(static_cast<byte>(entities[at + remote.size() + 4]) == 0x0B);
  assert(entities.substr(at + remote.size() + 5, 8) == std::string(reinterpret_cast<const char *>(&bigGetterValue), 8));
}

static void properties()
{
  hostMillis() = 0;
  propertyCallbacks = 0;
  FakeStream stream;
  Capture debug;
  TestBlaeck device;
  device.begin(stream).withDebugStream(&debug);

  float setpoint = 21.0f;
  byte percent = 10;
  bool enabled = false;
  byte mode = 0;
  char label[8] = "lab";
  float temperature = 20.0f;
  byte state = 1;
  device.addNumberInput(F("Setpoint"), &setpoint, onPropertySet)
      .withRange(5.0f, 30.0f, 0.1f).withUnit(F("C"));           // 0
  device.addNumberInput("Percent", &percent);                   // 1
  device.addSwitch(F("Enabled"), &enabled, onPropertySet);      // 2
  device.addSelect(F("Mode"), &mode, F("Off,Heat,Auto"));       // 3
  device.addTextInput(F("Label"), label, sizeof(label));        // 4
  device.addSensor(F("Temperature"), &temperature)
      .writeOnChange(0.5, 0);                                   // 5
  device.addSensor(F("State"), &state, F("Idle,Busy"));         // 6
  device.addSensor(F("Gauge"), readGauge);                      // 7
  device.addSensor(F("Running"), readRunning);                  // 8
  device.addSensor(F("Status"), readStatus);                    // 9
  assert(!device.hasRejections());

  // After the restart notice comes the entity list, then no 95: it carried every value.
  device.read();
  std::string list = commandFramePayload(stream.data.output, 0x90, 0);
  const auto u32 = [](uint32_t v) { return std::string(reinterpret_cast<const char *>(&v), 4); };
  const auto f32 = [](float v) { return std::string(reinterpret_cast<const char *>(&v), 4); };
  // Setpoint: board, property, name, number, READWRITE + range + step + unit, float, value,
  // min, max, step, unit.
  const std::string setpointEntry = std::string("\x00\x00", 2) + "Setpoint" + '\0' + '\x00' +
      u32(0x3 | (1UL << 2) | (1UL << 3) | (1UL << 4)) + '\x08' + f32(21.0f) + f32(5.0f) +
      f32(30.0f) + f32(0.1f) + "C" + '\0';
  assert(list.compare(0, setpointEntry.size(), setpointEntry) == 0);
  // Mode: an enum, its index as the value, then its options. Label: text, length-prefixed,
  // then its maximum length. Temperature: READ.
  assert(list.find(std::string("Mode") + '\0' + '\x02' + u32(0x3) + '\x01' + '\x00' + "Off,Heat,Auto" + '\0') !=
         std::string::npos);
  assert(list.find(std::string("Label") + '\0' + '\x03' + u32(0x3) + '\x0A' + '\x03' + "lab" + '\x07' + '\x00') !=
         std::string::npos);
  assert(list.find(std::string("Temperature") + '\0' + '\x00' + u32(0x1) + '\x08' + f32(20.0f)) !=
         std::string::npos);
  assert(list.find(std::string("Status") + '\0' + '\x03' + u32(0x1) + '\x0A' + '\x02' + "ok" + std::string("\xFF\x00", 2)) !=
         std::string::npos);
  stream.data.output.clear();
  hostMillis() = 1000;
  device.writeIfDue();
  assert(stream.data.output.empty());

  // A host's value: checked, stored, acknowledged, then the callback runs and 0x95 goes out.
  command(device, stream, "<Setpoint,22.5>");
  assert((ackOf(stream.data.output) == std::pair<byte, byte>(0, BLAECK_ACK_OK)));
  assert(setpoint == 22.5f && propertyCallbacks == 1);
  const size_t ackAt = stream.data.output.find(std::string("<blaeck:") + char(0xA5));
  const size_t pushAt = stream.data.output.find(std::string("<blaeck:") + char(0x95));
  assert(ackAt < pushAt);
  assert(commandFramePayload(stream.data.output, 0x95, 0) == std::string("\x00\x00\x08", 3) + f32(22.5f));
  stream.data.output.clear();

  // On its step: 0.1 steps from 5, so 22.3000004 is stored as 5 + 173 * 0.1.
  command(device, stream, "<Setpoint,22.3000004>");
  assert(setpoint == 5.0f + 173 * 0.1f);
  stream.data.output.clear();

  const auto refused = [&](const char *text, byte reason)
  {
    const int before = propertyCallbacks;
    command(device, stream, text);
    assert((ackOf(stream.data.output) == std::pair<byte, byte>(1, reason)));
    assert(stream.data.output.find(std::string("<blaeck:") + char(0x95)) == std::string::npos);
    assert(propertyCallbacks == before);
    stream.data.output.clear();
  };
  refused("<Setpoint,31>", BLAECK_ACK_OUT_OF_RANGE);
  refused("<Setpoint,abc>", BLAECK_ACK_OUT_OF_RANGE);
  refused("<Setpoint>", BLAECK_ACK_MISSING_VALUE);
  refused("<Setpoint,>", BLAECK_ACK_MISSING_VALUE);
  refused("<Percent,2.5>", BLAECK_ACK_NOT_AN_INTEGER);
  refused("<Percent,256>", BLAECK_ACK_OUT_OF_RANGE);
  refused("<Enabled,2>", BLAECK_ACK_BAD_SWITCH);
  refused("<Mode,Cool>", BLAECK_ACK_BAD_SELECT);
  refused("<Mode,3>", BLAECK_ACK_BAD_SELECT);
  refused("<Label,far too long>", BLAECK_ACK_TOO_LONG);
  refused("<Temperature,25>", BLAECK_ACK_READ_ONLY);
  refused("<Missing,1>", BLAECK_ACK_UNKNOWN);
  assert(setpoint == 5.0f + 173 * 0.1f && percent == 10 && !enabled && mode == 0 && strcmp(label, "lab") == 0);

  command(device, stream, "<Percent,200>");
  assert(percent == 200);
  command(device, stream, "<Enabled,1>");
  assert(enabled && propertyCallbacks == 3);
  command(device, stream, "<Mode,Auto>");
  assert(mode == 2);
  command(device, stream, "<Mode,1>");
  assert(mode == 1);
  command(device, stream, "<Label,a%2Cb>");
  assert(strcmp(label, "a,b") == 0);
  command(device, stream, "<Label,>");
  assert(label[0] == '\0');
  stream.data.output.clear();

  // Change reports: at most every 100 ms by default; Temperature only by 0.5 or more.
  hostMillis() = 2000;
  device.writeIfDue();
  assert(stream.data.output.empty()); // the writes above were sent already
  temperature = 20.3f;
  state = 0;
  gaugeValue = 2.5f;
  device.writeIfDue();
  std::string out = stream.data.output;
  assert(out.find(std::string("<blaeck:") + char(0x95)) != std::string::npos);
  assert(out.find(f32(20.3f)) == std::string::npos);            // below Temperature's delta
  assert(out.find(std::string("\x06\x00\x01\x00", 4)) != std::string::npos); // State index 0
  assert(out.find(std::string("\x07\x00\x08", 3) + f32(2.5f)) != std::string::npos); // Gauge
  stream.data.output.clear();
  temperature = 20.6f;
  gaugeValue = 3.5f;
  hostMillis() = 2050;
  device.writeIfDue();
  out = stream.data.output;
  assert(out.find(f32(20.6f)) != std::string::npos);            // no interval limit
  assert(out.find(f32(3.5f)) == std::string::npos);             // Gauge waits for 100 ms
  stream.data.output.clear();
  hostMillis() = 2100;
  device.writeIfDue();
  assert(stream.data.output.find(f32(3.5f)) != std::string::npos);
  stream.data.output.clear();

  // writeProperty() sends now, changed or not; an unknown name says so.
  device.writeProperty(F("Running"));
  assert(commandFramePayload(stream.data.output, 0x95, 0) == std::string("\x08\x00\x00\x01", 4));
  stream.data.output.clear();
  debug.text.clear();
  device.writeProperty("Nothing");
  assert(stream.data.output.empty() && debug.text.find("no property 'Nothing'") != std::string::npos);

  // Names: one target per name on the board, and none of them reserved or too long.
  const auto handler = [](const char *, const char *const *, byte) {};
  debug.text.clear();
  device.onCommand("Setpoint", handler);
  assert(debug.text.find("Dropped 'Setpoint': an input or sensor has the name already.") != std::string::npos);
  device.onCommand("RESET", handler);
  float other = 0;
  device.addSensor("RESET", &other);
  assert(debug.text.find("Dropped 'RESET': a button or command has the name already.") != std::string::npos);
  device.addSensor("BLAECK.X", &other);
  assert(debug.text.find("BLAECK. is reserved") != std::string::npos);
  device.addSensor("#1", &other);
  assert(debug.text.find("can't start with #") != std::string::npos);
  device.addSensor(std::string(80, 'n').c_str(), &other);
  assert(debug.text.find("too long to be received") != std::string::npos);
  device.addSelect("NoOptions", &mode, F(""));
  device.addTextInput("NoBuffer", label, 0);
  device.addSensor("Nothing", static_cast<float *>(nullptr));
  failAfter = 0;
  device.addSensor("NoRam", &other);
  failAfter = -1;
  assert(debug.text.find("Dropped 'NoRam': no room for another property.") != std::string::npos);
  Capture rejections;
  assert(device.printRejections(&rejections));
  assert(rejections.text.find("8 input and sensor registrations rejected.") != std::string::npos);

  // A missing device's inputs are refused like its commands.
  BlaeckDeviceRef pump = device.addDevice(F("Pump"));
  byte speed = 0;
  pump.addNumberInput("PumpSpeed", &speed);
  pump.markMissing();
  stream.data.output.clear();
  refused("<PumpSpeed,5>", BLAECK_ACK_DEVICE_NOT_RESPONDING);
  pump.markPresent();
  stream.data.output.clear();
  command(device, stream, "<PumpSpeed,5>");
  assert(speed == 5);
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
  TestBlaeck device;
  device.begin(stream);
  BlaeckDeviceRef pump = device.addDevice(F("Pump"));
  device.addDevice(F("Fan"));
  pump.onCommand("SET_PUMP_SPEED", onPing);
  device.onCommand("BOARD_PING", onPing);
  device.read();
  stream.data.output.clear();

  // A device's command is sent like the board's: its name alone finds it.
  command(device, stream, "<#7:SET_PUMP_SPEED,40>");
  assert((pings == std::vector<std::string>{"40"}));
  ackResult(stream.data.output, 7, "SET_PUMP_SPEED,40", 0);
  stream.data.output.clear();

  // '@' means nothing: "@1:#8:SET_PUMP_SPEED" is one unknown name, and nothing runs.
  command(device, stream, "<@1:#8:SET_PUMP_SPEED,41>");
  assert(pings.size() == 1);
  const std::string ack = commandFramePayload(stream.data.output, 0xA5, 0);
  assert(static_cast<byte>(ack[8]) == 1 && static_cast<byte>(ack[9]) == BLAECK_ACK_UNKNOWN);
  stream.data.output.clear();
  // So a name may start with it.
  device.onCommand("@PING", onPing);
  assert(!device.hasRejections());
  command(device, stream, "<#9:@PING,1>");
  ackResult(stream.data.output, 9, "@PING,1", 0);
  stream.data.output.clear();
  pings.pop_back();

  // While the pump is missing its command is refused and the handler doesn't run.
  pump.markMissing();
  stream.data.output.clear();
  command(device, stream, "<#11:SET_PUMP_SPEED,42>");
  assert(pings.size() == 1);
  const std::string refused = commandFramePayload(stream.data.output, 0xA5, 11);
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
  TestBlaeck device;
  device.begin(stream);
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
    device.begin(stream);
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
    const ReportingState *retained = device.reporting(0);
    const char *retainedText = device.reporting(2)->text;
    failAfter = 0;
    number.writeOnChange(BLAECK_OFF);
    boolean.writeOnChange(BLAECK_OFF);
    string.writeOnChange(BLAECK_OFF).writeOnChange(BLAECK_OFF);
    failAfter = -1;
    assert(allocations == before && !device.hasRejections());
    for (int i = 0; i < 3; ++i)
    {
      const ReportingState *r = device.reporting(i);
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
  device.begin(stream).withDebugStream(&debug);
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

  // Fill the first chunk, then fail the next one, for a rejected handle.
  for (int i = 1; i < 8; ++i)
    device.addSignal(("Filler" + std::to_string(i)).c_str(), &value);
  failAfter = 0;
  auto rejected = device.addSignal(F("Overflow"), &value);
  failAfter = -1;
  debug.text.clear();
  rejected.writeOnChange(BLAECK_OFF).writeOnChange(BLAECK_ANY_CHANGE)
      .writeOnChange(BLAECK_ALWAYS);
  assert(debug.text.empty()); // Invalid handles remain inert for both overloads.
}

static void reportingActivationSnapshot(bool buffered)
{
  hostMillis() = 0;
  FakeStream stream;
  TestBlaeck device;
  device.begin(stream);
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
  TestBlaeck device;
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
  TestBlaeck device;
  device.begin(stream).withDebugStream(&debug);
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
  TestBlaeck device;
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
  TestBlaeck device;
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
  TestBlaeck tcp;
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
  TestBlaeck device;
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
  TestBlaeck device;
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
  TestBlaeck device;
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
    TestBlaeck device;
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
  TestBlaeck device;
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
  TestBlaeck device;
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
  TestBlaeck device;
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
  TestBlaeck first;
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
#if BLAECK_ENABLE_IOT
  ordinaryConfiguration(false);
  ordinaryConfiguration(true);
  configurationAllocationFailures();
#else
  iotOff();
#endif
  flashSignalText(false);
  flashSignalText(true);
  frameEscaping(false);
  frameEscaping(true);
  flashNamesAndFailures();
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
    chunkedTables();
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
    properties();
    eventsAndButtons();
    longLongValues();
    deviceNoticesBeforeHost();
    sameNamesAcrossDevices();
    defaultTimestamps();
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
