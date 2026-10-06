/*
  Controls.ino

  Controls are what Home Assistant lets the user operate. A heater with one of each:

    <Setpoint,22.5>      number   5 to 30 °C in steps of 0.5
    <Room,Kitchen>       text     up to 23 characters
    <HeaterEnabled,1>    switch   0 or 1
    <Mode,Heat>          select   Off, Heat or Auto; <Mode,1> picks it by position
    <Reset>              button   back to 21 °C and Auto
    <Boost>              button   2 °C more, up to 30 °C

  Each value is stored in its variable, acknowledged and sent back. The functions given to
  addNumberInput(), addTextInput() and addSelect() run after the value is stored. A
  Temperature sensor shows the effect.

  Leave USE_TCP at 0 for Serial, or set it to 1 for TCP. Connect Loggbok to the
  serial port at 115200 baud, or to the printed network address on TCP port 23.
  The commands above can also be typed into a serial monitor or a TCP terminal.

  Author: Sebastian Strobl, https://github.com/sebaJoSt/blaeck
*/

#include <Blaeck.h>

#ifndef USE_TCP
#define USE_TCP 0  // 0: Serial, 1: TCP
#endif

#define HOST_NAME "Controls"

#if USE_TCP
// #define NETWORK_WITH_SERVICES  // Optional OTA and Bonjour; see WaveformGenerator/README.md.
#include "NetworkSetup.h"
NetworkSetup::Server server(23);
#endif

Blaeck device;

float setpoint = 21.0f;
char room[24] = "Living room";
bool heaterEnabled = true;
byte mode = 2;  // 0 Off, 1 Heat, 2 Auto

float temperature = 18.0f;

void onSetpoint()
{
  device.Terminal.print(F("Setpoint: "));
  device.Terminal.println(setpoint);
}

void onRoom()
{
  device.Terminal.print(F("Room: "));
  device.Terminal.println(room);
}

void onMode()
{
  char name[8];
  device.getSelectOptionNameAt(F("Mode"), mode, name, sizeof(name));
  device.Terminal.print(F("Mode: "));
  device.Terminal.println(name);
}

void onReset()
{
  setpoint = 21.0f;
  mode = 2;
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

  device.addNumberInput(F("Setpoint"), &setpoint, onSetpoint)   // <Setpoint,22.5>
      .withRange(5.0f, 30.0f, 0.5f)
      .withUnit(F("\xC2\xB0" "C"))
      .withDeviceClass(F("temperature"));
  device.addTextInput(F("Room"), room, sizeof(room), onRoom)    // <Room,Kitchen>
      .withIcon(F("mdi:sofa"))
      .config();
  device.addSwitch(F("HeaterEnabled"), &heaterEnabled)          // <HeaterEnabled,1>
      .withDisplayName(F("Heater enabled"));
  device.addSelect(F("Mode"), &mode, F("Off,Heat,Auto"), onMode)   // <Mode,Heat>
      .withIcon(F("mdi:thermostat"));

  device.addButton("Reset", onReset)                            // <Reset>
      .withIcon(F("mdi:restore"));
  device.addButton("Boost", []() { setpoint = min(setpoint + 2.0f, 30.0f); })   // <Boost>
      .withIcon(F("mdi:fire"));

  device.addSensor(F("Temperature"), &temperature)
      .withUnit(F("\xC2\xB0" "C"))
      .withDeviceClass(F("temperature"))
      .withDisplayPrecision(1)
      .writeOnChange(0.1, 1000);

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

  const bool heating = heaterEnabled && (mode == 1 || (mode == 2 && temperature < setpoint));
  temperature += ((heating ? 26.0f : 16.0f) - temperature) * 0.02f;
}
