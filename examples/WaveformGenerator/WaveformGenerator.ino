/*
  WaveformGenerator.ino

  A dashboard-friendly demo: Blaeck -> Loggbok (Serial/TCP host and MQTT bridge) ->
  MQTT broker -> Home Assistant (MQTT client and dashboard).

  One fully controllable waveform, driven entirely over MQTT. The device describes what it
  exposes - its inputs, sensors, buttons and events - and Loggbok turns that into Home
  Assistant MQTT Discovery entities, so a dashboard comes from the sketch rather than being
  built by hand.

  Log fast enough to resolve the wave: at the default 1 Hz, a 20 ms interval gives 50 points
  per cycle. Sample slower than that and Output aliases into a waveform the device never made.

  Four kinds of thing, each for its own job:
    signal   a value sampled and logged                -> Output, Frequency, Annotation
    input    a value a host sets and sees back          -> Frequency, Amplitude, Offset, ...
    sensor   a value a host only sees                   -> Output, Uptime, Status
    event    a moment from a fixed list                 -> idle_warning, resumed

  Inputs (shown under their display name, set by their name):
    Frequency      number  0..2 Hz, step 0.01, a typed box
    Amplitude      number  0..100, step 0.1, a slider
    Offset         number  -100..100, step 0.1
    Waveform       select  Sine/Square/Triangle/Sawtooth
    OutputEnabled  switch  off -> Output = Offset          "Output enabled"
    DeviceLabel    text    max 32 bytes, config category  "Device label", and the name the
                           status line reports under
    Annotation     text    max 24 bytes, logged with every row through its signal
  A button:
    STATUS         fills StatusOnDemand                   "Request status"

  Frequency and Annotation are signals as well, on the same variables: an input shows and sets
  the value, the signal logs it.

  --- HOW AN INPUT GETS ITS VALUE BACK ---

  Sending a value is a request, not a fact: it can be refused, lost on the way, or replaced by
  the device on its next boot. So the device sends every input back, and what a dashboard shows
  is the device's value rather than its own guess.

   Home Assistant          MQTT broker           Loggbok         this sketch
          |                     |                   |                 |
          | .../_cmd/Amplitude  |      "40"         | <Amplitude,40>  |  checked against
  command |-------------------->|------------------>|---------------->|  0..100, stored in
          |                     |                   |                 |  amplitude
          | ../_state/Amplitude |      "40.00"      |  0x95 Property  |
    state |<--------------------|<------------------|<----------------|  sent at once

  The loop only reads the variables. The one callback, onDeviceLabel(), exists because the
  label is used right away.

  Leave USE_TCP at 0 for Serial, or set it to 1 for TCP. Connect Loggbok to the
  serial port at 115200 baud, or to the printed network address on TCP port 23.

  Author: Sebastian Strobl, https://github.com/sebaJoSt/blaeck
*/

#include <Blaeck.h>

#ifndef USE_TCP
#define USE_TCP 0  // 0: Serial, 1: TCP
#endif

#define HOST_NAME "WaveformGenerator"

#if USE_TCP
// #define NETWORK_WITH_SERVICES  // Optional OTA and Bonjour; see WaveformGenerator/README.md.
#include "NetworkSetup.h"
NetworkSetup::Server server(23);
#endif

Blaeck device;

// The library keeps pointers to these, so they live for the whole run.
float output = 0.0f;
float frequency = 1.0f;     // [Hz]
float amplitude = 1.0f;
float offset = 0.0f;
// The wave, as its position in the Waveform options: enum and options must stay in step.
enum Wave : byte { Sine, Square, Triangle, Sawtooth };
byte waveIndex = Sine;
bool enabled = true;
char deviceLabel[33] = "wave-gen";
char annotation[25] = "";   // "swapped probe", "run 3 after warm-up"

unsigned long uptime = 0;   // [s]
char status[80] = "";
char statusOnDemand[80] = "";

//---GENERATOR STATE (never leaves the sketch)
float phase = 0.0f; // normalized phase 0..1
unsigned long lastMicros = 0;

// The handle addSensor() returns, kept so the icon can follow the wave - see showWaveInIcon().
BlaeckNumberPropertyRef outputSensor;

