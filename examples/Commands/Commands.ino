/*
  Commands.ino

  A command is a name with parameters, sent to the sketch. A function registered with
  onCommand() handles it and decides what the parameters mean. Commands are not entities,
  so Home Assistant shows no control for them; see the Controls example for those.

    <SwitchLED,1>          Turn on the LED
    <SwitchLED,0>          Turn off the LED
    <SwitchLED,ON>         Text works too: the handler parses its value itself
    <SwitchLED,OFF>
    <SwitchLED,>           An empty parameter: the LED goes off
    <Print,Hello,3>        Prints Hello three times; the count is 1 to 10

  onAnyCommand() sets one function that sees every command. Here it logs each one by name,
  except the BLAECK.* commands Loggbok sends.

  Leave USE_TCP at 0 for Serial, or set it to 1 for TCP. Connect Loggbok to the
  serial port at 115200 baud, or to the printed network address on TCP port 23.
  The commands above can also be typed into a serial monitor or a TCP terminal.

  Author: Sebastian Strobl, https://github.com/sebaJoSt/blaeck

  Command syntax:

    <COMMAND,PARAMETER01,PARAMETER02,...,PARAMETER10>

    Parameters arrive as text. An empty one keeps its slot, so <SwitchLED,> arrives with
    params[0][0] == '\0'.

  The circuit:
    - No wiring required, the on-board LED is used.
      LED_BUILTIN is pin 13 on the UNO and MEGA, and the right pin elsewhere.
    - Boards without an on-board LED, such as the ESP32-PoE and WT32-ETH01, use
      LED_PIN below: wire an LED with a resistor to that pin, or change it.
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

void onSwitchLED(const char *command, const char *const *params, byte paramCount);
void onPrint(const char *command, const char *const *params, byte paramCount);
bool handleAnyCommand(const char *command, const char *const *params, byte paramCount);

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

  device.onCommand("SwitchLED", onSwitchLED);   // <SwitchLED,1>
  device.onCommand("Print", onPrint);           // <Print,Hello,3>

  device.onAnyCommand(handleAnyCommand);
}

void loop()
{
  // Reads incoming commands and calls their functions.
  device.tick();
#if USE_TCP
  networkLoop();
#endif
}

void onSwitchLED(const char *command, const char *const *params, byte paramCount)
{
  (void)command;
  if (paramCount != 1)
  {
    device.Terminal.println(F("SwitchLED takes one value: 0, 1, ON or OFF."));
    return;
  }
  // equalsFlash() compares against a name kept in flash instead of SRAM.
  if (device.equalsFlash(params[0], F("ON")) || device.equalsFlash(params[0], F("1")))
  {
    digitalWrite(ledPin, HIGH);
    device.Terminal.println(F("LED is ON."));
  }
  else if (params[0][0] == '\0' ||
           device.equalsFlash(params[0], F("OFF")) || device.equalsFlash(params[0], F("0")))
  {
    digitalWrite(ledPin, LOW);
    device.Terminal.println(F("LED is OFF."));
  }
  else
  {
    device.Terminal.println(F("SwitchLED takes 0, 1, ON or OFF; LED unchanged."));
  }
}

void onPrint(const char *command, const char *const *params, byte paramCount)
{
  (void)command;
  if (paramCount != 2)
  {
    device.Terminal.println(F("Print takes two parameters: text and count."));
    return;
  }
  // Digits only, with nothing after them.
  char *end;
  const long repeats = strtol(params[1], &end, 10);
  if (params[1][0] < '0' || params[1][0] > '9' || *end != '\0' ||
      repeats < 1 || repeats > 10)
  {
    device.Terminal.println(F("Print takes a count from 1 to 10."));
    return;
  }
  for (int i = 0; i < repeats; i++)
  {
    device.Terminal.println(params[0]);
  }
}

// Sees every command, after its own function ran. Returns false, as it only logs: a mistyped
// name is still answered as unknown.
bool handleAnyCommand(const char *command, const char *const *params, byte paramCount)
{
  (void)params;
  if (strncmp(command, "BLAECK.", 7) == 0)
    return false;
  device.Terminal.print(F("Command: "));
  device.Terminal.print(command);
  device.Terminal.print(F(", parameters: "));
  device.Terminal.println(paramCount);
  return false;
}
