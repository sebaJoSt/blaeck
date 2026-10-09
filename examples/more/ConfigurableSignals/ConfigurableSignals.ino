/*
  ConfigurableSignals.ino

  Choose which signals to log, change the selection through commands, and save it in EEPROM.
  The sketch always computes the same 25 sine waves. Only the selected ones are registered
  with Blaeck, and that selection survives a restart.
  Loggbok forwards the data and controls through MQTT to Home Assistant.

  Connect Loggbok to the serial port at 115200 baud.
  Requires EEPROM support from the board's core; see the EEPROM helpers below.

  Features:
  - EEPROM stores which signals are activated.
  - Two number inputs pick a range; buttons activate or deactivate it. A host that is
    logging stops when the signals change, since its columns no longer fit.
  - Preset buttons carry their arguments in the press itself.

  Author: Sebastian Strobl,
  More information on: https://github.com/sebaJoSt/blaeck
*/

#include <Blaeck.h>
#include <EEPROM.h>

// Also marks the EEPROM layout: changing this resets the saved selection and range.
// Keep room for "X.xxx" and its null terminator.
const char FW_VERSION[6] = "1.000";

Blaeck device;

// Signals retain pointers into this array, so it must live for the device's lifetime.
#define MAXIMUM_SIGNALS 25
struct BlaeckSignal
{
  bool isActivated;
  float value;
} sine[MAXIMUM_SIGNALS + 1];
// Index 0 is unused; signal numbers start at 1.

// Bounds for the activate/deactivate buttons. Number inputs, so Home Assistant shows the range
// the next button press will apply to.
byte signalFirst = 1;
byte signalLast = MAXIMUM_SIGNALS;

//---EEPROM LAYOUT
// Offsets are fixed at compile time instead of handed out by an allocator at boot. The
// layout is known here, and a constant cannot drift between the write and the read the way
// an allocator can when someone reorders the calls that hand the addresses out.
constexpr int EEPROM_ADDR_FW_VERSION = 0;
constexpr int EEPROM_ADDR_SIGNAL_ACTIVATED = EEPROM_ADDR_FW_VERSION + sizeof(FW_VERSION);
constexpr int EEPROM_ADDR_SIGNAL_FIRST = EEPROM_ADDR_SIGNAL_ACTIVATED + (MAXIMUM_SIGNALS + 1);
constexpr int EEPROM_ADDR_SIGNAL_LAST = EEPROM_ADDR_SIGNAL_FIRST + sizeof(byte);
constexpr int EEPROM_BYTES = EEPROM_ADDR_SIGNAL_LAST + sizeof(byte);

// AVR writes straight through to real EEPROM, so both of these are nothing there. The cores
// that only emulate EEPROM in flash need a size up front and a commit afterwards, and
// wrapping that here is what lets the rest of the sketch read and write the same way on all
// of them. EEPROM.put() already compares before it writes, so an unchanged value costs no
// erase cycle on either kind of board.
#if defined(ARDUINO_ARCH_AVR)
inline void EepromBegin() {}
inline void EepromCommit() {}
#else
inline void EepromBegin() { EEPROM.begin(EEPROM_BYTES); }
inline void EepromCommit() { EEPROM.commit(); }
#endif

void onSetSignalFirst(const char *command, const char *const *params, byte paramCount);
void onSetSignalLast(const char *command, const char *const *params, byte paramCount);
void onSignalActivate();
void onSignalDeactivate();
void ApplySignalRange(bool activate, byte lo, byte hi);
void PersistActivatedSignals();

//---MEASUREMENT
unsigned long measurementLastTimeDone = 0; //[ms]
unsigned long measurementInterval = 10;    //[ms]
bool measurementFirstTime = true;

void setup()
{
  EEPROMConfiguration();

  Serial.begin(115200);

  device.begin(Serial);

  device.withName(F("ConfigurableSignals"));
  device.withFWVersion(FW_VERSION);

  // Each becomes a dashboard control. The bounds are number inputs a host sets; applying them
  // is a button.
  //
  // A bound is a setting, not a measurement, so it is not a signal: a signal is a column in
  // every logged row, and these two would be a constant repeated on each one. Boxes rather
  // than sliders, because the pair is picked by number - a slider invites dragging past the
  // bound you meant.
  device.addNumberInput(F("SignalFirst"), &signalFirst, onSignalFirst)
      .withRange(1.0f, (float)MAXIMUM_SIGNALS, 1.0f)
      .withMode(BLAECK_NUMBER_MODE_BOX)
      .withDisplayName(F("Range from"));
  device.addNumberInput(F("SignalLast"), &signalLast, onSignalLast)
      .withRange(1.0f, (float)MAXIMUM_SIGNALS, 1.0f)
      .withMode(BLAECK_NUMBER_MODE_BOX)
      .withDisplayName(F("Range to"));
  device.addButton("SIGNAL_ACTIVATE", onSignalActivate)
      .withDisplayName(F("Activate range"));
  device.addButton("SIGNAL_DEACTIVATE", onSignalDeactivate)
      .withDisplayName(F("Deactivate range"));

  // Presets: a press carries no value, so a fixed range goes in a lambda. The label says what
  // it does, so no dashboard control is needed to pick one.
  device.addButton("SIGNAL_ACTIVATE_ALL", []() { ApplySignalRange(true, 1, MAXIMUM_SIGNALS); })
      .withDisplayName(F("Activate all signals"))
      .withIcon(F("mdi:select-all"));
  device.addButton("SIGNAL_DEACTIVATE_ALL", []() { ApplySignalRange(false, 1, MAXIMUM_SIGNALS); })
      .withDisplayName(F("Deactivate all signals"))
      .withIcon(F("mdi:select-off"));

  UpdateLoggingSignals();
}

void loop()
{
  UpdateSineNumbers();

  // Processes commands and sends the selected signals at the host's interval.
  device.tick();
}

void UpdateSineNumbers()
{
  if ((millis() - measurementLastTimeDone >= measurementInterval) || measurementFirstTime)
  {
    measurementLastTimeDone = millis();
    measurementFirstTime = false;

    for (byte i = 1; i <= MAXIMUM_SIGNALS; i++)
    {
      float val = i * sin(millis() * 0.000005 * i);
      sine[i].value = val;
    }
  }
}

void UpdateLoggingSignals()
{
  device.clearAllSignals();

  // The name is a literal plus a counter, which withNameSuffix() says without building
  // it: the prefix stays in flash and the digits are produced when the name is sent. That
  // matters most here, where this runs again on every range change - a name copied to the
  // heap would be freed and allocated afresh each time, and that churn is what fragments
  // the heap.
  for (int i = 1; i <= MAXIMUM_SIGNALS; i++)
  {
    if (sine[i].isActivated)
    {
      device.addSignal(F("Sine_"), &sine[i].value).withNameSuffix(i);
    }
  }
  // A host that is logging stops at the next data frame; its next run asks for the new list.
}
