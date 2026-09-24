/*
  ESP32C6BugBoard.ino

  Two numbers sent over Ethernet from the ESP32-C6-Bug (V2.1.0) with the
  ESP32-BUG-ETH add-on (V1.0.0), at the interval a host requests.
  Serial is for diagnostics; Blaeck hosts and terminals connect over TCP.

  Setup:
    See README.md in this folder.

  Usage:
    Upload the sketch, and open the serial monitor at 115200 baud: it prints the
    board's address.

    Connect Loggbok, or another Blaeck host, to that address on port 23. It asks for
    the data with <BLAECK.ACTIVATE,1000> (one frame a second) and stops it with
    <BLAECK.DEACTIVATE>.

    To watch what the device does, connect a telnet client such as PuTTY to the same
    address and port. It shows connections, the commands that arrive, and anything the
    library refuses. Typing a BLAECK. command there turns it into a host too, and binary
    frames follow.

  Author: Sebastian Strobl,
  More information on: https://github.com/sebaJoSt/blaeck
*/

#include <Blaeck.h>
#include <ETH.h>
#include <NetworkServer.h>
#include <SPI.h>

#define HOST_NAME "ESP32C6BugBoard"
#define SERVER_PORT 23
#define MAX_CLIENTS 8

// W5500 Ethernet add-on wiring.
#define ETH_TYPE ETH_PHY_W5500
#define ETH_ADDR 1
#define ETH_CS 5
#define ETH_IRQ 4
#define ETH_RST -1

// SPI pins
#define ETH_SPI_SCK 6
#define ETH_SPI_MISO 2
#define ETH_SPI_MOSI 7

NetworkServer server(SERVER_PORT);
Blaeck device;

// The fallback address, for when no DHCP server answers, e.g. a board cabled straight to a PC.
IPAddress ip(192, 168, 10, 177);
IPAddress dns(192, 168, 10, 1);
IPAddress gateway(192, 168, 10, 1);
IPAddress subnet(255, 255, 0, 0);

// Signals retain pointers to these variables, so keep them alive for the device's lifetime.
float randomSmallNumber;
long randomBigNumber;

void onEvent(arduino_event_id_t event)
{
  switch (event)
  {
  case ARDUINO_EVENT_ETH_START:
    Serial.println("ETH Started");
    ETH.setHostname(HOST_NAME);
    break;
  case ARDUINO_EVENT_ETH_CONNECTED:
    Serial.println("ETH Connected");
    break;
  case ARDUINO_EVENT_ETH_GOT_IP:
    Serial.print("ETH MAC: ");
    Serial.print(ETH.macAddress());
    Serial.print(", IPv4: ");
    Serial.print(ETH.localIP());
    Serial.print(", ");
    Serial.print(ETH.subnetMask());
    Serial.print(", ");
    Serial.println(ETH.gatewayIP());
    Serial.print("Blaeck Server: ");
    Serial.print(ETH.localIP());
    Serial.print(":");
    Serial.println(SERVER_PORT);
    break;
  case ARDUINO_EVENT_ETH_DISCONNECTED:
    Serial.println("ETH Disconnected");
    break;
  case ARDUINO_EVENT_ETH_STOP:
    Serial.println("ETH Stopped");
    break;
  default:
    break;
  }
}

void setup()
{
  Serial.begin(115200);
  delay(500);

  Network.onEvent(onEvent);

  SPI.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI);
  ETH.begin(ETH_TYPE, ETH_ADDR, ETH_CS, ETH_IRQ, ETH_RST, SPI);

  // A DHCP answer can take a while on a managed network. A board with no DHCP server falls back.
  unsigned long waitUntil = millis() + 30000;
  while (!ETH.hasIP() && millis() < waitUntil)
  {
    delay(50);
  }

  if (!ETH.hasIP())
  {
    ETH.config(ip, gateway, subnet, dns);
  }

  // Library diagnostics go to TCP terminals, not host connections.
  server.begin();
  device.begin(server)
      .withClients(MAX_CLIENTS)
      .withSignals(2)
      .withDebugStream(&device.Terminal);

  device.DeviceName = HOST_NAME;
  // This wiring is for a specific board that the generic ESP32C6 build target cannot name.
  device.DeviceHWVersion = "ESP32-C6-Bug V2.1.0";
  device.DeviceFWVersion = "1.0";

  device.addSignal(F("Small Number"), &randomSmallNumber);
  device.addSignal(F("Big Number"), &randomBigNumber);
}

void loop()
{
  UpdateRandomNumbers();
  device.tick();
}

void UpdateRandomNumbers()
{
  randomSmallNumber = random(1001) / 100.0;
  randomBigNumber = random(2000000000, 2100000001);
}
