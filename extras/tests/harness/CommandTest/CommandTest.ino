/*
  CommandTest.ino

  Everything a host can send, crossed with the values it might send - including the ones the
  board must refuse.

    P   plain     onCommand(), parses its own parameters
    N   number    addNumberInput(), bounded by withRange()
    S   switch    addSwitch(), 0 or 1
    L   select    addSelect(), one of a named list
    B   button    addButton(), no value at all
    T   text      addTextInput(), bounded by its buffer
    R   sensor    addSensor(), which a host cannot set

  An input's value is checked before it is stored, so a refused value is observable only as
  an absence: the callback does not fire and the variable does not move. Every callback here
  prints one line and nothing else does, so silence is the assertion.

  What to look for:
    Serial   PASS/FAIL at startup, then one CMD line per accepted command
    Broker   the controls a host builds from the entity list, which the sketch cannot see

  Author: Sebastian Strobl, https://github.com/sebaJoSt/blaeck
*/

#include "Arduino.h"
#include "Blaeck.h"

Blaeck device;

// ---- what the inputs hold -------------------------------------------------------------------
int nInt = 0;
float nFloat = 0.0f;
byte nLevel = 0;
int nBadRange = 0;
bool sEnabled = false;
bool sFlag = false;
byte lWave = 0;
byte lCase = 0;
byte lRange = 0;
char tLabel[32] = "unnamed";
char tSecret[16] = "";
long pRepeats = 0;
char status[40] = "";

// Signals, so a logging session has something to log.
unsigned long uptime = 0;

// Every accepted command bumps this. A refused one must leave it alone.
unsigned long accepted = 0;

int checks = 0;
int failures = 0;

void Check(const __FlashStringHelper *what, bool ok)
{
  checks++;
  if (!ok) failures++;
  Serial.print(ok ? F("PASS  ") : F("FAIL  "));
  Serial.println(what);
}

// One line per accepted command, in a shape a driver can parse: CMD <name> <value>
void Accept(const char *command, const char *value)
{
  accepted++;
  Serial.print(F("CMD "));
  Serial.print(command);
  Serial.print(' ');
  Serial.println(value);
}

void AcceptNumber(const char *command, double value)
{
  char text[16];
  device.toText(value, 4, text, sizeof(text));
  Accept(command, text);
}

// ---- callbacks and handlers -----------------------------------------------------------------

// Plain: nothing was checked, so everything is this handler's problem.
void onPrint(const char *command, const char *const *params, byte paramCount)
{
  if (paramCount < 2 || params[0][0] == '\0')
  {
    Serial.println(F("CMD P_print refused-by-handler"));
    return;
  }
  pRepeats = atol(params[1]);
  Accept(command, params[0]);
}

// Inputs: the value is stored before these run, so they print the variable.
void onInt() { AcceptNumber("N_int", nInt); }
void onFloat() { AcceptNumber("N_float", nFloat); }
void onLevel() { AcceptNumber("N_level", nLevel); }
void onBadRange() { AcceptNumber("N_badrange", nBadRange); }
void onEnabled() { Accept("S_enabled", sEnabled ? "1" : "0"); }
void onFlag() { Accept("S_flag", sFlag ? "1" : "0"); }

// A select holds the position, whichever of name or position a host sent.
void onWave()
{
  char name[16];
  if (!device.getSelectOptionNameAt(F("L_wave"), lWave, name, sizeof(name)))
  {
    Serial.println(F("CMD L_wave readback-failed"));
    return;
  }
  Accept("L_wave", name);
}

void onCase() { AcceptNumber("L_case", lCase); }
void onRange() { AcceptNumber("L_range", lRange); }
void onLabel() { Accept("T_label", tLabel); }
// The value is masked in a host's input box, not on the wire and not here.
void onSecret() { Accept("T_secret", tSecret); }

void onPing()
{
  snprintf(status, sizeof(status), "alive, %lu accepted", accepted + 1);
  Accept("B_ping", "pressed");
}

void onReboot() { Accept("B_reboot", "pressed"); }

// The board this was built for, so a recording says which one produced it. A harness runs
// on every core the library supports, and the widths below are what differ.
#if defined(ARDUINO_GIGA)
#define HARNESS_BOARD "Arduino Giga R1"
#elif defined(ARDUINO_AVR_MEGA2560)
#define HARNESS_BOARD "Arduino Mega 2560 Rev3"
#elif defined(ARDUINO_ARCH_ESP32)
#define HARNESS_BOARD "ESP32"
#else
#define HARNESS_BOARD "unknown board"
#endif

