#pragma once

#include <Client.h>
#include <new>

namespace blaeck
{
namespace detail
{

class ServerAdapter
{
public:
  virtual ~ServerAdapter() {}
  virtual bool allocate(byte count) = 0;
  // A negative slot accepts and closes an excess connection.
  virtual bool acceptInto(int slot) = 0;
  virtual Client &client(byte slot) = 0;
  virtual void printPeer(byte slot, Print &out) = 0;
};

// Network tuning is optional; accepting clients is not.
template<class T>
auto setNoDelay(T &socket, bool enabled, int)
    -> decltype(socket.setNoDelay(enabled), void())
{
  socket.setNoDelay(enabled);
}

template<class T>
void setNoDelay(T &, bool, long) {}

template<class Server>
class TypedServerAdapter : public ServerAdapter
{
  // Deliberately requires accept(): available() has incompatible meanings across libraries.
  using Socket = decltype(static_cast<Server *>(nullptr)->accept());

public:
  TypedServerAdapter(Server &server, bool noDelay) : _server(server), _noDelay(noDelay)
  {
    setNoDelay(_server, _noDelay, 0);
  }

  ~TypedServerAdapter() override
  {
    for (byte i = 0; i < _count; ++i)
      _clients[i].stop();
    delete[] _clients;
  }

  bool allocate(byte count) override
  {
    if (_clients != nullptr)
      return count == _count;
    _clients = new (std::nothrow) Socket[count];
    if (_clients == nullptr)
      return false;
    _count = count;
    return true;
  }

  bool acceptInto(int slot) override
  {
    Socket incoming = _server.accept();
    if (!incoming)
      return false;
    if (slot < 0)
    {
      incoming.stop();
      return false;
    }
    setNoDelay(incoming, _noDelay, 0);
    _clients[slot] = incoming;
    return true;
  }

  Client &client(byte slot) override
  {
    return _clients[slot];
  }

  void printPeer(byte slot, Print &out) override
  {
    out.print(_clients[slot].remoteIP());
    out.print(':');
    out.print(_clients[slot].remotePort());
  }

private:
  Server &_server;
  Socket *_clients = nullptr;
  byte _count = 0;
  bool _noDelay;
};

} // namespace detail
} // namespace blaeck
