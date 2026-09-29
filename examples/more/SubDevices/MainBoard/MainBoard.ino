/*
  MainBoard.ino

  A second board shown as its own device. This board runs blaeck and is connected to the host;
  PumpBoard.ino runs on a second board, which this one polls over a serial link. A host shows
  the pump's values and its speed input under a device "Pump controller" below this board.

  blaeck only reports the device. This sketch talks to PumpBoard, decides when it counts as
  missing or restarted, forwards a speed a host sets, and sends the pump's values again when
  they may have changed without the host noticing.

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
// Set by a host through the PumpSpeed input, and by every reading with the speed the pump
// actually runs at.
byte pumpSpeed;
char link[12] = "";

unsigned long lastPumpUptime = 0;
byte missedReplies = 0;
// False until the first reading, which is reported like a return.
bool pumpReported = false;

uint32_t readLE(const byte *bytes, byte count)
{
  uint32_t value = 0;
  for (byte i = 0; i < count; i++)
    value |= (uint32_t)bytes[i] << (8 * i);
  return value;
}

// Asks PumpBoard for a reading. False if it did not answer in time or the reply was garbled.
// restarted is set when the pump's uptime went backwards since the last reading.
bool requestReading(bool &restarted)
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
  restarted = uptimeMs < lastPumpUptime;
  lastPumpUptime = uptimeMs;

  pumpSpeed = reply[4];
  pumpFlow = readLE(reply + 5, 2) / 100.0f;
  pumpPressure = readLE(reply + 7, 2) / 100.0f;
  return true;
}

// Sends the pump's input and link state even if they look unchanged: while the pump was
// missing, the host heard nothing of them. The signals need nothing, since they are read from
// their variables for every data frame.
void reportPumpState()
{
  strcpy(link, "ok");
  pump.writeProperty(F("PumpSpeed"));
  pump.writeProperty(F("Link"));
}

void pollPump()
{
  bool restarted = false;
  if (requestReading(restarted))
  {
    missedReplies = 0;
    const bool cameBack = pump.isMissing() || !pumpReported;
    pump.markPresent();
    if (restarted)
    {
      pump.writeRestarted();
      pump.writeEvent(F("Alarms"), F("restarted"));
    }
    // After a gap or a restart the host's view may be out of date, so send both. A new speed
    // alone needs nothing: tick() sends it.
    if (cameBack || restarted)
    {
      reportPumpState();
      pumpReported = true;
    }
    return;
  }

  if (missedReplies < MISSES_BEFORE_MISSING)
    missedReplies++;
  if (missedReplies == MISSES_BEFORE_MISSING && !pump.isMissing())
  {
    // Sent at once: a missing device's properties are left out of tick()'s checks.
    strcpy(link, "no answer");
    pump.writeProperty(F("Link"));
    pump.markMissing();
  }
}

// blaeck has already checked the range and stored the speed; forwarding it is up to the sketch.
void onPumpSpeed()
{
  pumpLink.write('S');
  pumpLink.write(pumpSpeed);
}

void setup()
{
  Serial.begin(115200);
  pumpLink.begin(9600);

  device.begin(Serial);

  device.DeviceName = "Greenhouse";
  device.DeviceFWVersion = "1.0";

  pump = device.addDevice(F("Pump controller"))
             .withHWVersion(F("Arduino Uno"))
             .withFWVersion(F("1.0"));

  device.addSignal(F("Temperature"), &boardTemperature);

  // The pump's entries, registered through its handle. A host shows them under
  // "Pump controller", so their names need no "Pump" of their own.
  pump.addSignal(F("Flow"), &pumpFlow);
  pump.addSignal(F("Pressure"), &pumpPressure);
  pump.addNumberInput(F("PumpSpeed"), &pumpSpeed, onPumpSpeed)
      .withRange(0.0f, 100.0f, 1.0f)
      .withUnit(F("%"));
  pump.addSensor(F("Link"), link, sizeof(link));
  pump.addEvent(F("Alarms"), F("restarted"));
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
