#include <Blaeck.h>
#include <Ethernet.h>

Blaeck SerialDevice;
EthernetServer server(23);
Blaeck NetworkDevice;
unsigned long Uptime = 0;

bool networkHeaderFirstCheck();

void require(bool condition, const __FlashStringHelper *message)
{
  if (condition)
    return;
  Serial.print(F("FAIL: "));
  Serial.println(message);
  while (true)
    delay(10);
}

void setup()
{
  Serial.begin(115200);
  require(SerialDevice.isBufferedWrites() == BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT,
          F("Serial buffering default"));
  require(networkHeaderFirstCheck(), F("Ethernet.h before Blaeck.h in another translation unit"));

  byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
  Ethernet.begin(mac, IPAddress(192, 168, 10, 177));
  server.begin();
  SerialDevice.begin(Serial);
  NetworkDevice.begin(server);
  require(NetworkDevice.isBufferedWrites() == BLAECK_TCP_BUFFERED_WRITES_DEFAULT,
          F("TCP buffering default after attach"));
  SerialDevice.setBufferedWrites(!BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT);
  NetworkDevice.setBufferedWrites(!BLAECK_TCP_BUFFERED_WRITES_DEFAULT);
  SerialDevice.begin(Serial).withSignals(1);
  NetworkDevice.begin(server).withClients(2).withSignals(1);
  require(SerialDevice.isBufferedWrites() != BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT,
          F("Serial begin preserved override"));
  require(NetworkDevice.isBufferedWrites() != BLAECK_TCP_BUFFERED_WRITES_DEFAULT,
          F("TCP begin preserved override"));

  SerialDevice.addSignal(F("Uptime"), &Uptime);
  NetworkDevice.addSignal(F("Uptime"), &Uptime);
  Serial.println(F("PASS: independent transports and buffering defaults"));
}

void loop()
{
  Uptime = millis() / 1000;
  SerialDevice.tick();
  NetworkDevice.tick();
}
