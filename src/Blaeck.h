/*
        File: Blaeck.h
        Author: Sebastian Strobl

    Self-describing telemetry over a Stream or a supplied TCP server.
*/

#ifndef BLAECK_H
#define BLAECK_H

#include "BlaeckVersion.h"

#include "BlaeckCore.h"
#include "detail/BlaeckServerAdapter.h"

#ifndef BLAECK_USB_PACKET_BYTES
  #define BLAECK_USB_PACKET_BYTES 64
#endif

// Nagle's algorithm off where the server/client supports it. Set it false in
// BlaeckConfig.h to favour throughput instead.
#ifndef BLAECK_TCP_NO_DELAY_DEFAULT
  #define BLAECK_TCP_NO_DELAY_DEFAULT true
#endif

// The core's names, at global scope where sketches use them.
using namespace BLAECK_CORE_NAMESPACE;

class Blaeck;

// Returned by begin(): the core's table sizes, plus the number of connections. Each call
// returns this handle again, so withClients() can come anywhere in the chain.
class BlaeckConnectionRef : public BlaeckBeginRef
{
public:
  explicit BlaeckConnectionRef(Blaeck *owner);

  /*!
    @brief   Sets how many connections the device accepts at once.

    Hosts and terminals together. Each connection takes a receive buffer of
    BLAECK_COMMAND_MAX_CHARS_DEFAULT bytes, 128 on a Mega. A connection beyond the
    limit is closed at once. Set it before the first read(); later it is refused.

    @param   count  1 to 255. The default is 4.
    @return  The same handle, for chaining.

    @code
      device.begin(server).withClients(2);
    @endcode
  */
  BlaeckConnectionRef &withClients(byte count);

  /*!
    @brief   Sets how many signals fit in the signal table.

    Each signal takes 9 bytes of SRAM on AVR.

    @param   count  Up to 32767. A larger literal fails the build.
    @return  The same handle, for chaining.

    @code
      device.begin(server).withSignals(50);
    @endcode
  */
  BlaeckConnectionRef &withSignals(unsigned int count)
  {
    BlaeckBeginRef::withSignals(count);
    return *this;
  }

  /*!
    @brief   Sets how many state channels fit in the state channel table.

    Count the channels from addStateChannel() plus one for each command that uses
    withOwnState(). Each channel takes 26 bytes of SRAM on AVR.

    @param   count  Up to 32767. A larger literal fails the build.
    @return  The same handle, for chaining.

    @code
      device.begin(server).withStateChannels(12);
    @endcode
  */
  BlaeckConnectionRef &withStateChannels(unsigned int count)
  {
    BlaeckBeginRef::withStateChannels(count);
    return *this;
  }

  /*!
    @brief   Sets how many event channels fit in the event channel table.

    Each channel takes 10 bytes of SRAM on AVR.

    @param   count  Up to 32767. A larger literal fails the build.
    @return  The same handle, for chaining.

    @code
      device.begin(server).withEventChannels(4);
    @endcode
  */
  BlaeckConnectionRef &withEventChannels(unsigned int count)
  {
    BlaeckBeginRef::withEventChannels(count);
    return *this;
  }

  /*!
    @brief   Sets how many event types fit, counted across all channels.

    All channels share one table of types, so give the total: four channels with
    five types each need 20. Each type takes 5 bytes of SRAM on AVR.

    @param   count  Up to 32767. A larger literal fails the build.
    @return  The same handle, for chaining.

    @code
      device.begin(server).withEventChannels(4).withEventTypes(20);
    @endcode
  */
  BlaeckConnectionRef &withEventTypes(unsigned int count)
  {
    BlaeckBeginRef::withEventTypes(count);
    return *this;
  }

  /*!
    @brief   Sets how many commands fit in the command table.

    onCommand() and all the typed commands share this table. Each command takes 48
    bytes of SRAM on AVR. A command using withOwnState() also needs a state channel,
    so raise withStateChannels() to match.

    @param   count  Up to 32767. A larger literal fails the build.
    @return  The same handle, for chaining.

    @code
      device.begin(server).withCommands(8);
    @endcode
  */
  BlaeckConnectionRef &withCommands(unsigned int count)
  {
    BlaeckBeginRef::withCommands(count);
    return *this;
  }

  /*!
    @brief   Sets a stream where the library reports what it rejected and why.

    Without one, problems such as a full table show only in hasRejections().

    @param   debugStream  Where to print: a serial port, or anything else that can print,
                          such as a display.
    @return  The same handle, for chaining.

    @code
      device.begin(server).withSignals(50).withDebugStream(&device.Terminal);
    @endcode
  */
  BlaeckConnectionRef &withDebugStream(Print *debugStream);

private:
  Blaeck *_device;
};

// Text for every connected terminal: what device.Terminal is.
class BlaeckTerminal : public Print
{
public:
  explicit BlaeckTerminal(Blaeck *owner) : _owner(owner) {}

  /*!
    @brief   Sends one byte to every connected terminal.

    Everything printed to Terminal goes through this; a sketch rarely calls it directly.

    @param   b  The byte.
    @return  1.

    @code
      device.Terminal.write('.');
    @endcode
  */
  size_t write(uint8_t b) override;

  /*!
    @brief   Sends bytes to every connected terminal.

    @param   buffer  The bytes.
    @param   size    How many.
    @return  size.

    @code
      device.Terminal.write((const uint8_t *)"ok\n", 3);
    @endcode
  */
  size_t write(const uint8_t *buffer, size_t size) override;

  using Print::write;

private:
  Blaeck *_owner;
};