void setup()
{
  Serial.begin(115200);

#if USE_TCP
  networkBegin(23);
  server.begin();
  device.begin(server)
      .withDebugStream(&device.Terminal);
#else
  device.begin(Serial);
#endif

  device.withName(HOST_NAME);
  device.withFWVersion(F("1.0"));

  // Logged: a column each.
  device.addSignal(F("Output [V]"), &output);
  device.addSignal(F("Frequency"), &frequency);
  device.addSignal(F("Annotation"), annotation);

  // Set by a host. A value is checked against the range and stored on the step before the
  // variable changes; anything else is refused.
  device.addNumberInput(F("Frequency"), &frequency)
      .withRange(0.0f, 2.0f, 0.01f)
      .withUnit(F("Hz"))
      .withMode(BLAECK_NUMBER_MODE_BOX);
  device.addNumberInput(F("Amplitude"), &amplitude)
      .withRange(0.0f, 100.0f, 0.1f)
      .withMode(BLAECK_NUMBER_MODE_SLIDER);
  device.addNumberInput(F("Offset"), &offset)
      .withRange(-100.0f, 100.0f, 0.1f);
  device.addSelect(F("Waveform"), &waveIndex, F("Sine,Square,Triangle,Sawtooth"))
      .withIcon(F("mdi:waveform"));
  device.addSwitch(F("OutputEnabled"), &enabled)
      .withDisplayName(F("Output enabled"));
  device.addTextInput(F("DeviceLabel"), deviceLabel, sizeof(deviceLabel), onDeviceLabel)
      .withDisplayName(F("Device label"))
      .withIcon(F("mdi:tag"))
      .config();
  device.addTextInput(F("Annotation"), annotation, sizeof(annotation));
  device.addButton("STATUS", onStatus)
      .withDisplayName(F("Request status"))
      .diagnostic();

  // Shown by a host, never set. Output changes all the time, so at most twice a second.
  outputSensor = device.addSensor(F("Output"), &output)
                     .withUnit(F("V"))
                     .withDeviceClass(F("voltage"))
                     .withStateClass(BLAECK_STATE_CLASS_MEASUREMENT)
                     .withDisplayPrecision(3)
                     .withIcon(F("mdi:sine-wave"))
                     .writeOnChange(0.001, 500);
  // The device class is what lets Home Assistant offer minutes or hours - without one the
  // unit is only a label.
  device.addSensor(F("Uptime"), &uptime)
      .withUnit(F("s"))
      .withDeviceClass(F("duration"))
      .diagnostic()
      .writeOnChange(0, 10000);
  device.addSensor(F("Status"), status, sizeof(status))
      .withIcon(F("mdi:pulse"))
      .diagnostic();
  device.addSensor(F("StatusOnDemand"), statusOnDemand, sizeof(statusOnDemand))
      .withIcon(F("mdi:message-text"))
      .diagnostic();

  // addEventType() does the same one name at a time, for a list built conditionally.
  device.addEvent(F("Activity"), F("idle_warning,resumed"))
      .withIcon(F("mdi:bell-alert"));

  // Silent unless a registration was rejected. The debug stream gives details.
  device.printRejections(&Serial);

  lastMicros = micros();
}

void loop()
{
  uptime = millis() / 1000;
  updateWaveform();
  showWaveInIcon();
  statusEvery10s();
  checkActivity();
  device.tick();
#if USE_TCP
  networkLoop();
#endif
}

// Gives the Output sensor the icon of the wave it is currently producing, so the Home
// Assistant entity changes with the input rather than staying as setup() configured it.
//
// Safe to call every pass: setting an icon that is already set does nothing, so the entity
// list goes out once per waveform change and not in between.
void showWaveInIcon()
{
  const __FlashStringHelper *icon;
  switch (waveIndex)
  {
  case Square:   icon = F("mdi:square-wave"); break;
  case Triangle: icon = F("mdi:triangle-wave"); break;
  case Sawtooth: icon = F("mdi:sawtooth-wave"); break;
  default:       icon = F("mdi:sine-wave"); break;
  }

  outputSensor.withIcon(icon);
}

void updateWaveform()
{
  unsigned long now = micros();
  float dt = (now - lastMicros) * 1e-6f; // [s]
  lastMicros = now;

  if (!enabled)
  {
    output = offset;
    return;
  }

  // Advance and wrap the normalized phase (0..1).
  phase += frequency * dt;
  phase -= floorf(phase);

  float w = 0.0f;
  switch (waveIndex)
  {
  case Square:
    w = (phase < 0.5f) ? 1.0f : -1.0f;
    break;
  case Triangle: // -1 at phase 0, +1 at phase 0.5
    w = 1.0f - 4.0f * fabsf(phase - 0.5f);
    break;
  case Sawtooth: // -1 .. +1 ramp
    w = 2.0f * phase - 1.0f;
    break;
  default:
    w = sinf((float)TWO_PI * phase);
    break;
  }

  output = offset + amplitude * w;
}

void statusEvery10s()
{
  static unsigned long lastStatusMs = 0;
  static bool first = true;

  if (!first && millis() - lastStatusMs < 10000UL)
    return;

  first = false;
  lastStatusMs = millis();
  writeStatus(status, sizeof(status));
}

// The status line, into one of the two text sensors. tick() sends it when it changes.
void writeStatus(char *out, size_t size)
{
  char freqText[10] = ""; // fits "2.00"
  device.toText(frequency, 2, freqText, sizeof(freqText));
  char waveName[12] = ""; // fits the longest option, "Triangle"
  device.getSelectOptionNameAt(F("Waveform"), waveIndex, waveName, sizeof(waveName));
  const char *runState = enabled ? "running" : "stopped";
  snprintf(out, size, "%s: %s %s @ %s Hz", deviceLabel, runState, waveName, freqText);
}

// Warns once per idle stretch (>=5s -> "idle_warning"), and only reports "resumed" if a warning
// was raised. Idle means OutputEnabled is off.
void checkActivity()
{
  static unsigned long idleSinceMs = 0;
  static bool warned = false;

  if (!enabled)
  {
    if (idleSinceMs == 0)
      idleSinceMs = millis();

    if (!warned && (millis() - idleSinceMs) >= 5000UL)
    {
      device.writeEvent(F("Activity"), F("idle_warning"));
      warned = true;
    }
    return;
  }

  if (warned)
    device.writeEvent(F("Activity"), F("resumed"));

  idleSinceMs = 0;
  warned = false;
}

// A new label shows in the status line at once, rather than with the next 10 s update.
void onDeviceLabel()
{
  writeStatus(status, sizeof(status));
}

void onStatus()
{
  writeStatus(statusOnDemand, sizeof(statusOnDemand));
}
