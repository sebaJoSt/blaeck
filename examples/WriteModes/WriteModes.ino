/*
  WriteModes.ino

  Five signals get the same value every 100 ms: a smooth 12-second sine wave
  from -0.8 to +0.8. Two rounded spikes add up to 4 at 5.0-5.6 and 6.0-6.6
  seconds. The wave and spikes join smoothly, including between cycles.
  Only the reporting policy differs:

    Interval                     the default: every host interval, even if unchanged.
    OnChangeAtInterval           at the host interval, only after a change of at least 0.05.
    OnChange                     checked every tick, after a change of at least 0.1,
                                 at most every 100 ms.
    OnChangeAndOnChangeAtInterval changes of at least 0.05 at intervals, or 0.1 promptly.
    Explicit                     write() sends each deliberate measurement, without filtering.

  Try this:
    Set the host's logging interval to 2000 ms, or send <BLAECK.ACTIVATE,2000>.
    One cycle takes 12 seconds:
      Interval samples the underlying wave, but can miss the spikes.
      OnChangeAtInterval samples the wave, skipping interval changes below 0.05.
      OnChange follows the wave and spikes whenever a change reaches 0.1.
      OnChangeAndOnChangeAtInterval also reports changes from 0.05 at interval times.
      Explicit shows the complete wave and rounded spike shapes.
    The separate and combined modes use matching thresholds for a fair comparison.
    Interval and OnChangeAtInterval can look alike here: most interval samples
    exceed the small threshold. With constant or slowly changing values, Interval
    keeps reporting while OnChangeAtInterval skips reports.
    OnChange and the combined signal can look similar with these fine thresholds;
    their report times can differ because interval reports update the shared baseline.
    A 2-second interval cannot sample both short spikes one second apart; it may
    miss both. Compare reported points, not just the lines drawn between them.
    Some points overlap because all signals share the same source.
    Interval and OnChangeAtInterval use that interval. OnChange does not need ACTIVATE.
    OnChangeAndOnChangeAtInterval uses both paths, with one shared last-sent value.
    Every ACTIVATE first reports all three interval-enabled signals without filtering.
    Later interval reports apply their normal policies and thresholds.
    Send <BLAECK.DEACTIVATE>: only interval reporting stops. OnChange,
    OnChangeAndOnChangeAtInterval's immediate path, and the explicit write() calls keep working.
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
float Interval = 0.0f;
float OnChangeAtInterval = 0.0f;
float OnChange = 0.0f;
float OnChangeAndOnChangeAtInterval = 0.0f;
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

  device.addSignal(F("Interval"), &Interval);
  device.addSignal(F("OnChangeAtInterval"), &OnChangeAtInterval)
      .writeAtInterval(BLAECK_ON_CHANGE, 0.05f);
  device.addSignal(F("OnChange"), &OnChange)
      .writeAtInterval(BLAECK_OFF).writeOnChange(0.1f);
  device.addSignal(F("OnChangeAndOnChangeAtInterval"), &OnChangeAndOnChangeAtInterval)
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
  if (now - lastUpdate < 100UL)
    return;
  lastUpdate = now;

  const unsigned long phaseMs = now % 12000UL;
  float value = 0.8f * sin(phaseMs * (TWO_PI / 12000.0f));
  if ((phaseMs >= 5000UL && phaseMs < 5600UL) ||
      (phaseMs >= 6000UL && phaseMs < 6600UL))
  {
    const float spikePhase = (phaseMs % 1000UL) / 600.0f;
    value += 2.0f * (1.0f - cos(TWO_PI * spikePhase));
  }

  Interval = OnChangeAtInterval = OnChange = OnChangeAndOnChangeAtInterval = value;
  device.write("Explicit", value);
}
