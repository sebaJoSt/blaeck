#include <Blaeck.h>
#include <cassert>
#include <deque>
#include <iostream>
#include <type_traits>
#include <utility>
#include <vector>

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
  unsigned int stops = 0;
  std::string input;
  std::string output;
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
    state->output.append(reinterpret_cast<const char *>(data), size);
    return size;
  }
  void setNoDelay(bool enabled) { if (state) state->noDelay = enabled; }
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
  float value = 12.5f;
  device.addSignal(F("Value"), &value);
  device.read();
  device.read();
  device.read();
  assert((opened == std::vector<byte>{0, 1, 2}));
  assert(host1.noDelay == BLAECK_TCP_NO_DELAY_DEFAULT);
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
  host1.output.clear();
  host2.input = "<BLAECK.GET_DEVICES>";
  device.read();
  assert(host1.output.empty() && !host2.output.empty() && terminal.output.empty());
  host2.output.clear();
  device.Terminal.print("terminal");
  assert(host1.output.empty() && host2.output.empty() && terminal.output == "terminal");
  terminal.output.clear();
  device.writeAllData();
  assert(!host1.output.empty() && host1.output == host2.output && terminal.output.empty());

  host1.output.clear();
  host2.output.clear();
  host1.input = "<BLAECK.GET_";
  host2.input = "<BLAECK.GET_DEVICES><BLAECK.GET_DEVICES>";
  device.read();
  assert(host1.output.empty() && !host2.output.empty());
  host2.output.clear();
  host1.input += "DEVICES>";
  device.read();
  assert(!host1.output.empty() && host2.output.empty()); // Fairness and independent parsers.
  assert(host2.input == "<BLAECK.GET_DEVICES>");
  device.read();

  server.pending.push_back(&excess);
  device.read();
  assert(!excess.open && excess.stops == 1 && opened.size() == 3);
  host1.open = false;
  device.read();
  assert((closed == std::vector<byte>{0}));
  size_t before = allocations;
  server.pending.push_back(&replacement);
  device.read();
  assert(allocations == before); // No adapter/client-array allocation per connection.
  assert(opened.back() == 0);
  device.Terminal.print("new terminal");
  assert(replacement.output == "new terminal"); // Reused slot has no old host state.

  device.end();
  assert(!host2.open && !terminal.open && !replacement.open);
  assert(device.transportError() == Blaeck::TransportError::NotStarted);
  size_t accepts = server.accepts;
  device.read();
  assert(server.accepts == accepts);
}

static void lifecycleAndErrors()
{
  PlainServer first;
  FakeServer<> second, third;
  SocketState a, b, c;
  Capture debug;
  {
    Blaeck one, two;
    first.pending.push_back(&a);
    second.pending.push_back(&b);
    one.begin(first).withClients(1);
    two.begin(second).withClients(1);
    one.read();
    two.read();
    one.Terminal.print("one");
    two.Terminal.print("two");
    assert(a.output == "one" && b.output == "two");
    auto handle = one.begin(third).withClients(1).withDebugStream(&debug);
    assert(!a.open && b.open);
    third.pending.push_back(&c);
    one.read();
    handle.withClients(2);
    assert(one.transportError() == Blaeck::TransportError::ClientLimitLocked);
    assert(debug.text.find("already set up") != std::string::npos);
  }
  assert(!b.open && !c.open);

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
    assert(device.transportError() == Blaeck::TransportError::None);
  }
  Blaeck device;
  device.begin(first).withClients(0).withDebugStream(&debug);
  assert(device.transportError() == Blaeck::TransportError::InvalidClientCount);
  device.begin(first).withClients(255);
  device.read(); // Regression: byte-sized round-robin counter would loop forever at 255.
  assert(device.transportError() == Blaeck::TransportError::None);
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
  device.setClientConnectedCallback(nullptr);
  device.setClientDisconnectedCallback(detachInCallback);
  device.begin(server);
  server.pending.push_back(&b);
  device.read();
  b.open = false;
  device.read();
  assert(device.transportError() == Blaeck::TransportError::NotStarted);
}

static void unifiedConnections()
{
  FakeStream stream;
  FakeServer<> server;
  SocketState host;
  Blaeck device;
  size_t before = allocations;
  device.begin(stream);
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

  stream.data.input = "<BLAECK.GET_";
  device.read();
  device.begin(server).withClients(1);
  assert(device.isBufferedWrites() == BLAECK_TCP_BUFFERED_WRITES_DEFAULT);
  assert(stream.data.open);
  server.pending.push_back(&host);
  device.read();
  device.begin(stream);
  assert(!host.open && stream.data.open);
  assert(device.isBufferedWrites() == BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT);
  stream.data.output.clear();
  stream.data.input = "DEVICES>";
  device.read();
  assert(stream.data.output.empty()); // A previous attachment's partial command was discarded.

  device.setBufferedWrites(false);
  device.begin(server);
  assert(!device.isBufferedWrites());
  device.begin(stream);
  assert(!device.isBufferedWrites());
  device.setBufferedWrites(true);
  device.begin(server);
  device.begin(stream);
  assert(device.isBufferedWrites());
  auto handle = device.begin(stream);
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

int main()
{
  diagnosticMessages();
  crc32Behavior();
  sessionBehavior(false);
  sessionBehavior(true);
  lifecycleAndErrors();
  detachFromCallbacks();
  unifiedConnections();
  std::cout << "PASS: CRC32, unified connections, wire identities, routing, lifecycle and allocation failures\n";
}
