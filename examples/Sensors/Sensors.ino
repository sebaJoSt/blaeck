/*
  Sensors.ino

  Sensors are what Home Assistant shows but the user can't set. The argument of addSensor()
  decides the kind:

    Temperature    a float          the simulated room temperature
    DoorOpen       a bool           a binary sensor, open for 20 s every 90 s
    Cold           a function       worked out on every check
    Phase          an index         Idle, Heating or Cooling, shown by name
    LastError      a text buffer    set when a fault is simulated, cleared 10 s later

  tick() checks every sensor and sends the ones that changed. Temperature is sent when it moves
  by 0.1 or more, at most once a second.

  Leave USE_TCP at 0 for Serial, or set it to 1 for TCP. Connect Loggbok to the
  serial port at 115200 baud, or to the printed network address on TCP port 23.

  Author: Sebastian Strobl, https://github.com/sebaJoSt/blaeck
*/

#include <Blaeck.h>

#ifndef USE_TCP
#define USE_TCP 0  // 0: Serial, 1: TCP
#endif

#define HOST_NAME "Sensors"

#if USE_TCP
// #define NETWORK_WITH_SERVICES  // Optional OTA and Bonjour; see WaveformGenerator/README.md.
#include "NetworkSetup.h"
NetworkSetup::Server server(23);
#endif

Blaeck device;

float temperature = 18.0f;
bool doorOpen = false;
byte phase = 0;  // 0 Idle, 1 Heating, 2 Cooling
char lastError[40] = "";

// Called on every check: returns quickly and sends nothing itself.
bool isCold()
{
  return temperature < 19.0f;
}

void setup()
{
  Serial.begin(115200);

#if USE_TCP
  networkBegin(23);
  server.begin();
  device.begin(server)
      .withDebugStream(&device.Terminal);
#else
  device.begin(Serial)
      .withDebugStream(&Serial);
#endif

  device.withName(HOST_NAME);
  device.withFWVersion(F("1.0"));

  device.addSensor(F("Temperature"), &temperature)
      .withUnit(F("\xC2\xB0" "C"))
      .withDeviceClass(F("temperature"))
      .withDisplayPrecision(1)
      .writeOnChange(0.1, 1000);
  device.addSensor(F("DoorOpen"), &doorOpen)
      .withDisplayName(F("Door"))
      .withDeviceClass(F("door"));
  device.addSensor(F("Cold"), isCold)
      .withDeviceClass(F("cold"));
  device.addSensor(F("Phase"), &phase, F("Idle,Heating,Cooling"));
  device.addSensor(F("LastError"), lastError, sizeof(lastError))
      .withIcon(F("mdi:alert-circle"))
      .diagnostic();

  device.printRejections(&Serial);
}

void loop()
{
  updateSimulation();
  device.tick();
#if USE_TCP
  networkLoop();
#endif
}

void updateSimulation()
{
  static unsigned long lastUpdate = 0;
  const unsigned long now = millis();
  if (now - lastUpdate < 500UL)
    return;
  lastUpdate = now;

  const unsigned long seconds = now / 1000UL;
  doorOpen = seconds % 90 < 20;

  // A thermostat heats from 20 to 22 °C; an open door cools the room.
  if (temperature < 20.0f)
    phase = 1;
  else if (phase == 1 && temperature >= 22.0f)
    phase = 0;
  else if (phase != 1)
    phase = doorOpen ? 2 : 0;
  const float target = phase == 1 ? 26.0f : (doorOpen ? 10.0f : 16.0f);
  temperature += (target - temperature) * 0.02f;

  // A fault every two minutes, cleared 10 s later.
  if (seconds % 120 == 60 && lastError[0] == '\0')
    snprintf(lastError, sizeof(lastError), "sensor timeout at %lu s", seconds);
  else if (seconds % 120 == 70)
    lastError[0] = '\0';
}