// One device and catalog, attached to either a Stream or a listening TCP server.
class Blaeck : public BlaeckCore
{
public:
  Blaeck();
  ~Blaeck();
  Blaeck(const Blaeck &) = delete;
  Blaeck &operator=(const Blaeck &) = delete;

  // Open the stream first. The caller retains ownership and keeps it alive until end().
  BlaeckConnectionRef begin(Stream &stream);

  /*!
    @brief   Attaches the library to an already-started TCP server.

    The sketch owns the server, starts it and keeps it alive until end(). Do not let
    another consumer accept from the same server. A connection becomes a host when it sends
    a BLAECK. command, such as <BLAECK.GET_DEVICES>, and receives frames from then on.
    Every other connection is a terminal: it receives the text sent to Terminal, and
    its commands run but aren't answered.

    @param   server  A server with accept() returning a Client-derived value.
    @return  A handle for setting the number of connections, table sizes and a debug
             stream. Each has a default, so the handle can be ignored.

    @code
      server.begin();
      device.begin(server)
          .withClients(4)
          .withSignals(50)
          .withDebugStream(&device.Terminal);
    @endcode
  */
  template<class Server>
  auto begin(Server &server) -> decltype(server.accept(), BlaeckConnectionRef(this))
  {
    end();
    _beginCore();
    _tcpSelected = true;
    _setBufferedWritesDefault(BLAECK_TCP_BUFFERED_WRITES_DEFAULT);
    _adapter = new (std::nothrow) blaeck::detail::TypedServerAdapter<Server>(
        server, BLAECK_TCP_NO_DELAY_DEFAULT);
    _setTransportError(_adapter != nullptr ? TransportError::None : TransportError::OutOfMemory);
    return BlaeckConnectionRef(this);
  }

  // Migration guard: start a server yourself, then pass that object rather than its port.
  BlaeckConnectionRef begin(uint16_t port) = delete;

  // Detaches a stream or closes accepted clients; never stops the caller's stream/server.
  // Also called by the destructor and before attaching to another server.
  void end();

  enum class TransportError : byte
  {
    None,
    NotStarted,
    OutOfMemory,
    InvalidClientCount,
    ClientLimitLocked,
    NotServer
  };

  TransportError transportError() const { return _transportError; }
  bool printTransportError(Print *out) const;

  /*!
    @brief   Text to the attached Stream, or to every connected TCP terminal.

    TCP hosts never receive it. Pass it to withDebugStream() to see on a terminal what
    the library refuses and which commands arrive.

    @note    A terminal that connects but never reads can fill its send buffer, and a
             write to it may then wait. Short lines don't get there.

    @code
      device.Terminal.println("LED is ON.");
    @endcode
  */
  BlaeckTerminal Terminal;

  /*!
    @brief   Sets a function to call when a connection opens.

    @param   callback  Receives the connection's slot, starting at 0.

    @code
      device.setClientConnectedCallback(onClientConnected);
    @endcode
  */
  void setClientConnectedCallback(void (*callback)(byte clientNo));

  /*!
    @brief   Sets a function to call when a connection closes.

    A connection that dies without closing, such as one whose cable was pulled, is
    noticed only when the network stack gives up on it.

    @param   callback  Receives the connection's slot, starting at 0.

    @code
      device.setClientDisconnectedCallback(onClientDisconnected);
    @endcode
  */
  void setClientDisconnectedCallback(void (*callback)(byte clientNo));

protected:
  bool _transportReady() const override;
  void _writeDirect(const byte *data, size_t len) override;
  void _flushDirect() override;
  void _sendBuffered() override;
  bool _receiveCommand() override;
  void _builtinCommandReceived() override;
  // Wire identities are intentionally unchanged until the host migration is addressed.
  const char *_libraryName() const override { return _tcpSelected ? "BlaeckTCP" : "BlaeckSerial"; }
  const char *_libraryVersion() const override { return BLAECK_VERSION; }

private:
  struct Connection
  {
    Receiver receiver;
    // The slot holds a connection. Tracked here because a client's own bool means
    // "connected" on some cores and "has a socket" on others.
    bool open = false;
    bool host = false;
  };

  // Allocated by the first read(), so withClients() on the begin() chain can size it.
  Connection *_connections = nullptr;
  Stream *_stream = nullptr;
  bool _tcpSelected = false;
  blaeck::detail::ServerAdapter *_adapter = nullptr;
  TransportError _transportError = TransportError::NotStarted;
  bool _transportErrorReported = false;
  byte _maxClients = 4;
  // The connection whose command is being handled; acks and answers go only there.
  byte _requester = 0;

  void (*_connectedCallback)(byte clientNo) = nullptr;
  void (*_disconnectedCallback)(byte clientNo) = nullptr;

  bool _ensureConnections();
  void _sendStreamBuffered();
  void _acceptConnection();
  void _dropClosedConnections();
  // Whether the frame being written goes to this connection.
  bool _receivesFrame(byte slot) const;
  void _setMaxClients(byte count);
  void _setTransportError(TransportError error);
  void _reportTransportError();

  friend class BlaeckConnectionRef;
  friend class BlaeckTerminal;
};

inline BlaeckConnectionRef::BlaeckConnectionRef(Blaeck *owner) : BlaeckBeginRef(owner), _device(owner) {}

inline BlaeckConnectionRef &BlaeckConnectionRef::withClients(byte count)
{
  if (_device != nullptr)
    _device->_setMaxClients(count);
  return *this;
}

inline BlaeckConnectionRef &BlaeckConnectionRef::withDebugStream(Print *debugStream)
{
  BlaeckBeginRef::withDebugStream(debugStream);
  if (_device != nullptr)
    _device->_reportTransportError();
  return *this;
}

#endif // BLAECK_H
