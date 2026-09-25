/*
  PumpBoard.ino

  The second board for MainBoard.ino. It does not use blaeck: it answers MainBoard over a
  serial link, and MainBoard reports its values as the device "Pump controller".

  The circuit:
    - This board's TX to MainBoard's RX1 (pin 19 on a Mega), its RX to MainBoard's TX1
      (pin 18), and GND to GND.
    - An Uno or classic Nano uses pins 2 (RX) and 3 (TX) instead, because its hardware
      serial port is taken by USB.
    - Connect only boards with the same logic level: 5 V to 5 V, 3.3 V to 3.3 V.
    - Optional: a pump driver or an LED on PUMP_PIN, driven by the speed MainBoard sends.

  The link, 9600 baud, all numbers little-endian:
    MainBoard sends 'R'         -> this board answers with a reading (see sendReading())
    MainBoard sends 'S', speed  -> this board sets the pump speed, 0-100 %

  Author: Sebastian Strobl,
  More information on: https://github.com/sebaJoSt/blaeck
*/

#if defined(__AVR__) && !defined(HAVE_HWSERIAL1)
#include <SoftwareSerial.h>
SoftwareSerial mainLink(2, 3); // RX, TX
#else
#define mainLink Serial1
#endif

const byte PUMP_PIN = 9;
const byte READING_MARKER = 0xA5;

byte pumpSpeed = 0; // 0-100 %

// No sensors needed: flow and pressure follow the speed. Read real sensors here instead.
float readFlow() { return pumpSpeed * 0.12f; }
float readPressure() { return 1.0f + pumpSpeed * 0.015f; }

void writeLE(uint32_t value, byte count, byte &checksum)
{
  for (byte i = 0; i < count; i++)
  {
    byte b = (byte)(value >> (8 * i));
    mainLink.write(b);
    checksum += b;
  }
}

// Marker, uptime (4 bytes), speed (1), flow and pressure in hundredths (2 each), checksum.
// Explicit bytes rather than a struct, so both boards agree whatever their architecture.
void sendReading()
{
  byte checksum = 0;
  mainLink.write(READING_MARKER);
  writeLE(millis(), 4, checksum);
  writeLE(pumpSpeed, 1, checksum);
  writeLE((uint16_t)(readFlow() * 100), 2, checksum);
  writeLE((uint16_t)(readPressure() * 100), 2, checksum);
  mainLink.write(checksum);
}

void setup()
{
  mainLink.begin(9600);
  pinMode(PUMP_PIN, OUTPUT);
}

void loop()
{
  if (mainLink.available() == 0)
    return;

  int request = mainLink.read();
  if (request == 'R')
  {
    sendReading();
  }
  else if (request == 'S')
  {
    unsigned long start = millis();
    while (mainLink.available() == 0 && millis() - start < 50)
      ;
    int speed = mainLink.read();
    if (speed >= 0 && speed <= 100)
    {
      pumpSpeed = (byte)speed;
      analogWrite(PUMP_PIN, map(pumpSpeed, 0, 100, 0, 255));
    }
  }
}
