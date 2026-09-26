/*
  SubDevicesTest.ino

  Sub-device checks, driven by drive_sub_devices.py at 115200 baud. Needs only a Mega and USB:
  the "Pump controller" is simulated in this sketch, so there is no second board or wiring.

  The simulation is controlled with commands:
    <POLL>              asks the simulated pump for a reading, then sends all data
    <SIM_SILENT,1|0>    makes the pump stop or resume answering ("Pump silent" switch)
    <SIM_RESTART>       restarts the pump, so its uptime starts again ("Pump restart" button)
    <SIM_AUTO,0|1>      stops or resumes polling the pump every second
  Each prints "DONE <command>" once it has finished.

  On its own the sketch polls the pump every second, as a real main board would, so it can be
  tried end to end with Loggbok and Home Assistant: the two controls make the pump go missing,
  come back and restart. The driver turns the polling off first, so its checks decide when the
  pump is asked.
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

const unsigned long AUTO_POLL_MS = 1000;
bool autoPoll = true;
unsigned long lastAutoPoll = 0;

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

// A switch, so blaeck has checked the value is 0 or 1.
void onSimSilent(const char *command, const char *const *params, byte paramCount)
{
  simSilent = atoi(params[0]) != 0;
  done(command);
}

void onSimRestart(const char *command, const char *const *params, byte paramCount)
{
  simBootMs = millis();
  done(command);
}

void onSimAuto(const char *command, const char *const *params, byte paramCount)
{
  autoPoll = paramCount > 0 && atoi(params[0]) != 0;
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
      .withCommands(5)
      .withStateChannels(3)
      .withEventChannels(1)
      .withEventTypes(1)
      .withDevices(1);

  device.DeviceName = "SubDevicesTest";
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
  device.onCommand("SIM_AUTO", onSimAuto);
  device.onSwitchCommand("SIM_SILENT", onSimSilent)
      .withDisplayName(F("Pump silent"))
      .withOwnState(F("PumpSilent"), &simSilent);
  device.onButtonCommand("SIM_RESTART", onSimRestart)
      .withDisplayName(F("Pump restart"));

  pump.addStateChannel(F("PumpLink"), BlaeckText);
  pump.addEventChannel(F("PumpAlarms"), F("restarted"));
}

void loop()
{
  if (autoPoll && millis() - lastAutoPoll >= AUTO_POLL_MS)
  {
    lastAutoPoll = millis();
    pollPump();
  }

  device.tick();
}
