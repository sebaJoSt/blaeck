/*
  StateChannels.ino

  State channels are shown but never logged. This example uses one per value source:
    Temperature  pointer         reads a variable the sketch keeps
    Running      getter          derives a value from the current operating phase
    LastError    explicit write  has no value until a fault is reported

  Temperature and Running are pushed every two seconds with writeState(name).
  LastError is sent only when a fault occurs, with writeState(name, value).
  These approaches work with text, numbers and booleans, not just the types shown here.

  The simulation repeats normal -> fault -> recovery, ten seconds per phase.
  Running is true only during normal operation. LastError reports the last fault,
  not the current status. Uptime is the only signal, so it alone is logged.

  Author: Sebastian Strobl, https://github.com/sebaJoSt/BlaeckSerial
*/

#include <Blaeck.h>
#define HOST_NAME "StateChannels"
#ifndef USE_TCP
#define USE_TCP 0  // 0: Serial, 1: TCP
#endif

#if USE_TCP
// Uncomment to enable OTA/Bonjour; see WaveformGenerator/README.md.
// #define NETWORK_WITH_SERVICES
#include "NetworkSetup.h"
NetworkSetup::Server server(23);
#endif

Blaeck device;

unsigned long Uptime = 0;
float Temperature = 20.0f;

enum class Phase { Normal, Fault, Recovery };
Phase CurrentPhase = Phase::Normal;

// Getters run while a frame is assembled: compute a value, but do not send anything.
bool isRunning()
{
  return CurrentPhase == Phase::Normal;
}

void setup()
{

  Serial.begin(115200);

#if USE_TCP
  networkBegin(23);
  server.begin();
  device.begin(server)
      .withSignals(1)
      .withStateChannels(3)
      .withDebugStream(&device.Terminal);
#else
  device.begin(Serial)
      .withSignals(1)
      .withStateChannels(3)
      .withDebugStream(&Serial);
#endif

  device.DeviceName = HOST_NAME;
  device.DeviceFWVersion = "1.0";

  device.addSignal(F("Uptime"), &Uptime).withUnit(F("s"));

  // Pointer: Temperature must remain alive for as long as the channel uses it.
  device.addStateChannel(F("Temperature"), &Temperature)
      .withUnit(F("\xC2\xB0" "C"))
      .withDeviceClass(F("temperature"))
      .withDisplayPrecision(1);

  // Getter: evaluated when read, but still needs a push to notify the host of changes.
  device.addStateChannel(F("Running"), BlaeckBool)
      .withStateValue(isRunning);

  // Type tag only: unlike a pointer, this can represent "no value yet".
  device.addStateChannel(F("LastError"), BlaeckText)
      .withIcon(F("mdi:alert-circle"))
      .diagnostic();

  device.printRejections(&Serial);
}

void loop()
{
  Uptime = millis() / 1000;
  UpdateSimulation();
  device.tick();
#if USE_TCP
  networkLoop();
#endif
}

void UpdateSimulation()
{
  static unsigned long lastUpdate = 0;
  static unsigned long lastPhase = 0;
  const unsigned long now = millis();
  if (now - lastUpdate < 2000UL)
    return;
  lastUpdate = now;

  if (now - lastPhase >= 10000UL)
  {
    lastPhase = now;
    switch (CurrentPhase)
    {
    case Phase::Normal:
      CurrentPhase = Phase::Fault;
      break;
    case Phase::Fault:
      CurrentPhase = Phase::Recovery;
      break;
    case Phase::Recovery:
      CurrentPhase = Phase::Normal;
      break;
    }

    if (CurrentPhase == Phase::Fault)
    {
      // The local text is copied into the frame before writeState() returns.
      char error[40];
      snprintf(error, sizeof(error), "simulated fault after %lu s", Uptime);
      device.writeState(F("LastError"), error);
    }
  }

  Temperature += ((isRunning() ? 21.0f : 18.0f) - Temperature) * 0.25f;
  device.writeState(F("Temperature"));
  device.writeState(F("Running"));
}
