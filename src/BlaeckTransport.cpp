#include "Blaeck.h"

namespace blaeck
{

BlaeckBeginRef Blaeck::begin(Stream &stream)
{
  end();
  _resetSignalCatalog();
  _setBufferedWritesDefault(BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT);
  _stream = &stream;
  _setTransportError(TransportError::None);
  return BlaeckBeginRef(this);
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

bool Blaeck::_receivesFrame(byte slot) const
{
  const Connection &c = _connections[slot];
  if (!c.open || !c.host || !_adapter->client(slot).connected())
    return false;
  return _frameAudience != AUDIENCE_REQUESTER || slot == _requester;
}

bool Blaeck::_transportReady() const
{
  if (_stream != nullptr)
    return true;
  if (_connections == nullptr)
    return false;
  for (byte i = 0; i < _maxClients; i++)
  {
    const Connection &c = _connections[i];
    if (c.open && c.host && _adapter->client(i).connected())
      return true;
  }
  return false;
}

void Blaeck::_writeDirect(const byte *data, size_t len)
{
  if (_stream != nullptr)
  {
    _stream->write(data, len);
    return;
  }
  for (byte i = 0; i < _maxClients; i++)
  {
    if (_receivesFrame(i))
      _adapter->client(i).write(data, len);
  }
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
  for (byte i = 0; i < _maxClients; i++)
  {
    if (_receivesFrame(i))
      _adapter->client(i).write(_frameBuf, _framePos);
  }
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
  _stream->write(_frameBuf, _framePos);
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
    c.host = false;
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
    Connection &c = _connections[i];
    if (!c.open || _adapter->client(i).connected())
      continue;

    _adapter->client(i).stop();
    c.open = false;
    c.host = false;
    c.receiver = Receiver();
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Client #"));
      _debugStream->print(i);
      _debugStream->println(F(" disconnected"));
    }
    if (_disconnectedCallback != nullptr)
      _disconnectedCallback(i);
    if (_connections == nullptr)
      return;
  }
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

void Blaeck::_builtinCommandReceived()
{
  if (_stream != nullptr)
    return;
  Connection &c = _connections[_requester];
  if (c.host)
    return;

  c.host = true;
  if (_debugStream != nullptr)
  {
    _debugStream->print(F("Client #"));
    _debugStream->print(_requester);
    _debugStream->println(F(" is a host"));
  }
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
    Blaeck::Connection &c = _owner->_connections[i];
    if (c.open && !c.host && _owner->_adapter->client(i).connected())
      _owner->_adapter->client(i).write(buffer, size);
  }
  return size;
}

} // namespace blaeck
