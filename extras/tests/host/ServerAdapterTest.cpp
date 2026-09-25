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
  const std::string identity = std::string(BLAECK_VERSION) + '\0' + "blaeck" + '\0';
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
  device.writeAllData();
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
  device.writeAllData();
  assert(terminal.output.empty() && replacement.output.empty()); // No host, no frames.

  device.end();
  assert(!terminal.open && !replacement.open);
  assert(device.transportError() == Blaeck::TransportError::NotStarted);
  size_t accepts = server.accepts;
  device.read();
  assert(server.accepts == accepts);
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
    p += 2; // schema hash
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
  device.writeAllData();
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
  device.writeAllData();
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
  device.writeAllData();
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
  device.writeAllData();
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
  device.writeAllData();
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
  if (!BLAECK_TEST_REPORTING_ONLY)
  {
    diagnosticMessages();
    crc32Behavior();
    sessionBehavior(false);
    sessionBehavior(true);
    lifecycleAndErrors();
    detachFromCallbacks();
    unifiedConnections();
    beginOnlyOnce();
  }
  reportingPolicies(false);
  reportingPolicies(true);
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
