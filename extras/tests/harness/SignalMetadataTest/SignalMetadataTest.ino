/*
  SignalMetadataTest.ino

  One signal per thing a signal can declare about itself, so a driver can check that each
  one survives the whole way: declared here, carried in the discovery payload, and shown
  by Home Assistant. A gap between any two of those is the finding.

  The other harnesses ask what the library refuses. This one asks whether what it accepts
  arrives intact - the failure it looks for is silent, because a host that cannot use a
  key drops it and says nothing.

  Every signal is named for what it declares, and drive_signal_metadata.py holds what each
  name should produce. Nothing here asserts: the board cannot see what a host made of it.

  Author: Sebastian Strobl, https://github.com/sebaJoSt/blaeck
*/

#include "Arduino.h"
#include "Blaeck.h"

Blaeck device;

// The board this was built for, so a recording says which one produced it.
#if defined(ARDUINO_GIGA)
#define HARNESS_BOARD "Arduino Giga R1"
#elif defined(ARDUINO_AVR_MEGA2560)
#define HARNESS_BOARD "Arduino Mega 2560 Rev3"
#elif defined(ARDUINO_ARCH_ESP32)
#define HARNESS_BOARD "ESP32"
#else
#define HARNESS_BOARD "unknown board"
#endif

// Values the signals point at. What they hold does not matter - only what they declare.
float temperature = 21.5f;
float humidity = 48.0f;
float power = 1234.5f;
float energy = 42.25f;
float angle = 137.0f;
float pressure = 1013.25f;
float ratio = 0.5f;
long counter = 7;
unsigned long uptime = 0;
int rawAdc = 512;
byte channelA = 1;
byte channelB = 2;
byte channelC = 3;
bool motion = false;
bool doorOpen = true;
bool problem = false;
char mode[16] = "Idle";
char label[24] = "bench";
char note[24] = "plain text";
char enumClass[16] = "Idle";
char classWins[16] = "Idle";
char emptyOpts[16] = "Idle";

void PrintWidths()
{
  Serial.print(F("board "));
  Serial.print(F(HARNESS_BOARD));
  Serial.print(F(", int "));
  Serial.print((unsigned)sizeof(int));
  Serial.print(F(" bytes, long "));
  Serial.print((unsigned)sizeof(long));
  Serial.print(F(", float "));
  Serial.print((unsigned)sizeof(float));
  Serial.print(F(", double "));
  Serial.println((unsigned)sizeof(double));
}

void onWidths(const char *command, const char *const *params, byte paramCount)
{
  (void)command;
  (void)params;
  (void)paramCount;
  PrintWidths();
}

void setup()
{
  Serial.begin(115200);

  device.begin(Serial)
      .withSignals(27)
      .withCommands(1)
      .withDebugStream(&Serial);

  device.DeviceName = "Signal Metadata Test";
  device.DeviceHWVersion = HARNESS_BOARD;
  device.DeviceFWVersion = "1.0";

  device.onCommand("WIDTHS", onWidths);

  // ---- bare, as the baseline every other line is read against -----------------------------
  device.addSignal(F("Bare"), &ratio);

  // ---- unit and device class, the pair Home Assistant converts with ------------------------
  device.addSignal(F("Temp_C"), &temperature)
      .withUnit(F("°C"))
      .withDeviceClass(F("temperature"));

  device.addSignal(F("Pressure_hPa"), &pressure)
      .withUnit(F("hPa"))
      .withDeviceClass(F("pressure"));

  // A class with no unit: declared on purpose, since it is what leaves a host converting
  // from nothing.
  device.addSignal(F("Humidity_nounit"), &humidity)
      .withDeviceClass(F("humidity"));

  // ---- state class, one signal per value ---------------------------------------------------
  device.addSignal(F("SC_measurement"), &power)
      .withUnit(F("W"))
      .withDeviceClass(F("power"))
      .withStateClass(BLAECK_STATE_CLASS_MEASUREMENT);

  device.addSignal(F("SC_total"), &energy)
      .withUnit(F("kWh"))
      .withDeviceClass(F("energy"))
      .withStateClass(BLAECK_STATE_CLASS_TOTAL);

  device.addSignal(F("SC_total_increasing"), &counter)
      .withStateClass(BLAECK_STATE_CLASS_TOTAL_INCREASING);

  device.addSignal(F("SC_measurement_angle"), &angle)
      .withUnit(F("°"))
      .withStateClass(BLAECK_STATE_CLASS_MEASUREMENT_ANGLE);

  // ---- display precision, including zero, which must not read as "unset" -------------------
  device.addSignal(F("Prec_0"), &temperature).withDisplayPrecision(0);
  device.addSignal(F("Prec_3"), &temperature).withDisplayPrecision(3);

  // ---- presentation ------------------------------------------------------------------------
  device.addSignal(F("Icon_gauge"), &ratio).withIcon(F("mdi:gauge"));
  device.addSignal(F("Named"), &ratio).withDisplayName(F("A Friendly Name"));

  // A run sharing a prefix, numbered as it is sent rather than built in RAM.
  device.addSignal(F("Chan"), &channelA).withNameSuffix(1);
  device.addSignal(F("Chan"), &channelB).withNameSuffix(2);
  device.addSignal(F("Chan"), &channelC).withNameSuffix(3);

  // ---- how a host files it -----------------------------------------------------------------
  device.addSignal(F("Diag_uptime"), &uptime)
      .withUnit(F("s"))
      .withDeviceClass(F("duration"))
      .diagnostic();

  device.addSignal(F("Hidden_rawadc"), &rawAdc).disabledByDefault();

  // Republished on every write even when the value has not moved.
  device.addSignal(F("Forced"), &ratio).forceUpdate();

  // ---- booleans, which become binary sensors and take their own vocabulary -----------------
  device.addSignal(F("Bool_bare"), &problem);
  device.addSignal(F("Bool_motion"), &motion).withDeviceClass(F("motion"));
  device.addSignal(F("Bool_door"), &doorOpen).withDeviceClass(F("door"));

  // ---- text, which can offer a closed set --------------------------------------------------
  device.addSignal(F("Text_plain"), note);
  device.addSignal(F("Text_options"), mode)
      .withOptions(F("Idle,Running,Fault"));
  device.addSignal(F("Text_named"), label)
      .withDisplayName(F("Bench Label"))
      .withIcon(F("mdi:tag"));

  // ---- what a host does when two rules collide ----------------------------------------------
  // Home Assistant's own schema requires device_class "enum" wherever options is present, and
  // rejects the entity if it sees any other class beside it. Two things follow from that, and
  // a host has to get both right - the second one shipped broken the first time around,
  // because it is easy to write "drop options when a class is declared" instead of "drop
  // options when a class *other than enum* is declared".
  device.addSignal(F("Enum_and_options"), enumClass)
      .withDeviceClass(F("enum"))
      .withOptions(F("Idle,Running,Fault"));

  device.addSignal(F("Class_wins_over_options"), classWins)
      .withDeviceClass(F("temperature"))
      .withOptions(F("Idle,Running,Fault"));

  // A closed set that turns out to hold nothing usable: every member is blank. Declared on
  // purpose, since the library refuses to store it (see withOptions()'s @warning) - the
  // question here is whether the modifier disappears cleanly rather than leaving a broken
  // options key or a device_class the sketch never really earned.
  device.addSignal(F("Empty_options"), emptyOpts)
      .withOptions(F(","));

  PrintWidths();
  Serial.println(F("---- SignalMetadataTest: 27 signals declared ----"));
}

void loop()
{
  uptime = millis() / 1000UL;
  device.tick();
}