// int is two bytes on AVR and four on a 32-bit core, double four and eight. A driver reads
// the width off the frame, but a run is easier to read when the board has said it too.
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

// Asked for rather than only printed at boot: a Giga does not reset when DTR is
// raised, so a driver that opens the port has missed the startup lines already.
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

  // Pointed at the stream the frames go out on, deliberately: N_badrange makes a warning,
  // and a warning must never land inside an open frame. Keeping the two streams the same is
  // what would catch that.
  device.begin(Serial).withDebugStream(&Serial);

  device.withName(F("Command Test"));
  device.withHWVersion(HARNESS_BOARD);
  device.withFWVersion(F("1.0"));

  device.addSignal(F("Uptime"), &uptime);
  device.addSignal(F("Level"), &nLevel);
  device.addSignal(F("Flag"), &sFlag);

  // ---- P: plain, so serial only and unchecked ------------------------------------------
  device.onCommand("P_print", onPrint);

  // ---- N: bounded numbers ---------------------------------------------------------------
  device.addNumberInput(F("N_int"), &nInt, onInt).withRange(0.0f, 100.0f, 1.0f);
  device.addNumberInput(F("N_float"), &nFloat, onFloat)
      .withRange(-5.0f, 5.0f, 0.25f)
      .withUnit(F("V"))
      .withMode(BLAECK_NUMBER_MODE_BOX);
  device.addNumberInput(F("N_level"), &nLevel, onLevel)
      .withRange(0.0f, 255.0f, 1.0f)
      .withMode(BLAECK_NUMBER_MODE_SLIDER);
  // Refused at declaration: the input stays, without a range.
  device.addNumberInput(F("N_badrange"), &nBadRange, onBadRange).withRange(10.0f, 5.0f, 1.0f);

  // ---- S: switches ----------------------------------------------------------------------
  device.addSwitch(F("S_enabled"), &sEnabled, onEnabled);
  device.addSwitch(F("S_flag"), &sFlag, onFlag);

  // ---- L: selects -----------------------------------------------------------------------
  device.addSelect(F("L_wave"), &lWave, F("Sine,Square,Triangle,Sawtooth"), onWave);
  // Two options differing only in case. A host lists both, so both must be reachable.
  device.addSelect(F("L_case"), &lCase, F("Auto,AUTO"), onCase);
  device.addSelect(F("L_range"), &lRange, F("1V,10V,100V"), onRange);

  // ---- B: buttons, which carry no value --------------------------------------------------
  device.addButton("B_ping", onPing);
  device.addButton("B_reboot", onReboot)
      .withDeviceClass(F("restart"))
      .diagnostic()
      .disabledByDefault();

  // ---- T: text ---------------------------------------------------------------------------
  device.addTextInput(F("T_label"), tLabel, sizeof(tLabel), onLabel);
  device.addTextInput(F("T_secret"), tSecret, sizeof(tSecret), onSecret)
      .withMode(BLAECK_TEXT_MODE_PASSWORD);

  // ---- R: sensors, which a host cannot set -----------------------------------------------
  device.addSensor(F("R_uptime"), &uptime);
  device.addSensor(F("Status"), status, sizeof(status));

  device.onCommand("WIDTHS", onWidths);

  PrintWidths();
  RunLocalChecks();
}

// Everything the sketch can prove without a host. The rest needs a broker or a driver
// sending commands, because a refused value is an absence and absences do not print.
void RunLocalChecks()
{
  Serial.println();
  Serial.println(F("---- CommandTest ----"));

  Check(F("everything registered"), !device.hasRejections());
  if (device.hasRejections())
    device.printRejections(&Serial);

  Check(F("nothing accepted before a host sends anything"), accepted == 0);
  Check(F("defaults intact: nInt"), nInt == 0);
  Check(F("defaults intact: sEnabled"), sEnabled == false);
  Check(F("defaults intact: lWave"), lWave == 0);
  Check(F("defaults intact: tLabel"), strcmp(tLabel, "unnamed") == 0);

  Serial.print(F("---- "));
  Serial.print(checks - failures);
  Serial.print('/');
  Serial.print(checks);
  Serial.println(F(" passed ----"));
  Serial.println();
}

void loop()
{
  uptime = millis() / 1000UL;
  device.tick();
}
