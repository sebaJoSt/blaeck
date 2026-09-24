/*
  SignalReportingTest.ino

  Direct-Serial reporting checks, driven by drive_signal_reporting.py at 115200 baud.
  No sensors, networking, Loggbok or external services are needed.

  loop() deliberately only reads commands. POLL services reporting explicitly, so
  the driver can distinguish an assignment from a reporting pass. WAIT advances the
  real Arduino clock without polling. Both direct and buffered writes are exercised.
*/

#include <Blaeck.h>
#include <math.h>

Blaeck device;
float periodic = 0, filtered = 0, immediate = 0, combined = 0, floating = 0;
bool flag = false;
char text[300] = "";
long signedValue = 0;
unsigned long unsignedValue = 0;
unsigned long callbackCalls = 0;

void beforeWrite()
{
  ++callbackCalls;
}

void resetSignals(bool buffered)
{
  device.setBeforeWriteCallback(nullptr);
  device.clearAllSignals();
  device.setBufferedWrites(buffered);
  periodic = filtered = immediate = combined = floating = 0;
  flag = false;
  text[0] = '\0';
  signedValue = 0;
  unsignedValue = 0;
  callbackCalls = 0;
  device.addSignal(F("Periodic"), &periodic);
  device.addSignal(F("Filtered"), &filtered).writeAtInterval(BLAECK_ON_CHANGE, 0.5f);
  device.addSignal(F("Immediate"), &immediate).writeAtInterval(BLAECK_OFF).writeOnChange(1, 1000);
  device.addSignal(F("Combined"), &combined).writeAtInterval(BLAECK_ON_CHANGE, 0.5f).writeOnChange(1, 1000);
  device.addSignal(F("Flag"), &flag).writeAtInterval(BLAECK_OFF).writeOnChange(999, 0);
  device.addSignal(F("Text"), text).writeAtInterval(BLAECK_OFF).writeOnChange(999, 0);
  device.addSignal(F("Signed"), &signedValue).writeAtInterval(BLAECK_OFF).writeOnChange(1, 0);
  device.addSignal(F("Unsigned"), &unsignedValue).writeAtInterval(BLAECK_OFF).writeOnChange(1, 0);
  device.addSignal(F("Float"), &floating).writeAtInterval(BLAECK_OFF).writeOnChange(0, 0);
}

bool setValue(byte index, const char *value)
{
  float number = atof(value);
  if (strcmp(value, "nan") == 0) number = NAN;
  if (strcmp(value, "inf") == 0) number = INFINITY;
  if (strcmp(value, "-inf") == 0) number = -INFINITY;
  switch (index)
  {
  case 0: periodic = number; break;
  case 1: filtered = number; break;
  case 2: immediate = number; break;
  case 3: combined = number; break;
  case 4: flag = atoi(value) != 0; break;
  case 6: signedValue = strtol(value, nullptr, 10); break;
  case 7: unsignedValue = strtoul(value, nullptr, 10); break;
  case 8: floating = number; break;
  default: return false;
  }
  return true;
}

void onTest(const char *, const char *const *params, byte count)
{
  if (count < 2)
  {
    Serial.println(F("TEST_ERROR missing sequence/action"));
    return;
  }
  const char *action = params[1];
  bool ok = true;
  if (strcmp(action, "HELLO") == 0 && count == 2)
  {
    Serial.print(F("BLAECK_REPORTING_TEST 1 "));
    Serial.print(sizeof(int));
    Serial.print(' ');
    Serial.print(sizeof(long));
    Serial.print(' ');
    Serial.println(sizeof(double));
  }
  else if (strcmp(action, "RESET") == 0 && count == 3)
    resetSignals(atoi(params[2]) != 0);
  else if (strcmp(action, "SET") == 0 && count == 4)
    ok = setValue(atoi(params[2]), params[3]);
  else if (strcmp(action, "TEXT") == 0 && count == 4)
  {
    const int length = atoi(params[2]);
    ok = length >= 0 && length < static_cast<int>(sizeof(text)) && strlen(params[3]) == 1;
    if (ok)
    {
      memset(text, 'a', length);
      if (length != 0) text[length - 1] = params[3][0];
      text[length] = '\0';
    }
  }
  else if (strcmp(action, "WRITE") == 0 && count == 4)
  {
    const int index = atoi(params[2]);
    ok = index == 2 || index == 3;
    if (ok) device.write(index, static_cast<float>(atof(params[3])));
  }
  else if (strcmp(action, "POLL") == 0 && count == 2)
    device.writeIfDue();
  else if (strcmp(action, "WAIT") == 0 && count == 3)
  {
    const long duration = atol(params[2]);
    ok = duration >= 0 && duration <= 5000;
    if (ok) delay(duration);
  }
  else if (strcmp(action, "CALLBACK") == 0 && count == 3)
    device.setBeforeWriteCallback(atoi(params[2]) ? beforeWrite : nullptr);
  else if (strcmp(action, "SYNC") != 0 || count != 2)
    ok = false;

  Serial.print(F("TEST_DONE "));
  Serial.print(params[0]);
  Serial.print(' ');
  Serial.print(ok ? 1 : 0);
  Serial.print(' ');
  Serial.print(callbackCalls);
  Serial.print(' ');
  Serial.print(millis());
  Serial.print(' ');
  Serial.println(device.hasRejections() ? 1 : 0);
}

void setup()
{
  Serial.begin(115200);
  device.begin(Serial).withSignals(9).withCommands(1);
  device.DeviceName = "Signal Reporting Test";
  device.DeviceFWVersion = "1";
  device.setTimestampMode(BLAECK_MICROS);
  resetSignals(false);
  device.onCommand("TEST", onTest);
}

void loop()
{
  device.read();
}
