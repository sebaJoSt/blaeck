// Globals the @code blocks in Blaeck.h are written against.
// Only one file includes this, and it does not exist until it is generated: DocCodeBlocks.ino
//
// Before adding a name, read extras/API-STYLE.md - it explains why these are shared
// and when a new one is warranted.

#pragma once

#include "Arduino.h"
#include <EEPROM.h>
#include <Ethernet.h>
#include "Blaeck.h"

// The instance, named as every example sketch in this library names it.
Blaeck device;
EthernetServer server(23);
inline void onClientConnected(byte) {}
inline void onClientDisconnected(byte) {}

// --- Values a sketch might publish as signals ---
float Temperature = 0.0f;
float Frequency = 1.0f;
float Output = 0.0f;
unsigned long Uptime = 0;

// A run of signals sharing a prefix, for the withNameSuffix() block.
float sine[8];

// --- Values an input might hold ---
float Amplitude = 1.0f;
byte waveIndex = 0;
// A secret-ish setting, so a block can show a field worth masking. Nothing else in the
// cast is one: masking a device label would teach the modifier and misplace it at once.
char ApiKey[33] = "";

// --- For the write() example, which shows a signal that pulses ---
bool Pulse = false;
bool triggered = false;

// Stands in for hardware a board may or may not have fitted, so a block can show
// something being declared conditionally.
const bool hasBatteryMonitor = false;
unsigned long pulseSince = 0;

// --- Where a restored setting is kept, for the getSelectOptionIndexOf() example ---
const int addr = 0;

// Stands in for whatever a sketch actually reads, so an example can show a fresh
// value arriving without dragging a sensor library in with it.
inline float readSensor() { return 21.5f; }

// Stands in for whatever a sketch samples before a write.
inline void readAllSensors() {}

// Stands in for a real clock, for the BLAECK_UNIX timestamp block.
inline unsigned long long unixMicros() { return 0; }

// --- Inputs and sensors ---
float setpoint = 21.0f;
float amplitude = 1.0f;
float temperature = 20.0f;
float energy = 0.0f;
bool enabled = true;
bool doorOpen = false;
byte mode = 0;
byte stateIndex = 0;
char label[33] = "lab-heater";
char token[33] = "";
unsigned long uptime = 0;
unsigned long buildNumber = 1;
unsigned long beat = 0;
float output = 0.0f;

// Work a value out when it is wanted, for the addSensor() function forms.
inline bool isRunning() { return true; }
inline const char *statusText() { return "ok"; }

// --- A second board, for the addDevice() and device-handle blocks ---
BlaeckDeviceRef pump;
float pumpFlow = 0.0f;
bool pumpAnswered = true;
// Asks the pump for its flow over the sketch's link; false when it did not answer.
inline bool readFlowFromPump(float &flow) { flow = 1.0f; return true; }
unsigned long lastPumpUptime = 0;
// What the sketch last read from the pump, for the writeRestarted() block.
struct
{
  unsigned long uptimeMs;
} reading = {0};

// --- Handlers, so a command or button example has something to point at ---
inline void onLED(const char *command, const char *const *params, byte paramCount) {}
inline void onSwitchLED(const char *command, const char *const *params, byte paramCount) {}
inline void onStatus() {}
inline void onReboot() {}
inline void onFactoryReset() {}
inline void onCalibrate() {}
inline void onAny(const char *command, const char *const *params, byte count) {}
