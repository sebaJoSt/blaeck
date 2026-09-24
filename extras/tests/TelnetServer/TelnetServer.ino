#include <Blaeck.h>
#if !defined(ESP32) && !defined(ESP8266)
#include <Ethernet.h>
#endif
#include <TelnetPrint.h>
#include <Blaeck.h>

Blaeck device;
unsigned long Uptime;

void setup()
{
  // Compile fixture: actual use must bring the network up before starting the server.
  TelnetPrint.begin();
  device.begin(TelnetPrint).withClients(2).withSignals(1);
  device.addSignal(F("Uptime"), &Uptime);
}

void loop()
{
  Uptime = millis() / 1000;
  device.tick();
}
