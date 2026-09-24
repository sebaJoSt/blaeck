/*
  SHT31TempHumiditySensor.ino

  Reads temperature and humidity from an Adafruit SHT31 sensor over I2C,
  then sends both values to Loggbok over Serial at 115200 baud.

  Requires the Adafruit SHT31 library and a sensor on the board's I2C pins
  at address 0x44. Readings are refreshed once per second; the host chooses
  how often they are logged.

  Author: Sebastian Strobl,
  More information on: https://github.com/sebaJoSt/blaeck
*/

#include <Blaeck.h>
#include "Adafruit_SHT31.h"

Adafruit_SHT31 sht31 = Adafruit_SHT31();
Blaeck device;

// Signals retain pointers to these variables, so keep them alive for the device's lifetime.
float temperature;
float humidity;

void setup()
{
  Serial.begin(115200);

  if (!sht31.begin(0x44))
  {
    // Carry on regardless: the device still appears, reporting NaN for both values,
    // which says more to a host than never connecting at all.
    Serial.println(F("No SHT31 answered at 0x44"));
  }

  // Clears condensation off the sensor. It warms the element, so a reading taken
  // with it on is too high.
  // sht31.heater(true);

  device.begin(Serial).withSignals(2);

  device.DeviceName = "SHT31TempHumiditySensor";
  device.DeviceFWVersion = "1.0";

  device.addSignal(F("Temperature [°C]"), &temperature)
      .withDisplayName(F("Temperature"))
      .withUnit(F("\xC2\xB0" "C"))
      .withDeviceClass(F("temperature"))
      .withStateClass(BLAECK_STATE_CLASS_MEASUREMENT)
      .withDisplayPrecision(1);
  device.addSignal(F("Humidity [%]"), &humidity)
      .withDisplayName(F("Humidity"))
      .withUnit(F("%"))
      .withDeviceClass(F("humidity"))
      .withStateClass(BLAECK_STATE_CLASS_MEASUREMENT)
      .withDisplayPrecision(1);
}

void loop()
{
  ReadSensor();

  // Reads what has come in and writes the signals when the interval is up.
  device.tick();
}

// A measurement takes about 15 ms of blocking I2C, so it runs on its own schedule
// rather than on every pass of loop().
void ReadSensor()
{
  static unsigned long lastRead = 0;

  if (millis() - lastRead < 1000)
    return;
  lastRead = millis();

  // Both are NaN while the sensor is unreachable, which a host shows as unavailable.
  temperature = sht31.readTemperature();
  humidity = sht31.readHumidity();
}
