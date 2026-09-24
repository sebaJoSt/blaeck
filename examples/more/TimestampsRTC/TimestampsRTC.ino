/*
  TimestampsRTC.ino

  Timestamp data with a real-time clock using BLAECK_UNIX and a callback that returns
  microseconds since the Unix epoch. The RTC here has whole-second resolution.

  Requires an RTC. This sketch uses the one built into the Arduino UNO R4;
  another board just needs its own RTC library inside GetRTCUnixTimeMicros().
  For timestamp modes without RTC hardware, see docs/sending-data.md.

  Connect Loggbok to the serial port at 115200 baud. This sketch uses a fixed
  example start time, not the actual wall clock; see setup() before relying on it.

  Author: Sebastian Strobl,
  More information on: https://github.com/sebaJoSt/blaeck
*/

#include <Blaeck.h>
#include "RTC.h"

Blaeck device;

// Signals retain pointers to these variables, so keep them alive for the device's lifetime.
float sine;

unsigned long long GetRTCUnixTimeMicros()
{
  RTCTime currentTime;
  RTC.getTime(currentTime);

  // The RTC counts whole seconds, so the microsecond part is always zero.
  return (unsigned long long)currentTime.getUnixTime() * 1000000ULL;
}

void setup()
{
  Serial.begin(115200);

  RTC.begin();

  // Set the start time (UTC). The date is arbitrary - it only gives the RTC
  // something to count from, so the timestamps in the data are plausible.
  // Replace it with a real time source if you need the actual wall clock.
  RTCTime startTime(13, Month::AUGUST, 2025, 14, 00, 00, DayOfWeek::WEDNESDAY, SaveLight::SAVING_TIME_ACTIVE);
  RTC.setTime(startTime);

  device.begin(Serial).withSignals(1);

  device.DeviceName = "TimestampsRTC";
  device.DeviceFWVersion = "1.0";

  device.addSignal(F("Sine_1"), &sine);

  device.setTimestampCallback(GetRTCUnixTimeMicros);
  device.setTimestampMode(BLAECK_UNIX);
}

void loop()
{
  UpdateSineNumbers();

  // Reads what has come in and writes the signals when the interval is up.
  device.tick();
}

void UpdateSineNumbers()
{
  sine = sin(millis() * 0.00005);
}