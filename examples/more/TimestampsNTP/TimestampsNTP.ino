/*
  TimestampsNTP.ino

  Timestamp data with the network clock using BLAECK_UNIX and a callback that returns
  microseconds since the Unix epoch. Runs on the ESP32-PoE and WT32-ETH01 through
  NetworkSetup.h and needs access to an NTP server.

  The TCP server starts only after the first successful clock sync, so readings cannot
  be logged with a timestamp in 1970. If NTP is unreachable, the serial monitor reports
  it and the sketch keeps waiting.

  Open the serial monitor at 115200 baud to see the network address and sync status.
  Once synchronized, connect Loggbok to that address on TCP port 23.

  For timestamps without a network clock, see docs/network.md. For an RTC instead, see
  Blaeck's more/TimestampsRTC example on the Arduino UNO R4.

  Author: Sebastian Strobl,
  More information on: https://github.com/sebaJoSt/blaeck
*/

#include <Blaeck.h>

#if !defined(ARDUINO_ARCH_ESP32)
#error "TimestampsNTP needs an ESP32 network clock. See docs/network.md for timestamp modes without a clock source."
#endif
#include <sys/time.h>
#include <time.h>

#define HOST_NAME "TimestampsNTP"
#define SERVER_PORT 23

#include "NetworkSetup.h"
NetworkSetup::Server server(SERVER_PORT);

Blaeck device;

// Signals retain pointers to these variables, so keep them alive for the device's lifetime.
float sine;

unsigned long long GetNtpUnixTimeMicros()
{
  struct timeval now;
  gettimeofday(&now, nullptr);
  return (unsigned long long)now.tv_sec * 1000000ULL + now.tv_usec;
}

void setup()
{
  Serial.begin(115200);

  // Gets the board online; see NetworkSetup.h.
  networkBegin(SERVER_PORT);

  configTime(0, 0, "pool.ntp.org");
  Serial.println(F("Waiting for the network clock..."));
  struct tm clock;
  while (!getLocalTime(&clock, 10000))
  {
    Serial.println(F("NTP has not synchronized. Check network access to pool.ntp.org; retrying."));
    networkLoop();
  }

  server.begin();
  device.begin(server).withSignals(1);

  device.DeviceName = HOST_NAME;
  device.DeviceFWVersion = "1.0";

  device.addSignal(F("Sine_1"), &sine);

  device.setTimestampCallback(GetNtpUnixTimeMicros);
  device.setTimestampMode(BLAECK_UNIX);
}

void loop()
{
  UpdateSineNumbers();

  // Reads what has come in and writes the signals when the interval is up.
  device.tick();

  // Maintains DHCP and optional network services.
  networkLoop();
}

void UpdateSineNumbers()
{
  sine = sin(millis() * 0.00005);
}
