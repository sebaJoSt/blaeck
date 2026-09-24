#include <Ethernet.h>
#include <Blaeck.h>

bool tcpHeaderFirstCheck()
{
  EthernetServer server(24);
  Blaeck serialDevice;
  Blaeck tcpDevice;
  tcpDevice.begin(server);
  blaeck::BlaeckCore *serialCore = &serialDevice;
  blaeck::BlaeckCore *tcpCore = &tcpDevice;
  return serialCore->isBufferedWrites() == BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT
      && tcpCore->isBufferedWrites() == BLAECK_TCP_BUFFERED_WRITES_DEFAULT;
}
