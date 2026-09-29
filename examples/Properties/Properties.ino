/*
  Properties.ino

  Properties are current values a host shows but never logs. Inputs a host can set:
    Setpoint       addNumberInput()  the temperature the heater aims for
    HeaterEnabled  addSwitch()       whether it may heat at all
    Mode           addSelect()       Off, Heat or Auto
  Sensors a host only shows:
    Temperature    a variable        the simulated room temperature
    Heating        a function        worked out when it is checked
    Phase          an index          one of Idle, Heating, Cooling, shown by name
    LastError      a text buffer     empty until a fault is simulated

  tick() checks every property and sends one when it changes. Temperature is sent only
  when it moves by 0.1 or more, at most once a second. A host's value for an input is
  checked, stored and sent back at once; the onSetpoint() callback then runs.

  Uptime is the only signal, so it alone is logged.

  Leave USE_TCP at 0 for Serial, or set it to 1 for TCP. Connect Loggbok to the
  serial port at 115200 baud, or to the printed network address on TCP port 23.

  Author: Sebastian Strobl, https://github.com/sebaJoSt/blaeck
*/

#include <Blaeck.h>

#ifndef USE_TCP
#define USE_TCP 0  // 0: Serial, 1: TCP
#endif

#define HOST_NAME "Properties"

#if USE_TCP
// #define NETWORK_WITH_SERVICES  // Optional OTA and Bonjour; see WaveformGenerator/README.md.
#include "NetworkSetup.h"
NetworkSetup::Server server(23);
#endif

Blaeck device;

unsigned long uptime = 0;

// Inputs: a host sets these.
float setpoint = 21.0f;
bool heaterEnabled = true;
byte mode = 2;  // 0 Off, 1 Heat, 2 Auto

// Sensors: the sketch sets these.
float temperature = 18.0f;
byte phase = 0;  // 0 Idle, 1 Heating, 2 Cooling
char lastError[40] = "";

// Functions run on every check: work a value out, but do not send anything.
bool isHeating()
{
  if (!heaterEnabled || mode == 0)
    return false;
  return mode == 1 || temperature < setpoint;
}

// Runs after a host has set the setpoint; the variable already holds the new value.
void onSetpoint()
{
  lastError[0] = '\0';
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

  device.DeviceName = HOST_NAME;
  device.DeviceFWVersion = "1.0";

  device.addSignal(F("Uptime"), &uptime);

  device.addNumberInput(F("Setpoint"), &setpoint, onSetpoint)
      .withRange(5.0f, 30.0f, 0.5f)
      .withUnit(F("\xC2\xB0" "C"));
  device.addSwitch(F("HeaterEnabled"), &heaterEnabled)
      .withDisplayName(F("Heater enabled"));
  device.addSelect(F("Mode"), &mode, F("Off,Heat,Auto"));

  device.addSensor(F("Temperature"), &temperature)
      .withUnit(F("\xC2\xB0" "C"))
      .withDeviceClass(F("temperature"))
      .withDisplayPrecision(1)
      .writeOnChange(0.1, 1000);
  device.addSensor(F("Heating"), isHeating);
  device.addSensor(F("Phase"), &phase, F("Idle,Heating,Cooling"));
  device.addSensor(F("LastError"), lastError, sizeof(lastError))
      .withIcon(F("mdi:alert-circle"))
      .diagnostic();

  device.printRejections(&Serial);
}

void loop()
{
  uptime = millis() / 1000;
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

  const bool heating = isHeating();
  temperature += ((heating ? 26.0f : 16.0f) - temperature) * 0.02f;
  phase = heating ? 1 : (temperature > setpoint ? 2 : 0);

  // Every two minutes a fault; the next setpoint from a host clears it.
  if (uptime % 120 == 60 && lastError[0] == '\0')
    snprintf(lastError, sizeof(lastError), "sensor timeout at %lu s", uptime);
}
