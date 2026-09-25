#include <Ethernet.h>
#include <Blaeck.h>

bool networkHeaderFirstCheck()
{
  EthernetServer server(24);
  Blaeck serialDevice;
  Blaeck tcpDevice;
  tcpDevice.begin(server);
  return serialDevice.isBufferedWrites() == BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT
      && tcpDevice.isBufferedWrites() == BLAECK_TCP_BUFFERED_WRITES_DEFAULT;
}
