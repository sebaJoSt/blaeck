/*
  WriteModes.ino

  Three signals get the same value once per second. Only the sending method differs:

    Immediate  write() stores the value and sends it immediately.
    Marked     assign the variable, then markSignalUpdated() flags it for sending.
    Updated    update() stores the value and flags it in one call.

  Marked and Updated behave identically. tickUpdated() sends the latest values of flagged
  signals on the host's interval; intermediate values are not queued.

  Try this:
    Set the host's logging interval to 5000 ms, or send <BLAECK.ACTIVATE,5000>.
    Immediate sends each new value once per second. After the initial interval-driven
    response, Marked and Updated send their latest values every five seconds.
    Send <BLAECK.DEACTIVATE>: interval-driven sends stop, but the explicit write() calls
    keep sending Immediate. Deactivation does not stop the sketch's own writes.

  These are binary data frames, not readable text in a serial monitor.

  Author: Sebastian Strobl,
  More information on: https://github.com/sebaJoSt/BlaeckSerial
*/

#include <Blaeck.h>
#define HOST_NAME "WriteModes"
#ifndef USE_TCP
#define USE_TCP 0  // 0: Serial, 1: TCP
#endif

#if USE_TCP
// Uncomment to enable OTA/Bonjour; see WaveformGenerator/README.md.
// #define NETWORK_WITH_SERVICES
#include "NetworkSetup.h"
NetworkSetup::Server server(23);
#endif

#define ExampleVersion "1.0"

// Instantiate a new Blaeck object
Blaeck device;

// Signals
float Immediate = 0.0f;
float Marked = 0.0f;
float Updated = 0.0f;

void setup()
{
  // Setup Blaeck
  Serial.begin(115200);

#if USE_TCP
  networkBegin(23);
  server.begin();
  device.begin(server).withSignals(3)
      .withDebugStream(&device.Terminal);
#else
  device.begin(Serial).withSignals(3);
#endif

  device.DeviceName = HOST_NAME;
  device.DeviceFWVersion = ExampleVersion;

  // Add signals to Blaeck
  device.addSignal(F("Immediate"), &Immediate);
  device.addSignal(F("Marked"), &Marked);
  device.addSignal(F("Updated"), &Updated);
}

void loop()
{
  UpdateSignals();

  // Reads what has come in and sends the signals marked above.
  device.tickUpdated();
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

  device.write("Immediate", value);

  Marked = value;
  device.markSignalUpdated("Marked");

  device.update("Updated", value);
}
