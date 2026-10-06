/*
  Signals.ino

  Numbers, bool and text, sampled and logged on the interval the host asks for.
  A signal keeps a pointer to a variable: update that variable and tick() reads its current
  value when a frame is due. No write() call is needed.

  A signal is a name and a type, nothing more: it becomes a column in the log. How a
  dashboard shows a value - unit, icon, precision - belongs to a sensor on the same variable;
  see the Sensors example.
    Temperature [C]  a float, with its unit in the column name
    DoorOpen         a bool, logged as 0 or 1
    Mode             text
    Uptime           an unsigned long
    Sine_1..Sine_5   an array registered in a loop, with numbered names kept in flash

  No sensor hardware is needed. The values below simulate a room that cools while a door
  is open and warms when it closes, alongside five phase-shifted sine waves.

  Leave USE_TCP at 0 for Serial, or set it to 1 for TCP. Connect Loggbok to the
  serial port at 115200 baud, or to the printed network address on TCP port 23.

  In Serial mode, a serial monitor can send <BLAECK.INTERVAL_START,1000> to request one
  binary data frame per second, and <BLAECK.INTERVAL_STOP> to stop. The frames are not readable text.

  Author: Sebastian Strobl, https://github.com/sebaJoSt/blaeck
*/

#include <Blaeck.h>

#ifndef USE_TCP
#define USE_TCP 0  // 0: Serial, 1: TCP
#endif

#define HOST_NAME "Signals"

#if USE_TCP
// #define NETWORK_WITH_SERVICES  // Optional OTA and Bonjour; see WaveformGenerator/README.md.
#include "NetworkSetup.h"
NetworkSetup::Server server(23);
#endif

Blaeck device;

// Signals retain pointers to these variables, so keep them alive for the device's lifetime.
float temperature = 21.5f;
bool doorOpen = false;
char mode[16] = "warming";
unsigned long uptime = 0;
constexpr byte SINE_COUNT = 5;
float sine[SINE_COUNT];

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

  // The name is the column name, so a unit that belongs in the log goes into it.
  device.addSignal(F("Temperature [C]"), &temperature);
  device.addSignal(F("DoorOpen"), &doorOpen);
  // Text takes the buffer itself, not &Mode.
  device.addSignal(F("Mode"), mode);
  device.addSignal(F("Uptime"), &uptime);

  // The suffix gives each array element a name without building or copying a string.
  for (byte i = 0; i < SINE_COUNT; i++)
    device.addSignal(F("Sine_"), &sine[i]).withNameSuffix(i + 1);

  device.printRejections(&Serial);
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
  uptime = now / 1000;

  const float phase = now * 0.00005f;
  for (byte i = 0; i < SINE_COUNT; i++)
    sine[i] = sin(phase + i * (TWO_PI / SINE_COUNT));

  if (now - lastUpdate < 1000UL)
    return;
  lastUpdate = now;

  doorOpen = (uptime / 10) % 2 != 0;
  temperature += ((doorOpen ? 18.0f : 22.0f) - temperature) * 0.25f;
  strcpy(mode, doorOpen ? "cooling" : "warming");
}
