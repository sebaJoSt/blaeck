/*
  DeviceTreeTest.ino

  Device checks, driven by drive_device_tree.py at 115200 baud. Needs only a Mega and USB:
  the "Pump controller" is simulated in this sketch, so there is no second board or wiring.

  The driver controls the simulation with plain commands:
    <POLL>              asks the simulated pump for a reading, then sends all data
    <SIM_SILENT,1|0>    makes the pump stop or resume answering
    <SIM_RESTART>       restarts the pump, so its uptime starts again
  Each prints "DONE <command>" once it has finished.
*/

#include <Blaeck.h>

// ----- The simulated pump controller -----
// In a real setup this is a second board, and readPump() talks to it over a cable.

struct PumpReading
{
  unsigned long uptimeMs;
  byte speed;
  float flow;
  float pressure;
};

bool simSilent = false;
unsigned long simBootMs = 0;
byte simSpeed = 0;

bool readPump(PumpReading &reading)
{
  if (simSilent)
    return false;
  reading.uptimeMs = millis() - simBootMs;
  reading.speed = simSpeed;
  reading.flow = simSpeed * 0.1f;
  reading.pressure = 1.0f + simSpeed * 0.01f;
  return true;
}

// ----- The main board -----

Blaeck device;
BlaeckDeviceRef pump;

float boardValue = 1.5f;
float pumpFlow = 0;
float pumpPressure = 0;
byte pumpSpeed = 0;
unsigned long lastPumpUptime = 0;

void done(const char *command)
{
  Serial.print(F("DONE "));
  Serial.println(command);
}

void pollPump()
{
  PumpReading reading;
  if (!readPump(reading))
  {
    if (!pump.isMissing())
    {
      pump.markMissing();
      pump.writeState(F("PumpLink"), "no answer");
    }
    return;
  }

  if (reading.uptimeMs < lastPumpUptime)
  {
    pump.writeRestarted();
    pump.writeEvent(F("PumpAlarms"), F("restarted"));
  }
  lastPumpUptime = reading.uptimeMs;

  pumpSpeed = reading.speed;
  pumpFlow = reading.flow;
  pumpPressure = reading.pressure;

  if (pump.isMissing())
  {
    pump.markPresent();
    pump.writeState(F("PumpLink"), "ok");
  }
}

void onPoll(const char *command, const char *const *params, byte paramCount)
{
  pollPump();
  device.writeAll();
  done(command);
}

void onSimSilent(const char *command, const char *const *params, byte paramCount)
{
  simSilent = paramCount > 0 && atoi(params[0]) != 0;
  done(command);
}

void onSimRestart(const char *command, const char *const *params, byte paramCount)
{
  simBootMs = millis();
  done(command);
}

// Forwarded to the pump, which reports the new speed with its next reading.
void onSetPumpSpeed(const char *command, const char *const *params, byte paramCount)
{
  simSpeed = (byte)atoi(params[0]);
  done(command);
}

void setup()
{
  Serial.begin(115200);
  device.begin(Serial)
      .withSignals(3)
      .withCommands(4)
      .withStateChannels(2)
      .withEventChannels(1)
      .withEventTypes(1)
      .withDevices(1);

  device.DeviceName = "DeviceTreeTest";
  device.DeviceHWVersion = "Arduino Mega 2560";
  device.DeviceFWVersion = "1.0";

  pump = device.addDevice(F("Pump controller"))
             .withHWVersion(F("Simulated"))
             .withFWVersion(F("1.0"));

  device.addSignal(F("BoardValue"), &boardValue);
  pump.addSignal(F("Flow"), &pumpFlow);
  pump.addSignal(F("Pressure"), &pumpPressure);

  pump.onNumberCommand("SET_PUMP_SPEED", onSetPumpSpeed)
      .withRange(0.0f, 100.0f, 1.0f)
      .withOwnState(F("PumpSpeed"), &pumpSpeed);
  device.onCommand("POLL", onPoll);
  device.onCommand("SIM_SILENT", onSimSilent);
  device.onCommand("SIM_RESTART", onSimRestart);

  pump.addStateChannel(F("PumpLink"), BlaeckText);
  pump.addEventChannel(F("PumpAlarms"), F("restarted"));
}

void loop()
{
  device.read();
}
