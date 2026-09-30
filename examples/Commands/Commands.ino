/*
  Commands.ino

  How a host tells the board to do something. Three ways:

    Plain    onCommand()        You parse the parameters yourself. Not listed, so
                                there is no dashboard control.

    Button   addButton()        A press with no value. Listed, so Loggbok can
                                publish it and Home Assistant shows a button.

    Switch   addSwitch()        Not a command you handle, but a value a host sets:
                                the library checks it, stores it in a bool and
                                sends it back. Home Assistant shows a switch that
                                follows what the board holds.

  <SwitchLED> and <LED> switch the same LED, one plain and one a switch, so the
  difference is easy to see.

  Leave USE_TCP at 0 for Serial, or set it to 1 for TCP. Connect Loggbok to the
  serial port at 115200 baud, or to the printed network address on TCP port 23.
  For manual commands, use a serial monitor or a TCP terminal respectively.

  Author: Sebastian Strobl,
  More information on: https://github.com/sebaJoSt/blaeck

  Command syntax:

    <COMMAND,PARAMETER01,PARAMETER02,...,PARAMETER10>

    Parameters arrive as text. Plain handlers must check the parameter count and the whole
    value, not just a numeric prefix. An empty one keeps its slot, so <SwitchLED,> below
    reads as params[0][0] == '\0'.

  The circuit:
    - No wiring required, the on-board LED is used.
      LED_BUILTIN is pin 13 on the UNO and MEGA, and the right pin elsewhere.
    - Boards without an on-board LED, such as the ESP32-PoE and WT32-ETH01, use
      LED_PIN below: wire an LED with a resistor to that pin, or change it.

  Listed, and so also controls in Home Assistant:

        <LED,1>                       Turn on the LED
        <LED,0>                       Turn off the LED
        <LED,7>                       Rejected: a switch only accepts 0 or 1
        <Ping>                        A button. Answers on the "Status" sensor
                                      with how long the board has been running.

  Plain commands, callable by a host or terminal but not auto-discovered as controls:

        <SwitchLED,1>                 Turn on the LED
        <SwitchLED,0>                 Turn off the LED
        <SwitchLED,ON>                Also accepts text: a plain command
        <SwitchLED,OFF>               parses its value itself
        <SwitchLED,>                  Empty parameter -> uses default (OFF)
        <SwitchLED,garbage>           Reports an error; leaves the LED unchanged
        <Print,Hello,3>               Prints Hello three times; count must be 1..10
        <Print,Hello,3abc>            Reports an error; prints no copies of Hello
        <Print,Hello,11>              Reports an error; does not clamp the count

  Plain-handler errors are text feedback, not protocol rejection acknowledgements.
*/

#include <Blaeck.h>
#include <stdlib.h>

#ifndef USE_TCP
#define USE_TCP 0  // 0: Serial, 1: TCP
#endif

#define HOST_NAME "Commands"

#if USE_TCP
// #define NETWORK_WITH_SERVICES  // Optional OTA and Bonjour; see WaveformGenerator/README.md.
#include "NetworkSetup.h"
NetworkSetup::Server server(23);
#endif

Blaeck device;

#ifdef LED_BUILTIN
const int ledPin = LED_BUILTIN;
#else
#define LED_PIN 4  // No on-board LED: any free GPIO with an external LED.
const int ledPin = LED_PIN;
#endif

// The LED's state: a host sets it through the "LED" switch, and <SwitchLED> sets it too.
bool ledState = false;
// Where <Ping> answers.
char status[40] = "";

void onSwitchLED(const char *command, const char *const *params, byte paramCount);
void onLED();
void onPing();
void onPrint(const char *command, const char *const *params, byte paramCount);
void setLed(bool on);

void setup()
{
  pinMode(ledPin, OUTPUT);

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

  // Plain: the board accepts them, but a host doesn't know them.
  device.onCommand("SwitchLED", onSwitchLED);
  device.onCommand("Print", onPrint);

  // A switch a host sets; onLED() runs after it did. A button carries no value.
  device.addSwitch(F("LED"), &ledState, onLED);
  device.addButton("Ping", onPing);

  // Where <Ping> answers. tick() sends it whenever the text changes.
  device.addSensor(F("Status"), status, sizeof(status)).withIcon(F("mdi:message-text"));
}

void loop()
{
  // Handles incoming commands, and sends the switch and the sensor when they change.
  device.tick();
#if USE_TCP
  networkLoop();
#endif
}

// Plain command: the parameters arrive as text and you decide what they mean.
void onSwitchLED(const char *command, const char *const *params, byte paramCount)
{
  (void)command;
  if (paramCount != 1)
  {
    device.Terminal.println(F("SwitchLED expects exactly one value: 0, 1, ON or OFF."));
    return;
  }
  // <SwitchLED,> sends an empty field
  if (params[0][0] == '\0')
  {
    device.Terminal.println("No state given, using default (OFF).");
    setLed(false);
    return;
  }
  // Parsing it yourself means accepting whatever spelling suits you.
  // equalsFlash() compares against a name kept in flash instead of SRAM.
  if (device.equalsFlash(params[0], F("ON")) || device.equalsFlash(params[0], F("1")))
  {
    setLed(true);
    device.Terminal.println("LED is ON.");
    return;
  }
  if (device.equalsFlash(params[0], F("OFF")) || device.equalsFlash(params[0], F("0")))
  {
    setLed(false);
    device.Terminal.println("LED is OFF.");
    return;
  }

  device.Terminal.println(F("Invalid SwitchLED value. Use 0, 1, ON or OFF; LED unchanged."));
}

// The switch: the library rejected <LED,7> and stored 0 or 1 in ledState before this runs.
void onLED()
{
  setLed(ledState);
  device.Terminal.println(ledState ? "LED is ON." : "LED is OFF.");
}

/* Button: no value to parse.

   It answers on the "Status" sensor: tick() sends the new text, and Loggbok forwards it
   to Home Assistant. device.Terminal.println() is only text feedback for a serial
   monitor or TCP terminal.
*/
void onPing()
{
  // %lu is fine on AVR; only float formatting (%f) is left out of printf there.
  unsigned long seconds = millis() / 1000UL;
  snprintf(status, sizeof(status), "alive, running for %lu s", seconds);
}

/* Exemplary command using two parameters:
   Example: <Print,Hello,3>
*/
void onPrint(const char *command, const char *const *params, byte paramCount)
{
  (void)command;
  if (paramCount != 2)
  {
    device.Terminal.println(F("Print expects exactly two parameters: text and count."));
    return;
  }
  // Digits only, with no ignored suffix. The bound keeps this handler short.
  char *end;
  const long repeats = strtol(params[1], &end, 10);
  if (params[1][0] < '0' || params[1][0] > '9' || *end != '\0' ||
      repeats < 1 || repeats > 10)
  {
    device.Terminal.println(F("Print count must be decimal digits with a value from 1 to 10."));
    return;
  }
  for (int i = 0; i < repeats; i++)
  {
    device.Terminal.println(params[0]);
  }
}

// Keeps the pin and the switch in step, whichever command was used. tick() sends the switch
// when <SwitchLED> changed it.
void setLed(bool on)
{
  ledState = on;
  digitalWrite(ledPin, on ? HIGH : LOW);
}
