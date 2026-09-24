/*
  WriteModes.ino

  Five signals get the same value once per second. Only the reporting policy differs:

    Periodic   the default: every host interval, even if unchanged.
    Filtered   at the host interval, only after a change of at least 0.05.
    OnChange   checked every tick, after a change of at least 0.1, at most every 100 ms.
    Combined   small changes at intervals, large changes promptly.
    Explicit   write() sends each deliberate measurement, without change filtering.

  Try this:
    Set the host's logging interval to 5000 ms, or send <BLAECK.ACTIVATE,5000>.
    Periodic and Filtered use that interval. OnChange does not need ACTIVATE.
    Combined uses both paths, with one shared last-sent value.
    Send <BLAECK.DEACTIVATE>: only interval reporting stops. OnChange, Combined's
    immediate path, and the explicit write() calls keep working.
    Automatic reporting compares current values, not a queue of intermediate samples.

  These are binary data frames, not readable text in a serial monitor.

  Leave USE_TCP at 0 for Serial, or set it to 1 for TCP. Connect Loggbok to the
  serial port at 115200 baud, or to the printed network address on TCP port 23.

  Author: Sebastian Strobl,
  More information on: https://github.com/sebaJoSt/blaeck
*/

#include <Blaeck.h>

#ifndef USE_TCP
#define USE_TCP 0  // 0: Serial, 1: TCP
#endif

#define HOST_NAME "WriteModes"

#if USE_TCP
// #define NETWORK_WITH_SERVICES  // Optional OTA and Bonjour; see WaveformGenerator/README.md.
#include "NetworkSetup.h"
NetworkSetup::Server server(23);
#endif

Blaeck device;

// Signals retain pointers to these variables, so keep them alive for the device's lifetime.
float Periodic = 0.0f;
float Filtered = 0.0f;
float OnChange = 0.0f;
float Combined = 0.0f;
float Explicit = 0.0f;

void setup()
{
  Serial.begin(115200);

#if USE_TCP
  networkBegin(23);
  server.begin();
  device.begin(server).withSignals(5)
      .withDebugStream(&device.Terminal);
#else
  device.begin(Serial).withSignals(5);
#endif

  device.DeviceName = HOST_NAME;
  device.DeviceFWVersion = "1.0";

  device.addSignal(F("Periodic"), &Periodic);
  device.addSignal(F("Filtered"), &Filtered).writeAtInterval(BLAECK_ON_CHANGE, 0.05f);
  device.addSignal(F("OnChange"), &OnChange)
      .writeAtInterval(BLAECK_OFF).writeOnChange(0.1f);
  device.addSignal(F("Combined"), &Combined)
      .writeAtInterval(BLAECK_ON_CHANGE, 0.05f).writeOnChange(0.1f);
  device.addSignal(F("Explicit"), &Explicit).writeAtInterval(BLAECK_OFF);
}

void loop()
{
  UpdateSignals();

  device.tick();
#if USE_TCP
  networkLoop();
#endif
}

void UpdateSignals()
{
  static unsigned long lastUpdate = 0;
  const unsigned long now = millis();
  if (now - lastUpdate < 1000UL)
    return;
  lastUpdate = now;

  const float value = sin(now * 0.00005f);

  Periodic = Filtered = OnChange = Combined = value;
  device.write("Explicit", value);
}
