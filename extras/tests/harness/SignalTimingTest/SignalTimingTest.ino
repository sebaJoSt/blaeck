/*
  SignalTimingTest.ino

  What timestamp does a value land in TimescaleDB with, and does it land at all the moment
  it is written rather than on the next periodic tick?

  Three things get exercised here, each against the actual database rows a host writes,
  not against what Home Assistant shows:

    - the three timestamp modes (PC / MICROS / UNIX) a device can choose, switched live
      through a select command so one flash covers all three;
    - the landmine in the library's own docs: BLAECK_UNIX with no callback stamps every
      row at the Unix epoch instead of failing - reproduced on demand rather than only
      once at boot, since setTimestampCallback(nullptr) re-arms it at any time;
    - write()'s per-call timestamp override, which should land in the row exactly as
      given, independent of whatever mode is otherwise active.

    A rapid burst of write() calls (faster than the periodic interval) checks a fourth
    thing incidentally: whether out-of-cadence rows survive back-to-back, in order, with
    none dropped.

    A fifth thing: Changed opts out of intervals and uses writeOnChange(). Fire_change
    assigns its next value; tick() should report it once as a partial row. Fire_same
    explicitly writes the same value, proving that write() bypasses change filtering.

  drive_signal_timing.py fires the commands and reads the rows back with psycopg2 - lgbk's
  own TimescaleDB connection, not the MQTT/HA path, since the question here is what got
  stored, not what got displayed.

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

// ---- signals -----------------------------------------------------------------------------
unsigned long Uptime = 0;      // periodic only, seconds since boot - the metronome every
                                // other row is read against.
long Periodic = 0;             // periodic only, increments every loop() pass - never
                                // touched by a command, so any jump in it is loop() alone.
long Pushed = 0;                // changed only by Fire_push - proves write() lands its own
                                // row between two periodic ticks rather than waiting for one.
long ExplicitTS = 0;            // changed only by Fire_explicit_ts, always with the same
                                // hardcoded timestamp, independent of TimestampMode.
long Burst = 0;                 // changed only by Fire_burst, five times back-to-back.
long Changed = 0;              // immediate change reporting only, no interval participation.

// ---- TimestampMode select state -----------------------------------------------------------
// Index into "PC,MICROS,UNIX_calibrated,UNIX_no_callback", mirrored back as this select's
// own state so a driver can see which mode is currently live.
byte TimestampModeIdx = 0;

// A fixed, recognizable epoch far from "now" (2030-01-01T00:00:00Z) plus millis() since the
// mode was chosen, in microseconds - lets a driver confirm BLAECK_UNIX actually reads this
// callback rather than something else, and that it keeps advancing.
const unsigned long long UNIX_CALIBRATED_BASE_US = 1893456000000000ULL;
unsigned long long calibratedStartMillis = 0;

unsigned long long unixMicrosFake()
{
  return UNIX_CALIBRATED_BASE_US + (unsigned long long)(millis() - calibratedStartMillis) * 1000ULL;
}

// A second fixed epoch, arbitrary and unrelated to the one above (2000-01-01T00:00:00Z), used
// only as the explicit per-write override - it must land exactly regardless of TimestampMode.
const unsigned long long EXPLICIT_TS_US = 946684800000000ULL;

long burstBase = 0;

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

// Runs after a host has set the TimestampMode select; TimestampModeIdx already holds it.
void onTimestampMode()
{
  switch (TimestampModeIdx)
  {
  case 0: // PC - no device timestamp at all, lgbk stamps on arrival
    device.setTimestampMode(BLAECK_NO_TIMESTAMP);
    break;
  case 1: // MICROS - device-relative, library resyncs it against PC periodically
    device.setTimestampMode(BLAECK_MICROS);
    break;
  case 2: // UNIX_calibrated - a real callback, restarted at "now" in the fake epoch
    calibratedStartMillis = millis();
    device.setTimestampCallback(unixMicrosFake);
    device.setTimestampMode(BLAECK_UNIX);
    break;
  case 3: // UNIX_no_callback - the documented landmine, re-armed on demand: every row
          // after this stamps at the Unix epoch (1970-01-01) until a callback is set again.
    device.setTimestampCallback(nullptr);
    device.setTimestampMode(BLAECK_UNIX);
    break;
  default:
    break;
  }

  Serial.println(F("CMD TimestampMode"));
}

void onFirePush()
{
  const char *command = "Fire_push";
  Pushed++;
  device.write("Pushed", Pushed);
  Serial.print(F("CMD "));
  Serial.println(command);
}

void onFireExplicitTs()
{
  const char *command = "Fire_explicit_ts";
  ExplicitTS++;
  device.write("ExplicitTS", ExplicitTS, EXPLICIT_TS_US);
  Serial.print(F("CMD "));
  Serial.println(command);
}

void onFireBurst()
{
  const char *command = "Fire_burst";
  for (byte i = 1; i <= 5; i++)
  {
    burstBase++;
    Burst = burstBase;
    device.write("Burst", Burst);
  }
  Serial.print(F("CMD "));
  Serial.println(command);
}

void onFireChange()
{
  const char *command = "Fire_change";
  Changed++;
  Serial.print(F("CMD "));
  Serial.println(command);
}

void onFireSame()
{
  const char *command = "Fire_same";
  device.write("Changed", Changed);
  Serial.print(F("CMD "));
  Serial.println(command);
}

void setup()
{
  Serial.begin(115200);

  device.begin(Serial)
      .withDebugStream(&Serial);

  device.DeviceName = "Signal Timing Test";
  device.DeviceHWVersion = HARNESS_BOARD;
  device.DeviceFWVersion = "1.0";

  device.onCommand("WIDTHS", onWidths);

  device.addSignal(F("Uptime"), &Uptime);
  device.addSignal(F("Periodic"), &Periodic);
  device.addSignal(F("Pushed"), &Pushed);
  device.addSignal(F("ExplicitTS"), &ExplicitTS);
  device.addSignal(F("Burst"), &Burst);
  device.addSignal(F("Changed"), &Changed).writeAtInterval(BLAECK_OFF).writeOnChange(0);

  device.addSelect(F("TimestampMode"), &TimestampModeIdx,
                   F("PC,MICROS,UNIX_calibrated,UNIX_no_callback"), onTimestampMode);

  device.addButton("Fire_push", onFirePush);
  device.addButton("Fire_explicit_ts", onFireExplicitTs);
  device.addButton("Fire_burst", onFireBurst);
  device.addButton("Fire_change", onFireChange);
  device.addButton("Fire_same", onFireSame);

  PrintWidths();
  Serial.println(F("---- SignalTimingTest: 8 signals, 7 commands declared ----"));
}

void loop()
{
  Uptime = millis() / 1000UL;
  Periodic++;
  device.tick();
}
