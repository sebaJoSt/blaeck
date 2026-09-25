#include "Blaeck.h"

namespace blaeck
{

BlaeckBeginRef Blaeck::begin(Stream &stream)
{
  if (!_beginOnce())
    return BlaeckBeginRef(nullptr);
  _resetSignalCatalog();
  _setBufferedWritesDefault(BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT);
  _stream = &stream;
  _setTransportError(TransportError::None);
  return BlaeckBeginRef(this);
}

bool Blaeck::_beginOnce()
{
  if (_beginCalled)
  {
    _setTransportError(TransportError::BeginAlreadyCalled);
    return false;
  }
  _beginCalled = true;
  return true;
}

void Blaeck::end()
{
  delete _adapter;
  _adapter = nullptr;
  delete[] _connections;
  _connections = nullptr;
  _stream = nullptr;
  _tcpSelected = false;
  _receiver = Receiver();
  _requester = 0;
  _hostSlot = NO_HOST;
  _transportError = TransportError::NotStarted;
  _transportErrorReported = false;
}

void Blaeck::setClientConnectedCallback(void (*callback)(byte clientNo))
{
  _connectedCallback = callback;
}

void Blaeck::setClientDisconnectedCallback(void (*callback)(byte clientNo))
{
  _disconnectedCallback = callback;
}

void Blaeck::_setMaxClients(byte count)
{
  if (!_tcpSelected)
  {
    _setTransportError(TransportError::NotServer);
    return;
  }
  if (_connections != nullptr)
  {
    _setTransportError(TransportError::ClientLimitLocked);
    return;
  }
  if (count == 0)
  {
    _setTransportError(TransportError::InvalidClientCount);
    return;
  }
  _maxClients = count;
}

bool Blaeck::printTransportError(Print *out) const
{
  if (out == nullptr || _transportError == TransportError::None)
    return false;
  out->print(F("Blaeck: "));
  switch (_transportError)
  {
  case TransportError::NotStarted:
    if (_beginCalled)
      out->println(F("transport ended; begin() cannot be called again on this instance."));
    else
      out->println(F("call begin(stream) or begin(server) before read() or tick()."));
    break;
  case TransportError::OutOfMemory:
    out->println(F("transport allocation failed; reduce client count in setup()."));
    break;
  case TransportError::InvalidClientCount:
    out->println(F("withClients() requires 1 to 255; previous limit retained."));
    break;
  case TransportError::ClientLimitLocked:
    out->println(F("withClients() ignored: the connections are already set up."));
    break;
  case TransportError::NotServer:
    out->println(F("withClients() requires begin(server), not a Stream."));
    break;
  case TransportError::BeginAlreadyCalled:
    out->println(F("begin() may be called only once per instance, even after end(); call ignored."));
    break;
  case TransportError::None:
    break;
  }
  return true;
}

void Blaeck::_setTransportError(TransportError error)
{
  if (_transportError == TransportError::OutOfMemory)
    return;
  _transportError = error;
  _transportErrorReported = false;
  _reportTransportError();
}

void Blaeck::_reportTransportError()
{
  if (!_transportErrorReported && _debugStream != nullptr
      && (_debugStream != &Terminal || _connections != nullptr || _stream != nullptr))
    _transportErrorReported = printTransportError(_debugStream);
}

bool Blaeck::_ensureConnections()
{
  _reportTransportError();
  if (_adapter == nullptr || _transportError == TransportError::OutOfMemory)
    return false;
  if (_connections != nullptr)
    return true;

  Connection *connections = new (std::nothrow) Connection[_maxClients];
  if (connections == nullptr || !_adapter->allocate(_maxClients))
  {
    delete[] connections;
    _setTransportError(TransportError::OutOfMemory);
    return false;
  }
  _connections = connections;
  return true;
}

// ----- Frames out -----

// Frames go to the host alone. Terminals only receive Terminal text.
bool Blaeck::_hostConnected() const
{
  return _connections != nullptr && _hostSlot != NO_HOST && _adapter->client(_hostSlot).connected();
}

bool Blaeck::_transportReady() const
{
  return _stream != nullptr || _hostConnected();
}

bool Blaeck::_requesterIsHost() const
{
  return _stream != nullptr || (_connections != nullptr && _requester == _hostSlot);
}

void Blaeck::_writeDirect(const byte *data, size_t len)
{
  if (_stream != nullptr)
  {
    if (_stream->write(data, len) != len)
      _frameWriteFailed = true;
    return;
  }
  if (!_hostConnected() || _adapter->client(_hostSlot).write(data, len) != len)
    _frameWriteFailed = true;
}

// Nothing to do: a TCP client has no send buffer to push out, and on older ESP32 cores
// flush() threw away received data instead.
void Blaeck::_flushDirect()
{
  if (_stream != nullptr)
    _stream->flush();
}

void Blaeck::_sendBuffered()
{
  if (_stream != nullptr)
  {
    _sendStreamBuffered();
    return;
  }
  if (!_hostConnected() ||
      _adapter->client(_hostSlot).write(_frameBuf, _framePos) != static_cast<size_t>(_framePos))
    _frameWriteFailed = true;
}

void Blaeck::_sendStreamBuffered()
{
  // USB bulk transfers need a short final packet. Padding is outside the protocol frame.
  bool padded = false;
  if (_framePos > 0 && (_framePos % BLAECK_USB_PACKET_BYTES) == 0 && _bufEnsure(1))
  {
    _frameBuf[_framePos++] = '\n';
    padded = true;
  }
  if (_stream->write(_frameBuf, _framePos) != static_cast<size_t>(_framePos))
    _frameWriteFailed = true;
  if (!padded && _framePos > 0 && (_framePos % BLAECK_USB_PACKET_BYTES) == 0)
    _stream->write('\n');
  _stream->flush();
}

// ----- Connections and commands in -----

void Blaeck::_acceptConnection()
{
  for (byte i = 0; i < _maxClients; i++)
  {
    Connection &c = _connections[i];
    if (c.open)
      continue;

    if (!_adapter->acceptInto(i))
      return;
    c.open = true;
    c.receiver = Receiver();
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Client #"));
      _debugStream->print(i);
      _debugStream->print(F(" connected: "));
      _adapter->printPeer(i, *_debugStream);
      _debugStream->println();
    }
    if (_connectedCallback != nullptr)
      _connectedCallback(i);
    return;
  }

