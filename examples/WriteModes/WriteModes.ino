/*
  WriteModes.ino

  Four signals get the same value every 100 ms; only how they report it differs:

    Interval            every host interval, changed or not (the default).
    OnChangeAtInterval  at the host interval, if it changed by at least 0.01.
    OnChange            as soon as it changes by at least 0.01; needs no interval.
    Explicit            write() sends every value.

  The value is a 12-second sine wave around 1.8 V, in a 36-second pattern:
    0-8 s    amplitude 0.8, with two short spikes at 5 and 6 s, peaking near 5 V
    8-12 s   fades out
    12-30 s  flat, flickering by one 10-bit ADC step (5 V / 1024, about 0.005)
    30-36 s  fades back in

  Try this:
    Set the host's logging interval to 2000 ms, or send <BLAECK.INTERVAL_START,2000>.
    Interval and OnChangeAtInterval only check every 2 s, so they can miss the
    spikes; OnChange and Explicit catch them.
    While flat, the flicker stays below 0.01: OnChangeAtInterval and OnChange
    go quiet, while Interval and Explicit keep sending.
    Send <BLAECK.INTERVAL_STOP>: only Interval and OnChangeAtInterval stop.

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
float interval = 0.0f;
float onChangeAtInterval = 0.0f;
float onChange = 0.0f;
float explicitValue = 0.0f;

void setup()
{
  Serial.begin(115200);

#if USE_TCP
  networkBegin(23);
  server.begin();
  device.begin(server)
      .withDebugStream(&device.Terminal);
#else
  device.begin(Serial);
#endif

  device.withName(HOST_NAME);
  device.withFWVersion(F("1.0"));

  device.addSignal(F("Interval"), &interval);
  device.addSignal(F("OnChangeAtInterval"), &onChangeAtInterval)
      .writeAtInterval(BLAECK_ON_CHANGE, 0.01f);
  device.addSignal(F("OnChange"), &onChange)
      .writeAtInterval(BLAECK_OFF).writeOnChange(0.01f, 0);
  device.addSignal(F("Explicit"), &explicitValue).writeAtInterval(BLAECK_OFF);
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
  if (now - lastUpdate < 100UL)
    return;
  lastUpdate = now;

  const unsigned long phaseMs = now % 36000UL;
  float amplitude = 0.8f;
  if (phaseMs >= 30000UL)
    amplitude = 0.4f * (1.0f - cos(PI * ((phaseMs - 30000UL) / 6000.0f)));
  else if (phaseMs >= 12000UL)
    amplitude = 0.0f;
  else if (phaseMs >= 8000UL)
    amplitude = 0.4f * (1.0f + cos(PI * ((phaseMs - 8000UL) / 4000.0f)));

  // While flat, flicker by one step of a 10-bit ADC at 5 V.
  const float adcStep = 5.0f / 1024.0f;
  const float jitter = amplitude == 0.0f ? random(2) * adcStep : 0.0f;

  const float offset = 1.8f;
  float value = offset + amplitude * sin((phaseMs % 12000UL) * (TWO_PI / 12000.0f)) + jitter;
  if ((phaseMs >= 5000UL && phaseMs < 5600UL) ||
      (phaseMs >= 6000UL && phaseMs < 6600UL))
  {
    const float spikePhase = (phaseMs % 1000UL) / 600.0f;
    value += 1.45f * (1.0f - cos(TWO_PI * spikePhase));
  }

  interval = onChangeAtInterval = onChange = value;
  device.write("Explicit", value);
}
