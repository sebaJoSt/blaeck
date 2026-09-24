/*
  Basic.ino

  Two numbers, registered as signals and sent at the interval a host requests.

  Leave USE_TCP at 0 for Serial, or set it to 1 for TCP. NetworkSetup.h covers
  Mega/GIGA Ethernet shields and ESP32-PoE/WT32-ETH01 boards.

  Connect Loggbok to the serial port at 115200 baud, or to the printed network
  address on TCP port 23. The host selects the logging interval.

  Author: Sebastian Strobl,
  More information on: https://github.com/sebaJoSt/blaeck
*/

#include <Blaeck.h>

#ifndef USE_TCP
#define USE_TCP 0  // 0: Serial, 1: TCP
#endif

#define HOST_NAME "Basic"

#if USE_TCP
// #define NETWORK_WITH_SERVICES  // Optional OTA and Bonjour; see WaveformGenerator/README.md.
#include "NetworkSetup.h"
NetworkSetup::Server server(23);
#endif

Blaeck device;

// Signals retain pointers to these variables, so keep them alive for the device's lifetime.
float randomSmallNumber;
long randomBigNumber;

void setup()
{
  Serial.begin(115200);

#if USE_TCP
  networkBegin(23);
  server.begin();
  device.begin(server).withSignals(2)
      .withDebugStream(&device.Terminal);
#else
  device.begin(Serial).withSignals(2);
#endif

  device.DeviceName = HOST_NAME;
  device.DeviceFWVersion = "1.0";

  // F() keeps the name in flash instead of SRAM, which is worth having on an
  // Uno or Nano and costs nothing anywhere else.
  device.addSignal(F("Small Number"), &randomSmallNumber);
  device.addSignal(F("Big Number"), &randomBigNumber);
}

void loop()
{
  UpdateRandomNumbers();

  // Reads what has come in and writes the signals when the interval is up.
  device.tick();
#if USE_TCP
  // Maintains DHCP and optional network services.
  networkLoop();
#endif
}

void UpdateRandomNumbers()
{
  // Random small number from 0.00 to 10.00
  randomSmallNumber = random(1001) / 100.0;

  // Random big number from 2 000 000 000 to 2 100 000 000
  randomBigNumber = random(2000000000, 2100000001);
}
