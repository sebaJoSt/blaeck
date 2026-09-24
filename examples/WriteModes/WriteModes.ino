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
float Immediate = 0.0f;
float Marked = 0.0f;
float Updated = 0.0f;

void setup()
{
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
  device.DeviceFWVersion = "1.0";

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
