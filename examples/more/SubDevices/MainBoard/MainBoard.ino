/*
  MainBoard.ino

  A second board shown as its own device. This board runs blaeck and is connected to the host;
  PumpBoard.ino runs on a second board, which this one polls over a serial link. A host shows
  the pump's values and control under a device "Pump controller" below this board.

  blaeck only reports the device. This sketch talks to PumpBoard, decides when it counts as
  missing, and forwards the SET_PUMP_SPEED command.

  The circuit:
    - This board: an Arduino Mega, or another board with a second hardware serial port.
    - Its TX1 (pin 18 on a Mega) to PumpBoard's RX, its RX1 (pin 19) to PumpBoard's TX,
      and GND to GND. See PumpBoard.ino for the other side.
    - Connect only boards with the same logic level: 5 V to 5 V, 3.3 V to 3.3 V.

  Connect the host to this board's USB port at 115200 baud. Unplug the link to see the pump
  go missing, and press PumpBoard's reset button to see a restart reported for the pump only.

  Author: Sebastian Strobl,
  More information on: https://github.com/sebaJoSt/blaeck
*/

#include <Blaeck.h>

#define pumpLink Serial1

const byte READING_MARKER = 0xA5;
const unsigned long POLL_INTERVAL_MS = 200;
// Missed replies in a row before the pump counts as missing. One can be a glitch.
const byte MISSES_BEFORE_MISSING = 3;

Blaeck device;
BlaeckDeviceRef pump;

float boardTemperature;
float pumpFlow;
float pumpPressure;
byte pumpSpeed;

unsigned long lastPumpUptime = 0;
byte missedReplies = 0;

uint32_t readLE(const byte *bytes, byte count)
{
  uint32_t value = 0;
  for (byte i = 0; i < count; i++)
    value |= (uint32_t)bytes[i] << (8 * i);
  return value;
}

// Asks PumpBoard for a reading. False if it did not answer in time or the reply was garbled.
bool requestReading()
{
  while (pumpLink.available() > 0) // drop anything left from an earlier, late reply
    pumpLink.read();
  pumpLink.write('R');

  byte reply[10];
  pumpLink.setTimeout(50);
  if (!pumpLink.find((char)READING_MARKER) || pumpLink.readBytes(reply, sizeof(reply)) != sizeof(reply))
    return false;

  byte checksum = 0;
  for (byte i = 0; i < 9; i++)
    checksum += reply[i];
  if (checksum != reply[9])
    return false;

  const uint32_t uptimeMs = readLE(reply, 4);
  if (uptimeMs < lastPumpUptime)
  {
    pump.writeRestarted();
    device.writeEvent(F("Pump alarms"), F("restarted"));
  }
  lastPumpUptime = uptimeMs;

  pumpSpeed = reply[4];
  pumpFlow = readLE(reply + 5, 2) / 100.0f;
  pumpPressure = readLE(reply + 7, 2) / 100.0f;
  return true;
}

void pollPump()
{
  if (requestReading())
  {
    missedReplies = 0;
    if (pump.isMissing())
    {
      pump.markPresent();
      device.writeState(F("Pump link"), "ok");
    }
    return;
  }

  if (missedReplies < MISSES_BEFORE_MISSING)
    missedReplies++;
  if (missedReplies == MISSES_BEFORE_MISSING && !pump.isMissing())
  {
    pump.markMissing();
    device.writeState(F("Pump link"), "no answer");
  }
}

// blaeck has already checked the range; forwarding it is up to the sketch.
void onSetPumpSpeed(const char *command, const char *const *params, byte paramCount)
{
  pumpLink.write('S');
  pumpLink.write((byte)atoi(params[0]));
}

void setup()
{
  Serial.begin(115200);
  pumpLink.begin(9600);

  device.begin(Serial)
      .withSignals(3)
      .withCommands(1)
      .withStateChannels(2)
      .withEventChannels(1)
      .withEventTypes(1)
      .withDevices(1);

  device.DeviceName = "Greenhouse";
  device.DeviceFWVersion = "1.0";

  pump = device.addDevice(F("Pump controller"))
             .withHWVersion(F("Arduino Uno"))
             .withFWVersion(F("1.0"));

  device.addSignal(F("Temperature"), &boardTemperature).withUnit(F("\xC2\xB0" "C"));

  device.addSignal(F("Flow"), &pumpFlow).withUnit(F("L/min")).inDevice(pump);
  device.addSignal(F("Pressure"), &pumpPressure).withUnit(F("bar")).inDevice(pump);
  device.onNumberCommand("SET_PUMP_SPEED", onSetPumpSpeed)
      .withRange(0.0f, 100.0f, 1.0f)
      .withUnit(F("%"))
      .withOwnState(F("Pump speed"), &pumpSpeed)
      .inDevice(pump);
  device.addStateChannel(F("Pump link"), BlaeckText).inDevice(pump);
  device.addEventChannel(F("Pump alarms"), F("restarted")).inDevice(pump);
}

void loop()
{
  // No sensor needed: a slowly varying value stands in for this board's own reading.
  boardTemperature = 21.0f + sin(millis() / 60000.0f);

  static unsigned long lastPoll = 0;
  if (millis() - lastPoll >= POLL_INTERVAL_MS)
  {
    lastPoll = millis();
    pollPump();
  }

  device.tick();
}
