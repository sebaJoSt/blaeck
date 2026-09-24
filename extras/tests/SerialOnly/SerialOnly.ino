#include <Blaeck.h>

Blaeck Device;
unsigned long Uptime = 0;

void setup()
{
  Serial.begin(115200);
  Device.begin(Serial).withSignals(1);
  Device.addSignal(F("Uptime"), &Uptime);
}

void loop()
{
  Uptime = millis() / 1000;
  Device.tick();
}