  // Every slot is taken.
  _adapter->acceptInto(-1);
}

void Blaeck::_dropClosedConnections()
{
  for (byte i = 0; i < _maxClients; i++)
  {
    if (!_connections[i].open || _adapter->client(i).connected())
      continue;

    _releaseConnection(i);
    _announceDisconnect(i, nullptr);
    if (_connections == nullptr)
      return;
  }
}

void Blaeck::_releaseConnection(byte slot)
{
  Connection &c = _connections[slot];
  _adapter->client(slot).stop();
  c.open = false;
  c.receiver = Receiver();
  if (_hostSlot == slot)
    _hostSlot = NO_HOST;
}

// The callback may call end(), so callers must check _connections afterwards.
void Blaeck::_announceDisconnect(byte slot, const __FlashStringHelper *reason)
{
  if (_debugStream != nullptr)
  {
    _debugStream->print(F("Client #"));
    _debugStream->print(slot);
    _debugStream->print(F(" disconnected"));
    if (reason != nullptr)
    {
      _debugStream->print(F(": "));
      _debugStream->print(reason);
    }
    _debugStream->println();
  }
  if (_disconnectedCallback != nullptr)
    _disconnectedCallback(slot);
}

bool Blaeck::_receiveCommand()
{
  if (_stream != nullptr)
  {
    _reportTransportError();
    while (_stream->available() > 0)
    {
      if (_receiveByte(_receiver, (char)_stream->read()))
        return true;
    }
    return false;
  }
  if (!_ensureConnections())
    return false;

  _acceptConnection();
  if (_connections == nullptr)
    return false;
  _dropClosedConnections();
  if (_connections == nullptr)
    return false;

  // Start after the connection served last, so one busy connection can't starve the rest.
  for (unsigned int k = 1; k <= _maxClients; k++)
  {
    byte i = (byte)((_requester + k) % _maxClients);
    Connection &c = _connections[i];
    if (!c.open)
      continue;

    // One byte at a time, so whatever follows a complete command stays in the socket for
    // the next read().
    Client &client = _adapter->client(i);
    while (client.available() > 0)
    {
      if (_receiveByte(c.receiver, (char)client.read()))
      {
        _receiver = c.receiver;
        _requester = i;
        return true;
      }
    }
  }
  return false;
}

// One host at a time, as on a serial port. The newest takes over and the previous one is
// closed: a host that died without closing would otherwise hold the role until the network
// stack gave up on it, and a live one learns from the close that it was replaced.
void Blaeck::_builtinCommandReceived()
{
  if (_stream != nullptr || _hostSlot == _requester)
    return;

  const byte previous = _hostSlot;
  _hostSlot = _requester;
  // Released before anything is printed, so neither host receives the lines as Terminal text.
  if (previous != NO_HOST)
    _releaseConnection(previous);
  _resetReportingBaselines();
  if (_debugStream != nullptr)
  {
    _debugStream->print(F("Client #"));
    _debugStream->print(_requester);
    _debugStream->println(F(" is the host"));
  }
  if (previous != NO_HOST)
    _announceDisconnect(previous, F("replaced as host"));
}

// ----- Terminal -----

size_t BlaeckTerminal::write(uint8_t b)
{
  return write(&b, 1);
}

size_t BlaeckTerminal::write(const uint8_t *buffer, size_t size)
{
  if (_owner->_stream != nullptr)
    return _owner->_stream->write(buffer, size);
  if (_owner->_connections == nullptr)
    return size;
  for (byte i = 0; i < _owner->_maxClients; i++)
  {
    const Blaeck::Connection &c = _owner->_connections[i];
    if (c.open && i != _owner->_hostSlot && _owner->_adapter->client(i).connected())
      _owner->_adapter->client(i).write(buffer, size);
  }
  return size;
}

} // namespace blaeck
