/*
  EventMetadataTest.ino

  Does what an event declares about itself survive to Home Assistant? One event per thing
  an event can declare - icon, category, device class, disabled-by-default, and a type list
  longer than the two-entry minimum - so a driver can check that each one arrives, and that
  a fired occurrence carries the right event_type once it does.

  EventTest asks the other question about events: does the library refuse what it should,
  and drop what was never declared.

  writeEvent()'s type has to be a flash literal (see EventTest's comment on why a runtime
  string cannot select one), so there is one button per declared type rather than one per
  event - a driver can press each and confirm both that the event survived discovery and
  that the specific type it asked for is what Home Assistant shows.

  Author: Sebastian Strobl, https://github.com/sebaJoSt/blaeck
*/

#include "Arduino.h"
#include "Blaeck.h"

Blaeck device;

// A host that refuses to log with zero signals declared needs one to have something to do;
// what it says is not this harness's question.
unsigned long uptime = 0;

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

// Every fire command does the same thing - send one named occurrence and say so on the
// debug stream, so a missing frame at the driver is the only difference between an event
// that went out and one that was refused.
void Fired(const char *command)
{
  Serial.print(F("CMD "));
  Serial.println(command);
}

// ---- Bare: the baseline every other channel is read against --------------------------------
void onFBareFirst() { device.writeEvent(F("Bare"), F("first")); Fired("F_bare_first"); }
void onFBareSecond() { device.writeEvent(F("Bare"), F("second")); Fired("F_bare_second"); }

// ---- Icon --------------------------------------------------------------------------------
void onFIconPing() { device.writeEvent(F("Icon"), F("ping")); Fired("F_icon_ping"); }
void onFIconPong() { device.writeEvent(F("Icon"), F("pong")); Fired("F_icon_pong"); }

// ---- Diag: filed as diagnostic rather than describing the device's work -------------------
void onFDiagCheck() { device.writeEvent(F("Diag"), F("check")); Fired("F_diag_check"); }
void onFDiagWarn() { device.writeEvent(F("Diag"), F("warn")); Fired("F_diag_warn"); }

// ---- Hidden: registered but switched off until someone enables it -------------------------
void onFHiddenTrace() { device.writeEvent(F("Hidden"), F("trace")); Fired("F_hidden_trace"); }
void onFHiddenDump() { device.writeEvent(F("Hidden"), F("dump")); Fired("F_hidden_dump"); }

// ---- Class_button: the closed set's standard names, though none are required --------------
void onFButtonStart() { device.writeEvent(F("Class_button"), F("press_start")); Fired("F_button_start"); }
void onFButtonEnd() { device.writeEvent(F("Class_button"), F("press_end")); Fired("F_button_end"); }

// ---- Class_doorbell: "ring" is the one type this device class requires --------------------
void onFDoorbellRing() { device.writeEvent(F("Class_doorbell"), F("ring")); Fired("F_doorbell_ring"); }

// ---- Class_motion ------------------------------------------------------------------------
void onFMotionDetected() { device.writeEvent(F("Class_motion"), F("detected")); Fired("F_motion_detected"); }
void onFMotionCleared() { device.writeEvent(F("Class_motion"), F("cleared")); Fired("F_motion_cleared"); }

// ---- Multi_type: four types rather than two, so a longer list's order is checked too ------
void onFMultiAlpha() { device.writeEvent(F("Multi_type"), F("alpha")); Fired("F_multi_alpha"); }
void onFMultiBeta() { device.writeEvent(F("Multi_type"), F("beta")); Fired("F_multi_beta"); }
void onFMultiGamma() { device.writeEvent(F("Multi_type"), F("gamma")); Fired("F_multi_gamma"); }
void onFMultiDelta() { device.writeEvent(F("Multi_type"), F("delta")); Fired("F_multi_delta"); }

void setup()
{
  Serial.begin(115200);

  device.begin(Serial)
      .withDebugStream(&Serial);

  device.withName(F("Event Metadata Test"));
  device.withHWVersion(HARNESS_BOARD);
  device.withFWVersion(F("1.0"));

  device.onCommand("WIDTHS", onWidths);

  device.addSignal(F("Uptime"), &uptime);

  // ---- Bare, no modifiers --------------------------------------------------------------------
  device.addEvent(F("Bare"), F("first,second"));
  device.addButton("F_bare_first", onFBareFirst);
  device.addButton("F_bare_second", onFBareSecond);

  // ---- Icon -----------------------------------------------------------------------------------
  device.addEvent(F("Icon"), F("ping,pong"))
      .withIcon(F("mdi:pulse"));
  device.addButton("F_icon_ping", onFIconPing);
  device.addButton("F_icon_pong", onFIconPong);

  // ---- Diagnostic category ---------------------------------------------------------------------
  device.addEvent(F("Diag"), F("check,warn"))
      .diagnostic();
  device.addButton("F_diag_check", onFDiagCheck);
  device.addButton("F_diag_warn", onFDiagWarn);

  // ---- Disabled by default, entity created but hidden until enabled -----------------------------
  device.addEvent(F("Hidden"), F("trace,dump"))
      .disabledByDefault();
  device.addButton("F_hidden_trace", onFHiddenTrace);
  device.addButton("F_hidden_dump", onFHiddenDump);

  // ---- Device class, the closed three-value set --------------------------------------------
  device.addEvent(F("Class_button"), F("press_start,press_end"))
      .withDeviceClass(F("button"));
  device.addButton("F_button_start", onFButtonStart);
  device.addButton("F_button_end", onFButtonEnd);

  // "ring" is the one type withDeviceClass(F("doorbell")) requires the channel to declare.
  device.addEvent(F("Class_doorbell"), F("ring"))
      .withDeviceClass(F("doorbell"));
  device.addButton("F_doorbell_ring", onFDoorbellRing);

  device.addEvent(F("Class_motion"), F("detected,cleared"))
      .withDeviceClass(F("motion"));
  device.addButton("F_motion_detected", onFMotionDetected);
  device.addButton("F_motion_cleared", onFMotionCleared);

  // ---- More than two types, to check the list is carried whole and in order -------------------
  device.addEvent(F("Multi_type"), F("alpha,beta,gamma,delta"));
  device.addButton("F_multi_alpha", onFMultiAlpha);
  device.addButton("F_multi_beta", onFMultiBeta);
  device.addButton("F_multi_gamma", onFMultiGamma);
  device.addButton("F_multi_delta", onFMultiDelta);

  PrintWidths();
  Serial.println(F("---- EventMetadataTest: 8 channels, 17 types declared ----"));
}

void loop()
{
  uptime = millis() / 1000UL;
  device.tick();
}
