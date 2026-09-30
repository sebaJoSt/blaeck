/*
        File: Blaeck.cpp
        Author: Sebastian Strobl
*/

#include <Arduino.h>
#include "Blaeck.h"
#include <math.h>
#include <float.h>

namespace blaeck
{

static byte _textByte(const void *text, bool inFlash, size_t index)
{
  const byte *p = static_cast<const byte *>(text) + index;
  return inFlash ? pgm_read_byte(p) : *p;
}

static size_t _textLength(const void *text, bool inFlash, size_t limit = 255)
{
  size_t length = 0;
  if (text != nullptr)
    while (length < limit && _textByte(text, inFlash, length) != 0)
      ++length;
  return length;
}

static bool _textMatchesRam(const void *text, bool inFlash, const char *ram, size_t length)
{
  if (!inFlash)
    return memcmp(text, ram, length) == 0;
  for (size_t i = 0; i < length; ++i)
    if (_textByte(text, true, i) != static_cast<byte>(ram[i]))
      return false;
  return true;
}

void Blaeck::_emitTextBytes(const void *text, bool inFlash, size_t length)
{
  if (!inFlash)
    _emitBytes(static_cast<const byte *>(text), length);
  else
    for (size_t i = 0; i < length; ++i)
      _emitByte(_textByte(text, true, i));
}

// A device's unset version, sent as "n/a" like the board's firmware version default.
static BlaeckString _orNotAvailable(BlaeckString value)
{
  return value != nullptr ? value : BlaeckString(F("n/a"));
}

static const char *_defaultBoardName()
{
#if defined(ARDUINO_AVR_MEGA2560)
  return "Arduino Mega 2560";
#elif defined(ARDUINO_AVR_UNO)
  return "Arduino Uno";
#elif defined(ARDUINO_AVR_NANO)
  return "Arduino Nano";
#elif defined(ARDUINO_AVR_LEONARDO)
  return "Arduino Leonardo";
#elif defined(ARDUINO_AVR_MICRO)
  return "Arduino Micro";
#elif defined(ARDUINO_GIGA)
  return "Arduino GIGA R1";
#elif defined(ARDUINO_UNOWIFIR4)
  return "Arduino UNO R4 WiFi";
#elif defined(ARDUINO_MINIMA)
  return "Arduino UNO R4 Minima";
#elif defined(ARDUINO_SAMD_MKRZERO)
  return "Arduino MKR Zero";
#elif defined(ARDUINO_NANO_ESP32)
  return "Arduino Nano ESP32";
#elif defined(ARDUINO_BOARD)
  return ARDUINO_BOARD;
#else
  return "n/a";
#endif
}

Blaeck::Blaeck()
    : BlaeckDeviceBase(this, 0), Terminal(this),
      _bufferedWrites(BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT)
{
  validatePlatformSizes();
}

Blaeck::~Blaeck()
{
  end();
  // Free what the entries own before the tables, which hold the pointers. The tables free
  // their chunks when they are destroyed.
  _freeSignalOwned();
  // An event channel's name is a heap copy unless it came from flash, and nothing else
  // releases it. Every other owner has a destructor of its own.
  clearAllEvents();
  _bufFree();
}

// Outside every BLAECK_ENABLE_* block: the device list and the schema hash need it too.
byte Blaeck::_dtypeCode(dataType t)
{
  switch (t)
  {
  case (Blaeck_bool):   return 0x0;
  case (Blaeck_byte):   return 0x1;
  case (Blaeck_short):  return 0x2;
  case (Blaeck_ushort): return 0x3;
  case (Blaeck_int):    return 0x4;
  case (Blaeck_uint):   return 0x5;
  case (Blaeck_long):   return 0x6;
  case (Blaeck_ulong):  return 0x7;
  case (Blaeck_float):  return 0x8;
  case (Blaeck_double): return 0x9;
  case (Blaeck_string): return 0xA;
  case (Blaeck_longlong): return 0xB;
  default:              return 0x8;
  }
}


void Blaeck::_flushCatalogs()
{
  if (!_mayWriteFrame())
    return;

  // Each writer clears its own dirty flag, so a catalog a host asked for isn't sent twice.
  if (_entityCatalogDirty)
    this->writeEntities(0);
}

void Blaeck::_resetSignalCatalog()
{
  // The table keeps its chunks; only what the entries own is freed.
  _freeSignalOwned();
  _signalIndex = 0;
  SignalCount = 0;
  _schemaHash = 0;
  _rejectedSignalCount = 0;
  _rejectedSignalPolicyCount = 0;
}

bool Blaeck::hasRejections() const
{
  if (_rejectedStringCount != 0)
    return true;
  if (_rejectedSignalCount > 0 || _rejectedCommandCount > 0 || _rejectedSignalPolicyCount > 0
      || _rejectedDeviceCount > 0)
    return true;
  if (_rejectedPropertyCount > 0 || _rejectedEventChannelCount > 0 || _rejectedEventTypeCount > 0)
    return true;
  return false;
}

void Blaeck::_printRejectionLine(Print *out, const __FlashStringHelper *what, uint16_t dropped)
{
  out->print(F("  "));
  out->print(dropped);
  out->print(F(" "));
  out->print(what);
  out->println(F(" registrations rejected."));
}

bool Blaeck::printRejections(Print *out)
{
  if (out == nullptr || !hasRejections())
    return false;

  out->println(F("Blaeck registration rejections:"));
  if (_rejectedStringCount != 0)
  {
    out->print(F("  "));
    out->print(_rejectedStringCount);
    out->println(F(" configuration string updates rejected: insufficient memory."));
  }
  if (_rejectedSignalPolicyCount > 0)
  {
    out->print(F("  "));
    out->print(_rejectedSignalPolicyCount);
    out->println(F(" signal reporting configuration/storage failure(s); enable withDebugStream() for details."));
  }
  if (_rejectedSignalCount > 0)
    _printRejectionLine(out, F("signal"), _rejectedSignalCount);
  if (_rejectedCommandCount > 0)
    _printRejectionLine(out, F("command and button"), _rejectedCommandCount);
  if (_rejectedDeviceCount > 0)
    _printRejectionLine(out, F("device"), _rejectedDeviceCount);
  if (_rejectedPropertyCount > 0)
    _printRejectionLine(out, F("input and sensor"), _rejectedPropertyCount);
  if (_rejectedEventChannelCount > 0)
    _printRejectionLine(out, F("event"), _rejectedEventChannelCount);
  if (_rejectedEventTypeCount > 0)
    _printRejectionLine(out, F("event type"), _rejectedEventTypeCount);
  out->println(F("  Possible causes include invalid or conflicting names, "
                 "invalid event types, or insufficient memory."));
  out->println(F("  Enable withDebugStream() before registration for details."));
  return true;
}

void Blaeck::_warnNoRoom(const __FlashStringHelper *what, const char *droppedName)
{
  _warnNoRoom(what, BlaeckString(droppedName));
}

void Blaeck::_warnNoRoom(const __FlashStringHelper *what, const __FlashStringHelper *droppedName)
{
  _warnNoRoom(what, BlaeckString(droppedName));
}

void Blaeck::_warnNoRoom(const __FlashStringHelper *what, BlaeckString droppedName)
{
  if (_debugStream == nullptr)
    return;
  _debugStream->print(F("Dropped '"));
  if (droppedName != nullptr)
    droppedName.printTo(*_debugStream);
  _debugStream->print(F("': no room for another "));
  _debugStream->print(what);
  _debugStream->println(F(". The board is out of RAM."));
}

// ----- Devices -----

BlaeckDeviceRef Blaeck::addDevice(BlaeckString name)
{
  if (name == nullptr || name.read() == 0)
  {
    if (_debugStream != nullptr)
      _debugStream->println(F("Dropped a device with an empty name."));
    ++_rejectedDeviceCount;
    return BlaeckDeviceRef();
  }
  if (_hasControlChar(name))
  {
    if (_debugStream != nullptr)
      _debugStream->println(F("Dropped a device whose name holds a control character."));
    ++_rejectedDeviceCount;
    return BlaeckDeviceRef();
  }
  // A host names a device after its path, so two devices of one board can't share a name - the
  // board's own included.
  bool taken = _deviceName() == name;
  for (byte i = 0; i < _deviceCount && !taken; ++i)
    taken = BlaeckString(_devices[i].name) == name;
  if (taken)
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Dropped duplicate device name: "));
      name.printTo(*_debugStream);
      _debugStream->println();
    }
    ++_rejectedDeviceCount;
    return BlaeckDeviceRef();
  }
  if (_deviceCount >= MAX_DEVICES)
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Dropped '"));
      name.printTo(*_debugStream);
      _debugStream->println(F("': a board has at most 254 devices."));
    }
    ++_rejectedDeviceCount;
    return BlaeckDeviceRef();
  }
  if (!_devices.reserve(_deviceCount + 1))
  {
    _warnNoRoom(F("device"), name);
    ++_rejectedDeviceCount;
    return BlaeckDeviceRef();
  }
  if (!_storeString(_devices[_deviceCount].name, name))
  {
    ++_rejectedDeviceCount;
    return BlaeckDeviceRef();
  }
  ++_deviceCount;
  return BlaeckDeviceRef(this, _deviceCount);
}

void Blaeck::_setDeviceMissing(byte id, bool missing)
{
  DeviceEntry *d = _deviceEntry(id);
  if (d == nullptr || d->missing == missing)
    return;
  d->missing = missing;
  if (!missing)
  {
    d->dropNoted = false;
    // The host got none of its values meanwhile, so each signal that reports on change is
    // due again rather than compared with a value from before the gap.
    for (int i = 0; i < _signalIndex; ++i)
      if (Signals[i].DeviceId == id && Signals[i].Reporting != nullptr)
        Signals[i].Reporting->valid = false;
  }
  _writeDeviceNotices();
}

void Blaeck::_writeDeviceRestarted(byte id)
{
  DeviceEntry *d = _deviceEntry(id);
  if (d == nullptr)
    return;
  d->restartPending = true;
  _writeDeviceNotices();
}

void Blaeck::_writeDeviceNotices()
{
  // Like the board's restart notice: held back until a host can receive frames.
  if (!_mayWriteFrame())
    return;
  for (byte i = 0; i < _deviceCount; ++i)
  {
    DeviceEntry &d = _devices[i];
    if (d.restartPending)
    {
      if (!_writeDeviceNotice(i + 1, DEVICE_EVENT_RESTARTED))
        return;
      d.restartPending = false;
    }
    // Only the latest state counts: missing and back again before a host could hear of it
    // is no change.
    if (d.missing != d.reportedMissing)
    {
      if (!_writeDeviceNotice(i + 1, d.missing ? DEVICE_EVENT_NOT_RESPONDING : DEVICE_EVENT_RESPONDING))
        return;
      d.reportedMissing = d.missing;
    }
  }
}

bool Blaeck::_writeDeviceNotice(byte deviceId, byte event)
{
  // Layout: Device Notification (0xC1) in the protocol spec.
  if (!_frameOpen(0xC1, 0))
    return false;
  _emitByte(deviceId);
  _emitByte(event);
  return _frameClose();
}

void Blaeck::_writeSignalNow(int signalIndex, unsigned long long timestamp)
{
  if (timestamp == BLAECK_NOW)
    timestamp = getTimeStamp();
  if (signalIndex >= 0 && signalIndex < _signalIndex)
  {
    DeviceEntry *d = _deviceEntry(Signals[signalIndex].DeviceId);
    if (d != nullptr && d->missing)
    {
      // Said once per missing phase, so a sketch that keeps writing doesn't flood the stream.
      if (!d->dropNoted && _debugStream != nullptr)
      {
        _debugStream->print(F("write() dropped for '"));
        _emitSignalName(Signals[signalIndex], NAME_SINK_DEBUG);
        _debugStream->print(F("': '"));
        BlaeckString(d->name).printTo(*_debugStream);
        _debugStream->println(F("' is marked missing (once until markPresent())."));
      }
      d->dropNoted = true;
      return;
    }
  }
  this->writeDataFrame(0, signalIndex, signalIndex, false, timestamp);
}

void Blaeck::_writeDeviceSignals(byte deviceId, unsigned long long timestamp)
{
  if (_signalIndex == 0)
    return;
  for (int i = 0; i < _signalIndex; ++i)
    Signals[i].Selected = Signals[i].DeviceId == deviceId;
  this->writeDataFrame(0, 0, _signalIndex - 1, true, timestamp);
}

BlaeckDeviceRef &BlaeckDeviceRef::withHWVersion(BlaeckString hwVersion)
{
  if (_core != nullptr)
    if (Blaeck::DeviceEntry *d = _core->_deviceEntry(_deviceId))
      _core->_storeString(d->hwVersion, hwVersion);
  return *this;
}

BlaeckDeviceRef &BlaeckDeviceRef::withFWVersion(BlaeckString fwVersion)
{
  if (_core != nullptr)
    if (Blaeck::DeviceEntry *d = _core->_deviceEntry(_deviceId))
      _core->_storeString(d->fwVersion, fwVersion);
  return *this;
}

void BlaeckDeviceRef::markMissing()
{
  if (_core != nullptr)
    _core->_setDeviceMissing(_deviceId, true);
}

void BlaeckDeviceRef::markPresent()
{
  if (_core != nullptr)
    _core->_setDeviceMissing(_deviceId, false);
}

bool BlaeckDeviceRef::isMissing() const
{
  return _core != nullptr && _core->_deviceMissing(_deviceId);
}

void BlaeckDeviceRef::writeRestarted()
{
  if (_core != nullptr)
    _core->_writeDeviceRestarted(_deviceId);
}

void BlaeckDeviceRef::writeAll(unsigned long long timestamp)
{
  if (_core != nullptr)
    _core->_writeDeviceSignals(_deviceId, timestamp == BLAECK_NOW ? _core->getTimeStamp() : timestamp);
}

int Blaeck::_registerSignal(byte deviceId, const char *signalName, dataType type, void *address, bool textInFlash)
{
  return _registerSignalCommon(deviceId, signalName, nullptr, type, address, textInFlash);
}

int Blaeck::_registerSignal(byte deviceId, const __FlashStringHelper *signalName, dataType type, void *address, bool textInFlash)
{
  return _registerSignalCommon(deviceId, nullptr, signalName, type, address, textInFlash);
}

int Blaeck::_registerSignalCommon(byte deviceId, const char *ram, const __FlashStringHelper *flash,
                                        dataType type, void *address, bool textInFlash)
{
  // A host stores a signal under its name, so the name must be there, readable and unique on its
  // device. Anything else, '/' included, is fine: signals are never sent as commands.
  const BlaeckString name = flash != nullptr ? BlaeckString(flash) : BlaeckString(ram);
  const __FlashStringHelper *why = nullptr;
  if (name == nullptr || name.read(0) == 0)
    why = F("the name is empty");
  else if (_hasControlChar(name))
    why = F("the name holds a control character");
  else if ((flash != nullptr ? _findSignalIndex(deviceId, flash) : _findSignalIndex(deviceId, ram)) >= 0)
    why = F("its device has a signal of that name already");
  if (why != nullptr)
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Dropped signal '"));
      if (name != nullptr)
        name.printTo(*_debugStream);
      _debugStream->print(F("': "));
      _debugStream->print(why);
      _debugStream->println('.');
    }
    _rejectedSignalCount++;
    return -1;
  }

  if (_signalIndex >= MAX_TABLE_ENTRIES || !Signals.reserve(_signalIndex + 1))
  {
    if (flash != nullptr)
      _warnNoRoom(F("signal"), flash);
    else
      _warnNoRoom(F("signal"), ram);
    _rejectedSignalCount++;
    // -1 makes a handle that ignores every call.
    return -1;
  }
  _setSignalName(_signalIndex, ram, flash);
  Signals[_signalIndex].DataType = type;
  Signals[_signalIndex].Address = address;
  Signals[_signalIndex].TextInFlash = textInFlash;
  // Bit-fields can't have initializers, so set them here. The slot may be reused after
  // clearAllSignals().
  Signals[_signalIndex].IntervalMode = BLAECK_ALWAYS;
  Signals[_signalIndex].Selected = 0;
  Signals[_signalIndex].HasSuffix = 0;
  Signals[_signalIndex].NameSuffix = 0;
  Signals[_signalIndex].DeviceId = deviceId;
  int16_t added = (int16_t)_signalIndex;
  _signalIndex++;
  SignalCount = _signalIndex;
  _schemaHash = _computeSchemaHash();
  return added;
}

void Blaeck::clearAllSignals()
{
  // Free the name copies too, not only rewind the index.
  _freeSignalOwned();
  _signalIndex = 0;
  SignalCount = _signalIndex;
  _schemaHash = 0;
  _rejectedSignalCount = 0;
  _rejectedSignalPolicyCount = 0;
}

uint16_t Blaeck::_computeSchemaHash()
{
  // CRC-16/XMODEM (init 0x0000, poly 0x1021) over each signal's device list bytes: its name, the
  // 0 that ends it, its type code; a sub-device's signal is preceded by the device's name and its
  // 0. That is Python's binascii.crc_hqx(data, 0). Names go through _emitSignalName(), as in the
  // device list. The 0 after each name keeps two different lists from feeding the same bytes, and
  // lets names hold any character, '/' included.
  //
  // Signals are hashed in device list order: the board's first, then each device's. That is the
  // order the list gives them in and data frames number them by, so each signal's WireIndex is
  // set here too. This runs whenever the signals change.
  _schemaHashAccum = 0x0000;
  uint16_t wireIndex = 0;
  for (int dev = 0; dev <= _deviceCount; ++dev)
  for (int j = 0; j < _signalIndex; j++)
  {
    if (Signals[j].DeviceId != dev)
      continue;
    Signals[j].WireIndex = wireIndex++;
    if (const DeviceEntry *d = _deviceEntry(Signals[j].DeviceId))
    {
      BlaeckString deviceName = d->name;
      for (size_t k = 0; deviceName.read(k) != 0; ++k)
        _schemaHashFeedByte(deviceName.read(k));
      _schemaHashFeedByte(0);
    }
    _signalNameFeedHash(Signals[j]);
    _schemaHashFeedByte(0);
    _schemaHashFeedByte(_dtypeCode(Signals[j].DataType));
  }
  return _schemaHashAccum;
}

void Blaeck::setSignalName(int signalIndex, const char *signalName)
{
  _setSignalName(signalIndex, signalName, nullptr);
  // A new name changes the schema, so the hash must change too.
  _schemaHash = _computeSchemaHash();
}

void Blaeck::_setSignalName(int signalIndex, const char *ram, const __FlashStringHelper *flash)
{
  if (signalIndex < 0 || signalIndex >= (int)Signals.capacity())
    return;

  Signal &s = Signals[signalIndex];
  // Free the copy the slot held; a flash name owns nothing. Test the pointer before
  // NameInFlash, which means nothing in a slot that has never been named.
  if (s.SignalName != nullptr && !s.NameInFlash)
    free((void *)s.SignalName);
  s.SignalName = nullptr;
  s.NameInFlash = 0;

  if (flash != nullptr)
  {
    s.SignalName = reinterpret_cast<const char *>(flash);
    s.NameInFlash = 1;
    return;
  }
  if (ram == nullptr)
    return;

  // Copied, so the caller can reuse its buffer.
  size_t needed = strlen(ram) + 1;
  char *copy = (char *)malloc(needed);
  if (copy != nullptr)
  {
    memcpy(copy, ram, needed);
    s.SignalName = copy;
  }
    // Out of RAM: the signal stays, with an empty name.
}

void Blaeck::_freeSignalOwned()
{
  for (unsigned int i = 0; i < Signals.capacity(); i++)
  {
    // Pointer first, as in _setSignalName().
    if (Signals[i].SignalName != nullptr && !Signals[i].NameInFlash)
      free((void *)Signals[i].SignalName);
    Signals[i].SignalName = nullptr;
    Signals[i].NameInFlash = 0;
    delete Signals[i].Reporting;
    Signals[i].Reporting = nullptr;
  }
}

void Blaeck::_reportSignalPolicyError(const __FlashStringHelper *message)
{
  if (_rejectedSignalPolicyCount != UINT16_MAX)
    ++_rejectedSignalPolicyCount;
  if (_debugStream != nullptr)
    _debugStream->println(message);
}

ReportingState *Blaeck::_ensureReporting(int16_t index)
{
  if (index < 0 || index >= _signalIndex)
    return nullptr;
  Signal &s = Signals[index];
  if (s.Reporting == nullptr)
  {
    s.Reporting = new (std::nothrow) ReportingState();
    if (s.Reporting == nullptr)
      _reportSignalPolicyError(F("No RAM for signal reporting; previous policy retained."));
  }
  return s.Reporting;
}

void Blaeck::_setSignalInterval(int16_t index, BlaeckIntervalMode mode, double delta)
{
  if (index < 0 || index >= _signalIndex)
    return;
  Signal &s = Signals[index];
  if ((mode != BLAECK_OFF && mode != BLAECK_ALWAYS && mode != BLAECK_ON_CHANGE) ||
      (mode == BLAECK_ON_CHANGE && s.DataType != Blaeck_bool && s.DataType != Blaeck_string &&
       (delta < 0 || isnan(delta) || isinf(delta))))
  {
    _reportSignalPolicyError(F("Invalid interval reporting policy/threshold; previous policy retained."));
    return;
  }
  if (mode == BLAECK_ON_CHANGE)
  {
    ReportingState *r = _ensureReporting(index);
    if (r == nullptr)
      return;
    r->intervalDelta = delta;
  }
  s.IntervalMode = mode;
  if (mode != BLAECK_ON_CHANGE && s.Reporting != nullptr && !s.Reporting->immediate)
  {
    delete s.Reporting;
    s.Reporting = nullptr;
  }
}

void Blaeck::_setSignalOnChange(int16_t index, double delta, uint32_t minIntervalMs)
{
  if (index < 0 || index >= _signalIndex)
    return;
  const Signal &s = Signals[index];
  if (s.DataType != Blaeck_bool && s.DataType != Blaeck_string &&
      (delta < 0 || isnan(delta) || isinf(delta)))
  {
    _reportSignalPolicyError(F("Invalid change threshold; previous policy retained."));
    return;
  }
  if (ReportingState *r = _ensureReporting(index))
  {
    r->changeDelta = delta;
    r->minIntervalMs = minIntervalMs;
    r->immediate = true;
  }
}

void Blaeck::_setSignalOnChange(int16_t index, BlaeckIntervalMode mode)
{
  if (index < 0 || index >= _signalIndex)
    return;
  if (mode != BLAECK_OFF)
  {
    _reportSignalPolicyError(F("Invalid change reporting mode; use BLAECK_OFF or a numeric threshold; previous policy retained."));
    return;
  }
  Signal &s = Signals[index];
  if (s.Reporting == nullptr)
    return;
  s.Reporting->immediate = false;
  if (s.IntervalMode != BLAECK_ON_CHANGE)
  {
    delete s.Reporting;
    s.Reporting = nullptr;
  }
}

void Blaeck::_resetReportingBaselines()
{
  for (int i = 0; i < _signalIndex; ++i)
    if (Signals[i].Reporting != nullptr)
      Signals[i].Reporting->valid = false;
}

static size_t _signalValueSize(dataType type)
{
  switch (type)
  {
  case Blaeck_bool: case Blaeck_byte: return 1;
  case Blaeck_short: case Blaeck_ushort: case Blaeck_int: case Blaeck_uint: return 2;
  case Blaeck_long: case Blaeck_ulong: case Blaeck_float: return 4;
  case Blaeck_double: case Blaeck_longlong: return 8;
  default: return 0;
  }
}

static bool _isIntegerType(dataType type)
{
  return type != Blaeck_float && type != Blaeck_double && type != Blaeck_bool && type != Blaeck_string;
}

// Whether v is inside what a variable of this type holds. A float or a double takes any finite
// value; a whole number is bounded by its width. Used both by a write, which must not store what
// the variable cannot hold, and by withRange(), where a bound the variable cannot reach would
// accept such a write in the first place.
static bool _fitsType(dataType type, double v)
{
  switch (type)
  {
  case Blaeck_byte: return v >= 0 && v <= 255.0;
  case Blaeck_short: case Blaeck_int: return v >= -32768.0 && v <= 32767.0;
  case Blaeck_ushort: case Blaeck_uint: return v >= 0 && v <= 65535.0;
  case Blaeck_long: return v >= -2147483648.0 && v <= 2147483647.0;
  case Blaeck_ulong: return v >= 0 && v <= 4294967295.0;
  case Blaeck_float: case Blaeck_double: return true;
  // The bounds are the nearest doubles inside the range: 2^63 itself does not fit.
  case Blaeck_longlong: return v >= -9223372036854775808.0 && v < 9223372036854775808.0;
  default: return false;
  }
}

// Whether v survives the type a range is sent in. A whole-number type carries no fraction, so a
// bound or a step with one would arrive truncated - 0.5 as 0 - stating something the sketch did
// not. Stricter than _fitsType(), which a write uses: a write's fraction is refused earlier, by
// the command parser, and never reaches a variable this way.
static bool _representable(dataType type, double v)
{
  if (!_fitsType(type, v))
    return false;
  switch (type)
  {
  case Blaeck_float: case Blaeck_double: return true;
  default: return v == floor(v);
  }
}

// _fitsType() for a whole number, compared as one. A bound on a wide integer is checked without
// a double ever holding it: on a board where a double is a float, 4000000000 would round to a
// neighbour before the comparison and pass or fail on that instead.
static bool _fitsTypeWhole(dataType type, long long v)
{
  switch (type)
  {
  case Blaeck_byte: return v >= 0 && v <= 255LL;
  case Blaeck_short: case Blaeck_int: return v >= -32768LL && v <= 32767LL;
  case Blaeck_ushort: case Blaeck_uint: return v >= 0 && v <= 65535LL;
  case Blaeck_long: return v >= -2147483648LL && v <= 2147483647LL;
  case Blaeck_ulong: return v >= 0 && v <= 4294967295LL;
  // A whole number reaches any of these: a long long by being one, a float or a double by
  // being a value they take, exactly or as the nearest they have.
  case Blaeck_float: case Blaeck_double: case Blaeck_longlong: return true;
  default: return false;
  }
}

// A bound as a double, whichever member of it is the live one. A whole-number variable's bound
// is a long long; every other variable's is a double already.
static double _boundAsDouble(dataType type, const blaeck_detail::RangeBound &b)
{
  return _isIntegerType(type) ? (double)b.asInteger : b.asDouble;
}

// A bound in the kind the variable keeps it in, ready to store.
static blaeck_detail::RangeBound _makeBound(dataType type, double v)
{
  blaeck_detail::RangeBound b;
  if (!_isIntegerType(type))
    b.asDouble = v;
  // A bound the variable cannot reach is refused before it is ever sent, but it still passes
  // through here, and converting one to a long long anyway is what the language leaves
  // undefined - 1e300 has no answer. It is kept as nothing at all instead.
  else if (_fitsType(type, v))
    b.asInteger = (long long)v;
  else
    b.asInteger = 0;
  return b;
}

static blaeck_detail::RangeBound _makeBound(dataType type, long long v)
{
  blaeck_detail::RangeBound b;
  if (_isIntegerType(type))
    b.asInteger = v;
  else
    b.asDouble = (double)v;
  return b;
}

// Makes room in r for a text snapshot of this length. False, reported once, if RAM ran out.
bool Blaeck::_prepareTextSnapshot(ReportingState &r, size_t length)
{
  const uint16_t needed = static_cast<uint16_t>(length) + 1;
  if (needed > r.textCapacity)
  {
    char *text = new (std::nothrow) char[needed];
    if (text == nullptr)
    {
      if (!r.memoryError)
        _reportSignalPolicyError(F("No RAM for a text snapshot; the value is not sent."));
      r.memoryError = true;
      return false;
    }
    if (r.text != nullptr)
      memcpy(text, r.text, static_cast<size_t>(r.textLength) + 1);
    delete[] r.text;
    r.text = text;
    r.textCapacity = needed;
  }
  r.memoryError = false;
  return true;
}

bool Blaeck::_prepareSignalSnapshot(Signal &s)
{
  if (s.Reporting == nullptr || s.DataType != Blaeck_string)
    return true;
  return _prepareTextSnapshot(*s.Reporting, _textLength(s.Address, s.TextInFlash));
}

// Copies a value into r as the baseline later values are compared with. For text, r must have
// room from _prepareTextSnapshot().
static void _captureValue(ReportingState &r, dataType type, const void *value, bool inFlash)
{
  if (type == Blaeck_string)
  {
    r.textLength = static_cast<byte>(_textLength(value, inFlash));
    if (r.textLength != 0)
    {
      if (!inFlash)
        memcpy(r.text, value, r.textLength);
      else
        for (uint16_t i = 0; i < r.textLength; ++i)
          r.text[i] = static_cast<char>(_textByte(value, true, i));
    }
    r.text[r.textLength] = '\0';
  }
  else
    memcpy(r.value, value, _signalValueSize(type));
}

void Blaeck::_captureSignalSnapshot(Signal &s)
{
  _captureValue(*s.Reporting, s.DataType, s.Address, s.TextInFlash);
}

// U is an unsigned type at least as wide as T.
template<class T, class U = unsigned long>
static bool _integerSignalChanged(const void *address, const byte *baseline, double delta)
{
  T current, previous;
  memcpy(&previous, baseline, sizeof(T));
  memcpy(&current, address, sizeof(T));
  if (current == previous)
    return false;
  // Unsigned subtraction also handles signed endpoints without signed overflow.
  const U difference = current > previous
      ? static_cast<U>(current) - static_cast<U>(previous)
      : static_cast<U>(previous) - static_cast<U>(current);
  if (delta >= ldexp(1.0, sizeof(U) * CHAR_BIT))
    return false;
  return difference >= static_cast<U>(ceil(delta));
}

template<class T>
static bool _floatingSignalChanged(const void *address, const byte *baseline, double delta, double minNormal)
{
  T current, previous;
  memcpy(&current, address, sizeof(T));
  memcpy(&previous, baseline, sizeof(T));
  if (memcmp(&current, &previous, sizeof(T)) == 0)
    return false;
  // Like Arduino Cloud, stable NaN representations do not repeatedly publish.
  if (isnan(current) || isnan(previous) || isinf(current) || isinf(previous) ||
      (current != 0 && fabs(current) < minNormal) ||
      (previous != 0 && fabs(previous) < minNormal))
    return true;
  return fabs(current - previous) >= delta;
}

// Whether value differs from r's baseline by at least delta. Always true without a baseline.
static bool _valueChanged(const ReportingState &r, dataType type, const void *value, bool inFlash, double delta)
{
  if (!r.valid)
    return true;
  switch (type)
  {
  case Blaeck_bool: return memcmp(value, r.value, sizeof(bool)) != 0;
  case Blaeck_byte: return _integerSignalChanged<byte>(value, r.value, delta);
  case Blaeck_short: case Blaeck_int: return _integerSignalChanged<int16_t>(value, r.value, delta);
  case Blaeck_ushort: case Blaeck_uint: return _integerSignalChanged<uint16_t>(value, r.value, delta);
  case Blaeck_long: return _integerSignalChanged<int32_t>(value, r.value, delta);
  case Blaeck_ulong: return _integerSignalChanged<uint32_t>(value, r.value, delta);
  case Blaeck_longlong: return _integerSignalChanged<int64_t, uint64_t>(value, r.value, delta);
  case Blaeck_float: return _floatingSignalChanged<float>(value, r.value, delta, FLT_MIN);
  case Blaeck_double: return _floatingSignalChanged<double>(value, r.value, delta, DBL_MIN);
  case Blaeck_string:
  {
    const byte length = static_cast<byte>(_textLength(value, inFlash));
    return length != r.textLength || (length != 0 && !_textMatchesRam(value, inFlash, r.text, length));
  }
  default: return false;
  }
}

bool Blaeck::_signalChanged(const Signal &s, double delta) const
{
  return _valueChanged(*s.Reporting, s.DataType, s.Address, s.TextInFlash, delta);
}

bool Blaeck::_signalNameEquals(const Signal &s, const char *name, bool nameInFlash) const
{
  if (name == nullptr)
    return false;
  size_t at = 0;
  if (s.SignalName != nullptr)
  {
    byte c;
    while ((c = _textByte(s.SignalName, s.NameInFlash, at)) != 0)
    {
      if (_textByte(name, nameInFlash, at++) != c)
        return false;
    }
  }
  // The suffix isn't stored, so compare against its digits.
  if (s.HasSuffix)
  {
    char digits[4];
    byte n = _signalSuffixDigits(s, digits);
    for (byte i = 0; i < n; i++)
    {
      if (_textByte(name, nameInFlash, at++) != static_cast<byte>(digits[i]))
        return false;
    }
  }
  return _textByte(name, nameInFlash, at) == 0;
}

// The suffix as decimal digits, without a terminator. Returns the count; out must hold three.
byte Blaeck::_signalSuffixDigits(const Signal &s, char *out)
{
  uint8_t v = s.NameSuffix;
  byte n = 0;
  if (v >= 100)
    out[n++] = (char)('0' + (v / 100));
  if (v >= 10)
    out[n++] = (char)('0' + ((v / 10) % 10));
  out[n++] = (char)('0' + (v % 10));
  return n;
}

// Walks a name (flash or RAM, plus any suffix) and sends each byte to the frame or the hash,
// so both always see the same bytes.
void Blaeck::_emitSignalName(const Signal &s, NameSink sink)
{
  if (s.SignalName != nullptr)
  {
    if (s.NameInFlash)
    {
      PGM_P p = reinterpret_cast<PGM_P>(s.SignalName);
      byte c;
      while ((c = pgm_read_byte(p++)) != 0)
        _emitNameByte(c, sink);
    }
    else
    {
      const char *p = s.SignalName;
      while (*p)
        _emitNameByte((byte)*p++, sink);
    }
  }
  if (s.HasSuffix)
  {
    char digits[4];
    byte n = _signalSuffixDigits(s, digits);
    for (byte i = 0; i < n; i++)
      _emitNameByte((byte)digits[i], sink);
  }
}

void Blaeck::_emitNameByte(byte c, NameSink sink)
{
  switch (sink)
  {
  case NAME_SINK_FRAME:
    _emitByte(c);
    break;
  case NAME_SINK_DEBUG:
    if (_debugStream != nullptr)
      _debugStream->write(c);
    break;
  default:
    _schemaHashFeedByte(c);
    break;
  }
}

void Blaeck::_signalNameFeedHash(const Signal &s)
{
  _emitSignalName(s, NAME_SINK_HASH);
}

void Blaeck::_emitSignalName0(const Signal &s)
{
  _emitSignalName(s, NAME_SINK_FRAME);
  // One terminator after the prefix and suffix together.
  _emitByte(0);
}

bool blaeck_detail::optionsAccepted(BlaeckString optionsCsv, Print *debug,
                                    const char *name, bool nameInFlash)
{
  const bool empty = Blaeck::_flashCsvOptionCount(optionsCsv) == 0;
  if (!empty && !Blaeck::_flashCsvHasBlankField(optionsCsv))
    return true;

  if (debug != nullptr)
  {
    debug->print(F("Dropped '"));
    if (name != nullptr)
    {
      if (nameInFlash)
        debug->print(reinterpret_cast<const __FlashStringHelper *>(name));
      else
        debug->print(name);
    }
    debug->println(empty ? F("': it needs at least one option.")
                         : F("': one of its options is blank."));
  }
  return false;
}

// Store a value in a signal, converted to the signal's declared type. There are three
// because no one C++ type holds all the others: a double on AVR is 4 bytes and can't hold a
// long. False if there is no such signal, or it holds text.
//
// The integer cases go through the fixed-width type the protocol names, not the C++ one: a
// board's int is registered as Blaeck_long, which is four bytes, while long on a 64-bit host
// is eight. Writing through a long* there ran four bytes past a variable the size it says.
#define BLAECK_STORE_CASES(v)                                                                  \
  switch (Signals[signalIndex].DataType)                                                       \
  {                                                                                            \
  case (Blaeck_bool):   *((bool *)Signals[signalIndex].Address)           = ((v) != 0); break; \
  case (Blaeck_byte):   *((byte *)Signals[signalIndex].Address)           = (byte)(v); break;  \
  case (Blaeck_short): case (Blaeck_int):                                                      \
    { int16_t x = (int16_t)(v); memcpy(Signals[signalIndex].Address, &x, 2); } break;          \
  case (Blaeck_ushort): case (Blaeck_uint):                                                    \
    { uint16_t x = (uint16_t)(v); memcpy(Signals[signalIndex].Address, &x, 2); } break;        \
  case (Blaeck_long):                                                                          \
    { int32_t x = (int32_t)(v); memcpy(Signals[signalIndex].Address, &x, 4); } break;          \
  case (Blaeck_ulong):                                                                         \
    { uint32_t x = (uint32_t)(v); memcpy(Signals[signalIndex].Address, &x, 4); } break;        \
  case (Blaeck_float):  *((float *)Signals[signalIndex].Address)          = (float)(v); break; \
  case (Blaeck_double): *((double *)Signals[signalIndex].Address)         = (double)(v); break;\
  case (Blaeck_longlong): *((long long *)Signals[signalIndex].Address)    = (long long)(v); break; \
  default: return false;                                                                       \
  }                                                                                            \
  return true;

bool Blaeck::_storeSigned(int signalIndex, long value)
{
  if (signalIndex < 0 || signalIndex >= _signalIndex)
    return false;
  BLAECK_STORE_CASES(value)
}

bool Blaeck::_storeUnsigned(int signalIndex, unsigned long value)
{
  if (signalIndex < 0 || signalIndex >= _signalIndex)
    return false;
  BLAECK_STORE_CASES(value)
}

bool Blaeck::_storeLongLong(int signalIndex, long long value)
{
  if (signalIndex < 0 || signalIndex >= _signalIndex)
    return false;
  BLAECK_STORE_CASES(value)
}

bool Blaeck::_storeFloating(int signalIndex, double value)
{
  if (signalIndex < 0 || signalIndex >= _signalIndex)
    return false;
  BLAECK_STORE_CASES(value)
}

#undef BLAECK_STORE_CASES

int Blaeck::_findSignalIndex(byte deviceId, const char *signalName)
{
  for (int i = 0; i < _signalIndex; i++)
  {
    if (Signals[i].DeviceId == deviceId && _signalNameEquals(Signals[i], signalName))
    {
      return i;
    }
  }
  return -1; // Not found
}

int Blaeck::_findSignalIndex(byte deviceId, const __FlashStringHelper *signalName)
{
  for (int i = 0; i < _signalIndex; ++i)
    if (Signals[i].DeviceId == deviceId
        && _signalNameEquals(Signals[i], reinterpret_cast<const char *>(signalName), true))
      return i;
  return -1;
}

void Blaeck::read()
{
  this->writeRestarted();
  _writeDeviceNotices();

  if (_receiveCommand())
  {
    // Parsed once, for both the built-ins and the registered handlers.
    _parseCommandTokens(_receiver.chars);
    // Before the truncation check, so even a cut-off built-in counts.
    if (strncmp(_parsedCommand, "BLAECK.", 7) == 0)
      _builtinCommandReceived();
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("<"));
      _debugStream->print(_receiver.chars);
      _debugStream->println(F(">"));
    }

    // A command that didn't arrive whole must not run, built-in or not.
    if (_parsedTruncated)
    {
      _writeCommandAck(_receiver.chars, 1, BLAECK_ACK_TRUNCATED);
    }
    else
    {
      // Acknowledge before replying, so a host can tell the request arrived. The handler dispatch
      // below then doesn't acknowledge again.
      bool builtinMatched = true;
      const unsigned long msg_id = _parsedPrefixMsgId;

      if (equalsFlash(_parsedCommand, F(BLAECK_BUILTIN_WRITE_DATA)))
      {
        _writeCommandAck(_receiver.chars, 0, BLAECK_ACK_OK);
        // Marks the data frame as a reply to a request.
        _frameRequested = true;
        this->writeAll(msg_id, getTimeStamp());
        _frameRequested = false;
      }
      else if (equalsFlash(_parsedCommand, F(BLAECK_BUILTIN_GET_DEVICES)))
      {
        _writeCommandAck(_receiver.chars, 0, BLAECK_ACK_OK);
        this->writeDevices(msg_id);
      }
      else if (equalsFlash(_parsedCommand, F(BLAECK_BUILTIN_WRITE_ENTITIES)))
      {
        _writeCommandAck(_receiver.chars, 0, BLAECK_ACK_OK);
        this->writeEntities(msg_id);
      }
      else if (equalsFlash(_parsedCommand, F(BLAECK_BUILTIN_ACTIVATE)))
      {
        // strtoul, because atoi is 16-bit on AVR.
        unsigned long timedInterval_ms = 0;
        if (_parsedParamCount > 0 && _parsedParamPtrs[0] != nullptr)
          timedInterval_ms = strtoul(_parsedParamPtrs[0], nullptr, 10);
        _writeCommandAck(_receiver.chars, 0, BLAECK_ACK_OK);
        this->_setTimedDataState(true, timedInterval_ms);
      }
      else if (equalsFlash(_parsedCommand, F(BLAECK_BUILTIN_DEACTIVATE)))
      {
        _writeCommandAck(_receiver.chars, 0, BLAECK_ACK_OK);
        this->_setTimedDataState(false, _timedInterval_ms);
      }
      else if (equalsFlash(_parsedCommand, F(BLAECK_BUILTIN_PAUSE_WRITES)))
      {
        // Check for the word first; strtoul would read it as 0, the default duration.
        bool forever = _parsedParamCount > 0 && _parsedParamPtrs[0] != nullptr &&
                       equalsFlash(_parsedParamPtrs[0], F(BLAECK_PAUSE_WRITES_FOREVER));

        unsigned long pause_ms = 0;
        if (!forever && _parsedParamCount > 0 && _parsedParamPtrs[0] != nullptr)
          pause_ms = strtoul(_parsedParamPtrs[0], nullptr, 10);
        // Acknowledge before pausing, or the ack itself would be held back.
        _writeCommandAck(_receiver.chars, 0, BLAECK_ACK_OK);

        if (forever)
          this->_setWritesPausedForever();
        else
          this->_setWritesPaused(pause_ms);
      }
      else if (equalsFlash(_parsedCommand, F(BLAECK_BUILTIN_RESUME_WRITES)))
      {
        _clearWritesPaused();
        _writeCommandAck(_receiver.chars, 0, BLAECK_ACK_OK);
      }
      else
      {
        builtinMatched = false;
      }

      _dispatchRegisteredHandlers(!builtinMatched);
    }
  }

  // Send any catalog a handler changed.
  _flushCatalogs();
}

void Blaeck::onBeforeWrite(void (*callback)())
{
  _beforeWriteCallback = callback;
}

Blaeck &Blaeck::withName(BlaeckString name)
{
  // A sub-device already holds the name: the board keeps the one it has.
  for (byte i = 0; i < _deviceCount; ++i)
  {
    if (name != nullptr && name.read(0) != 0 && BlaeckString(_devices[i].name) == name)
    {
      if (_debugStream != nullptr)
      {
        _debugStream->print(F("withName(): a device from addDevice() is named "));
        name.printTo(*_debugStream);
        _debugStream->println(F(" already; the board keeps its name."));
      }
      ++_rejectedDeviceCount;
      return *this;
    }
  }
  _storeString(_boardName, name);
  return *this;
}

Blaeck &Blaeck::withHWVersion(BlaeckString hwVersion)
{
  _storeString(_boardHW, hwVersion);
  return *this;
}

Blaeck &Blaeck::withFWVersion(BlaeckString fwVersion)
{
  _storeString(_boardFW, fwVersion);
  return *this;
}

int Blaeck::_registerCommand(byte deviceId, const char *command, BlaeckCommandHandler handler,
                             BlaeckButtonFunction press)
{
#if !BLAECK_ENABLE_IOT
  // Buttons belong to the IoT part; without it they store nothing.
  if (press != nullptr)
    return -1;
#endif
  if (command == nullptr || (handler == nullptr && press == nullptr) || command[0] == '\0')
  {
    _rejectedCommandCount++;
    return -1;
  }
  if (strlen(command) >= MAX_COMMAND_NAME_COUNT)
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Command name too long for handler table: "));
      _debugStream->println(command);
    }
    _rejectedCommandCount++;
    return -1;
  }
  // Empty, reserved, starting with '#' or held by an input or sensor.
  if (_nameRefused(BlaeckString(command), true))
  {
    _rejectedCommandCount++;
    return -1;
  }

  // Registering a name again replaces the entry, wherever it was.
  int slot = -1;
  for (uint16_t i = 0; i < _commandSlots(); i++)
  {
    if (_commandHandlers[i].inUse && strcmp(_commandHandlers[i].command, command) == 0)
    {
      slot = (int)i;
      break;
    }
  }
  if (slot < 0)
  {
    if (!_roomFor(_commandHandlers))
    {
      _warnNoRoom(F("command"), command);
      _rejectedCommandCount++;
      return -1;
    }
    for (uint16_t i = 0; i < _commandSlots(); i++)
    {
      if (!_commandHandlers[i].inUse)
      {
        slot = (int)i;
        break;
      }
    }
    if (slot < 0)
    {
      _warnNoRoom(F("command"), command);
      _rejectedCommandCount++;
      return -1;
    }
  }

  CommandHandlerEntry &e = _commandHandlers[slot];
  // Only buttons are listed, so only a button coming or going changes the entity list.
  if (e.press != nullptr || press != nullptr)
    _entityCatalogDirty = true;
  _resetCommand(e);
  strncpy(e.command, command, MAX_COMMAND_NAME_COUNT - 1);
  e.command[MAX_COMMAND_NAME_COUNT - 1] = '\0';
  e.handler = handler;
  e.press = press;
  e.inUse = true;
  // Names are unique per board, so registering one through another device's handle moves it
  // there.
  e.deviceId = deviceId;
  return slot;
}

// Empties an entry, so nothing carries over from an earlier registration.
void Blaeck::_resetCommand(CommandHandlerEntry &e)
{
  e.inUse = false;
  e.handler = nullptr;
  e.press = nullptr;
  e.command[0] = '\0';
  e.category = BLAECK_CAT_NONE;
  e.disabledByDefault = false;
  e.displayName = nullptr;
  e.deviceClass = nullptr;
  e.icon = nullptr;
}

void BlaeckDeviceBase::onCommand(const char *command, BlaeckCommandHandler handler)
{
  _registerCommand(command, handler, nullptr);
}

void Blaeck::onAnyCommand(BlaeckAnyCommandHandler handler)
{
  _anyCommandHandler = handler;
}

void Blaeck::clearAllCommands()
{
  // Plain commands only: a button is a control, and goes with clearAllControls().
  for (uint16_t i = 0; i < _commandSlots(); i++)
    if (_commandHandlers[i].inUse && _commandHandlers[i].press == nullptr)
      _resetCommand(_commandHandlers[i]);
  _anyCommandHandler = nullptr;
}

void Blaeck::clearAllControls()
{
  for (uint16_t i = 0; i < _commandSlots(); i++)
    if (_commandHandlers[i].inUse && _commandHandlers[i].press != nullptr)
    {
      _resetCommand(_commandHandlers[i]);
      _entityCatalogDirty = true;
    }
  _clearProperties(true);
}

void Blaeck::clearAllSensors()
{
  _clearProperties(false);
}

void Blaeck::_clearProperties(bool writable)
{
#if BLAECK_ENABLE_IOT
  // A property is numbered by its position, so the ones kept close the gap: each moves down,
  // taking what it owns along. A slot left behind is empty and reused by the next add.
  uint16_t kept = 0;
  for (uint16_t i = 0; i < _propertyCount; ++i)
  {
    PropertyEntry &p = _properties[i];
    if (p.writable == writable)
    {
      _resetProperty(p);
      _entityCatalogDirty = true;
      continue;
    }
    if (kept != i)
      _moveProperty(_properties[kept], p);
    ++kept;
  }
  _propertyCount = kept;
#else
  (void)writable;
#endif
}

void Blaeck::_resetProperty(PropertyEntry &p)
{
  delete p.reporting;
  delete p.presentation;
  p.reporting = nullptr;
  p.presentation = nullptr;
  p.name = nullptr;
  p.options = nullptr;
  p.address = nullptr;
  p.getter = nullptr;
  p.callback = nullptr;
  p.flags = 0;
  p.textSize = 0;
  p.type = Blaeck_float;
  // The member the type makes the live one, so nothing reads the other.
  p.rangeMin = p.rangeMax = p.rangeStep = _makeBound(p.type, 0.0);
  p.kind = BLAECK_VALUE_NUMBER;
  p.deviceId = 0;
  p.getterType = 0;
  p.writable = false;
}

// Moves everything from a property into an empty slot and leaves the source empty.
void Blaeck::_moveProperty(PropertyEntry &to, PropertyEntry &from)
{
  to.name = from.name;
  to.options = from.options;
  to.address = from.address;
  to.getter = from.getter;
  to.callback = from.callback;
  to.rangeMin = from.rangeMin;
  to.rangeMax = from.rangeMax;
  to.rangeStep = from.rangeStep;
  to.reporting = from.reporting;
  to.presentation = from.presentation;
  to.flags = from.flags;
  to.textSize = from.textSize;
  to.type = from.type;
  to.kind = from.kind;
  to.deviceId = from.deviceId;
  to.getterType = from.getterType;
  to.writable = from.writable;
  from.reporting = nullptr;
  from.presentation = nullptr;
  _resetProperty(from);
}

bool Blaeck::_storeString(detail::StoredString &slot, BlaeckString value)
{
  if (slot.set(value))
    return true;
  if (_rejectedStringCount != UINT16_MAX)
    ++_rejectedStringCount;
  if (_debugStream != nullptr)
    _debugStream->println(F("No RAM for configuration text; previous value retained."));
  return false;
}

BlaeckButtonRef BlaeckDeviceBase::addButton(const char *name, BlaeckButtonFunction press)
{
  // A button always has a function; without one it would be a plain command.
  if (press == nullptr)
  {
    if (_core != nullptr)
      _core->_rejectedCommandCount++;
    return BlaeckButtonRef(_core, -1);
  }
  return BlaeckButtonRef(_core, (int16_t)_registerCommand(name, nullptr, press));
}

uint16_t Blaeck::_flashCsvOptionCount(BlaeckString csv)
{
  if (csv == nullptr)
    return 0;
  size_t at = 0;
  uint16_t count = 1;
  bool any = false;
  byte c;
  while ((c = csv.read(at++)) != 0)
  {
    any = true;
    if (c == ',')
      count++;
  }
  return any ? count : 0;
}

// True for a field that is empty or only spaces, which a host couldn't show or offer.
bool Blaeck::_flashCsvHasBlankField(BlaeckString csv)
{
  if (csv == nullptr)
    return true;
  size_t at = 0;
  bool fieldHasContent = false;
  byte c;
  while ((c = csv.read(at++)) != 0)
  {
    if (c == ',')
    {
      if (!fieldHasContent)
        return true;
      fieldHasContent = false;
    }
    else if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
    {
      fieldHasContent = true;
    }
  }
  // The last field, and an empty string.
  return !fieldHasContent;
}

long Blaeck::getSelectOptionIndexOf(BlaeckString name, const char *optionName) const
{
  const int index = _findProperty(name);
  if (index < 0 || optionName == nullptr || _properties[index].kind != BLAECK_VALUE_ENUM)
    return -1;
  // The same match as a value a host sends.
  return _flashCsvIndexOf(_properties[index].options, optionName);
}

bool Blaeck::getSelectOptionNameAt(BlaeckString name, byte index, char *out, byte outSize) const
{
  if (out == nullptr || outSize == 0)
    return false;
  out[0] = '\0';
  const int property = _findProperty(name);
  if (property < 0 || _properties[property].kind != BLAECK_VALUE_ENUM)
    return false;

  // Skip `index` commas, then copy up to the next one.
  BlaeckString p = _properties[property].options;
  byte seen = 0;
  unsigned int at = 0;
  while (seen < index)
  {
    byte c = p.read(at);
    if (c == 0)
      return false; // fewer options than the index asked for
    if (c == ',')
      seen++;
    at++;
  }

  byte len = 0;
  byte c;
  while ((c = p.read(at + len)) != 0 && c != ',')
  {
    // A shortened name would match no option, so fail instead.
    if ((unsigned int)len + 1 >= outSize)
    {
      out[0] = '\0';
      return false;
    }
    out[len] = (char)c;
    len++;
  }
  out[len] = '\0';
  return len > 0;
}

long Blaeck::_flashCsvIndexOf(BlaeckString csv, const char *value)
{
  if (csv == nullptr || value == nullptr || value[0] == '\0')
    return -1;

  size_t at = 0;
  long index = 0;
  const char *v = value;
  bool matching = true; // current token still matches value so far

  byte c;
  while (true)
  {
    c = csv.read(at++);
    if (c == ',' || c == '\0')
    {
      // End of a token: match if value was fully consumed too.
      if (matching && *v == '\0')
        return index;
      if (c == '\0')
        return -1;
      // Advance to next token
      index++;
      v = value;
      matching = true;
    }
    else
    {
      if (matching)
      {
        // Case-sensitive, so options differing only in case stay distinct.
        if (*v == '\0' || (char)c != *v)
          matching = false;
        else
          v++;
      }
    }
  }
}

bool Blaeck::_receiveByte(Receiver &r, char rc)
{
  const char startMarker = '<';
  const char endMarker = '>';

  if (r.inProgress)
  {
    if (rc == startMarker)
    {
      // A second '<' abandons the unfinished command. Otherwise a command cut off mid-way would
      // swallow the next one.
      r.ndx = 0;
      r.overflowed = false;
    }
    else if (rc != endMarker)
    {
      r.chars[r.ndx] = rc;
      r.ndx++;
      if (r.ndx >= MAXIMUM_CHAR_COUNT)
      {
        // Buffer full: the rest of the command is dropped. Flag it now, because after parsing it
        // would look like a valid shorter command.
        r.ndx = MAXIMUM_CHAR_COUNT - 1;
        r.overflowed = true;
      }
    }
    else
    {
      r.chars[r.ndx] = '\0';
      r.inProgress = false;
      r.ndx = 0;
      return true;
    }
  }
  else if (rc == startMarker)
  {
    r.inProgress = true;
    r.overflowed = false;
  }
  return false;
}

bool Blaeck::_setChannelName(const char *&slot, bool &inFlash, const char *ram, const __FlashStringHelper *flash)
{
  // Free the copy the slot held; a flash name owns nothing.
  if (slot != nullptr && !inFlash)
    free((void *)slot);
  slot = nullptr;
  inFlash = false;

  if (flash != nullptr)
  {
    slot = reinterpret_cast<const char *>(flash);
    inFlash = true;
    return true;
  }
  if (ram == nullptr)
    return true;

  // Copied, so the caller's buffer is free the moment this returns.
  size_t needed = strlen(ram) + 1;
  char *copy = (char *)malloc(needed);
  if (copy != nullptr)
  {
    memcpy(copy, ram, needed);
    slot = copy;
    return true;
  }
  return false;
}

bool Blaeck::_channelNameEquals(const char *stored, bool inFlash, const char *candidate)
{
  if (stored == nullptr || candidate == nullptr)
    return false;
  if (!inFlash)
    return strcmp(stored, candidate) == 0;
  return equalsFlash(candidate, reinterpret_cast<const __FlashStringHelper *>(stored));
}

bool Blaeck::_channelNameEqualsFlash(const char *stored, bool inFlash, const __FlashStringHelper *candidate)
{
  if (stored == nullptr || candidate == nullptr)
    return false;
  if (!inFlash)
    return equalsFlash(stored, candidate);

  // Both in flash, so both are read with pgm_read_byte.
  PGM_P a = reinterpret_cast<PGM_P>(stored);
  PGM_P b = reinterpret_cast<PGM_P>(candidate);
  byte ca, cb;
  do
  {
    ca = pgm_read_byte(a++);
    cb = pgm_read_byte(b++);
    if (ca != cb)
      return false;
  } while (ca != 0);
  return true;
}

byte Blaeck::copyFlashName(const __FlashStringHelper *flash, char *out, byte outSize)
{
  if (out == nullptr || outSize == 0)
    return 0;

  byte len = 0;
  if (flash != nullptr)
  {
    PGM_P p = reinterpret_cast<PGM_P>(flash);
    byte c;
    while ((c = pgm_read_byte(p + len)) != 0 && len + 1 < outSize)
    {
      out[len] = (char)c;
      len++;
    }
  }
  // Always terminated, even for a null or overlong name.
  out[len] = '\0';
  return len;
}

char *Blaeck::toText(float value, byte decimals, char *out, byte outSize)
{
  if (out == nullptr || outSize == 0)
    return out;
  out[0] = '\0';

  // Said in words rather than digits, because no digits are right.
  const char *word = nullptr;
  if (isnan(value))
    word = "nan";
  else if (isinf(value))
    word = "inf";
  else if (value > 4294967040.0f || value < -4294967040.0f)
    word = "ovf"; // past what the unsigned long below can hold
  if (word != nullptr)
  {
    byte w = 0;
    while (word[w] != '\0' && w + 1 < outSize)
    {
      out[w] = word[w];
      w++;
    }
    out[w] = '\0';
    return out;
  }

  byte at = 0;
  if (value < 0.0f)
  {
    if (at + 1 < outSize)
      out[at++] = '-';
    value = -value;
  }

  // Round before splitting, so 9.999 at two decimals becomes 10.00, not 9.100.
  float rounding = 0.5f;
  for (byte i = 0; i < decimals; i++)
    rounding /= 10.0f;
  value += rounding;

  unsigned long whole = (unsigned long)value;
  float frac = value - (float)whole;

  // Digits come out lowest first, so they are written back to front.
  char digits[11];
  byte n = 0;
  do
  {
    digits[n++] = (char)('0' + (whole % 10UL));
    whole /= 10UL;
  } while (whole > 0UL && n < sizeof(digits));
  while (n > 0 && at + 1 < outSize)
    out[at++] = digits[--n];

  if (decimals > 0 && at + 1 < outSize)
    out[at++] = '.';
  for (byte i = 0; i < decimals && at + 1 < outSize; i++)
  {
    frac *= 10.0f;
    byte d = (byte)frac;
    out[at++] = (char)('0' + d);
    frac -= d;
  }
  out[at] = '\0';
  return out;
}

bool Blaeck::equalsFlash(const char *ram, const __FlashStringHelper *flash)
{
  if (ram == nullptr || flash == nullptr)
    return false;

  PGM_P p = reinterpret_cast<PGM_P>(flash);
  byte c;
  while ((c = pgm_read_byte(p++)) != 0)
  {
    if (*ram++ != (char)c)
      return false;
  }
  // Both ended together, or the RAM side is the longer of the two.
  return *ram == '\0';
}

void Blaeck::_parseCommandTokens(const char *raw)
{
  _parsedCommand[0] = '\0';
  _parsedParamCount = 0;
  _parsedPrefixMsgId = 0;
  _parsedPrefixLen = 0;
  // Characters were lost while receiving, so this is a fragment. Reset here, before the
  // empty-command check, so an empty command doesn't keep the previous verdict.
  _parsedTruncated = _receiver.overflowed;
  for (byte i = 0; i < MAX_COMMAND_PARAM_COUNT; i++)
  {
    _parsedParamPtrs[i] = nullptr;
  }

  if (raw == nullptr || raw[0] == '\0')
  {
    return;
  }

  strncpy(_parsedTokenBuffer, raw, sizeof(_parsedTokenBuffer) - 1);
  _parsedTokenBuffer[sizeof(_parsedTokenBuffer) - 1] = '\0';

  // Split on commas by hand, so empty fields between commas are kept.
  char *p = _parsedTokenBuffer;

  // The prefix: the message id, "#<id>:", at most one. A malformed or second one stays part of
  // the name, which can't start with '#', so the command is answered as unknown.
  if (*p == '#')
  {
    const char *scan = p + 1;
    uint32_t id = 0;
    byte digits = 0;
    while (*scan >= '0' && *scan <= '9' && digits < 5)
    {
      id = id * 10UL + (uint32_t)(*scan - '0');
      scan++;
      digits++;
    }
    // 0 means no id, so "#0:" is malformed.
    if (digits > 0 && *scan == ':' && id != 0 && id <= 65535UL)
    {
      _parsedPrefixMsgId = (uint16_t)id;
      p = (char *)scan + 1;
    }
  }
  // The ack hashes the command after the prefix, as its sender wrote it.
  _parsedPrefixLen = static_cast<uint16_t>(p - _parsedTokenBuffer);

  // The command name is everything before the first comma.
  char *tokenStart = p;
  while (*p != ',' && *p != '\0')
    p++;
  bool hasComma = (*p == ',');
  if (hasComma)
  {
    *p = '\0';
    p++;
  }
  if (tokenStart[0] == '\0')
  {
    return;
  }
  strncpy(_parsedCommand, tokenStart, MAX_PARSED_COMMAND_COUNT - 1);
  _parsedCommand[MAX_PARSED_COMMAND_COUNT - 1] = '\0';

  if (!hasComma)
    return;

  // Parameters. An empty field (,,) gives a pointer to an empty string.
  bool moreParams = true;
  while (moreParams && _parsedParamCount < MAX_COMMAND_PARAM_COUNT)
  {
    tokenStart = p;
    while (*p != ',' && *p != '\0')
      p++;
    if (*p == ',')
    {
      *p = '\0';
      p++;
    }
    else
    {
      moreParams = false;
    }
    _parsedParamPtrs[_parsedParamCount] = tokenStart;
    _parsedParamCount++;
  }

  // Out of parameter slots but more commas follow: the list was cut short.
  if (moreParams)
    _parsedTruncated = true;

  // Every parameter may be percent-encoded, so a value can hold ',', '<', '>' and any byte.
  // Decoded once here, for inputs, selects and plain commands alike.
  for (byte i = 0; i < _parsedParamCount; i++)
    _percentDecodeInPlace(const_cast<char *>(_parsedParamPtrs[i]));
}

void Blaeck::_dispatchRegisteredHandlers(bool sendAck)
{
  if (_parsedCommand[0] == '\0')
  {
    return;
  }

  // read() rejects a truncated command before this is reached.

  byte ackStatus = 1;                  // 0 = accepted, 1 = rejected
  byte ackReason = BLAECK_ACK_UNKNOWN; // reason reported when rejected
  bool matched = false;
  int propertySet = -1;

  // An input or sensor: the value is checked and stored here; its callback and the new value
  // follow the ack.
  const int property = _findProperty(BlaeckString(_parsedCommand));
  if (property >= 0)
  {
    matched = true;
    ackReason = _receiveProperty((uint16_t)property);
    if (ackReason == BLAECK_ACK_OK)
    {
      ackStatus = 0;
      propertySet = property;
    }
  }

  for (uint16_t i = 0; !matched && i < _commandSlots(); i++)
  {
    const CommandHandlerEntry &e = _commandHandlers[i];
    if (e.inUse && strcmp(e.command, _parsedCommand) == 0)
    {
      matched = true;
      // Refused for a missing device, so its handler doesn't forward to nothing.
      if (_deviceMissing(e.deviceId))
        ackReason = BLAECK_ACK_DEVICE_NOT_RESPONDING;
      else
        ackReason = BLAECK_ACK_OK;
      if (ackReason == BLAECK_ACK_OK)
      {
        ackStatus = 0;
        // A button ignores any parameters sent with the press.
        if (e.press != nullptr)
          e.press();
        else
          e.handler(_parsedCommand, (const char *const *)_parsedParamPtrs, _parsedParamCount);
      }
      break;
    }
  }

  if (_anyCommandHandler != nullptr)
  {
    const bool took = _anyCommandHandler(
        _parsedCommand,
        (const char *const *)_parsedParamPtrs,
        _parsedParamCount);

    // A command nothing else has is accepted only if the catch-all says it took it, so a handler
    // that only logs leaves a typo answered as unknown.
    if (!matched && took)
    {
      matched = true;
      ackStatus = 0;
      ackReason = BLAECK_ACK_OK;
    }
  }

  // Every command gets one ack. A matched built-in was acknowledged in read() and passes
  // sendAck false; an unknown BLAECK.* name arrives with sendAck true and is answered UNKNOWN.
  if (sendAck)
  {
    _writeCommandAck(_receiver.chars, ackStatus, ackReason);
  }

  if (propertySet >= 0)
  {
    PropertyEntry &p = _properties[propertySet];
    if (p.callback != nullptr)
      p.callback();
    _writePropertyFrame((uint16_t)propertySet);
  }
}

uint32_t Blaeck::_fnv1a32(const char *s)
{
  uint32_t h = 0x811C9DC5UL; // FNV offset basis
  if (s != nullptr)
  {
    while (*s != '\0')
    {
      h ^= (uint8_t)(*s++);
      h *= 0x01000193UL; // FNV prime
    }
  }
  return h;
}

void Blaeck::_writeCommandAck(const char *rawCommand, byte status, byte reasonCode)
{
  if (!_requesterIsHost() || !_mayWriteFrame())
    return;

  // The name hash still identifies a command that didn't arrive whole, since the name comes
  // before the first comma.
  uint32_t nameHash = (_parsedCommand[0] == '\0') ? 0UL : _fnv1a32(_parsedCommand);

  // Hash what follows the prefix. The length check can't fail after the parse above, but guards
  // against reading past the end.
  const char *payload = rawCommand;
  if (payload != nullptr && _parsedPrefixLen > 0 && strlen(payload) >= _parsedPrefixLen)
    payload += _parsedPrefixLen;

  // The ack carries the message id from the command's prefix (0 if none), so a host can tell
  // which of two same-named commands it answers.
  uint32_t ackMsgId = (uint32_t)_parsedPrefixMsgId;

  if (!_frameOpen(0xA5, ackMsgId))
    return;
  // Command hash (4 bytes, little-endian), name hash (4), status (1), reason (1).
  ulngCvt.val = _fnv1a32(payload);
  _emitBytes(ulngCvt.bval, 4);
  ulngCvt.val = nameHash;
  _emitBytes(ulngCvt.bval, 4);
  _emitByte(status);
  _emitByte(reasonCode);
  _frameClose();
}

bool Blaeck::_flashStringEqualsName(const __FlashStringHelper *flashName, const char *name)
{
  if (flashName == nullptr || name == nullptr)
    return false;

  PGM_P p = reinterpret_cast<PGM_P>(flashName);
  unsigned int i = 0;
  for (;; i++)
  {
    byte a = pgm_read_byte(p + i);
    char b = name[i];
    if (a != (byte)b)
      return false;
    if (a == 0)
      return true;
  }
}

#if BLAECK_ENABLE_IOT
int Blaeck::_registerEventChannel(byte deviceId, const char *channelName, const __FlashStringHelper *flashName, BlaeckString eventTypes)
{
  char probe[2];
  bool emptyFlash = flashName != nullptr && copyFlashName(flashName, probe, sizeof(probe)) == 0;
  if ((channelName == nullptr && flashName == nullptr) || emptyFlash ||
      (channelName != nullptr && channelName[0] == '\0'))
  {
    _rejectedEventChannelCount++;
    return -1;
  }

  // An event needs at least one type, and none may be blank.
  if (eventTypes == nullptr || _flashCsvOptionCount(eventTypes) == 0)
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Event needs at least one event type: "));
      _debugStream->println(channelName);
    }
    _rejectedEventChannelCount++;
    return -1;
  }

  if (_flashCsvHasBlankField(eventTypes))
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Event has a blank event type: "));
      _debugStream->println(channelName);
    }
    _rejectedEventChannelCount++;
    return -1;
  }

  if (_hasControlChar(flashName != nullptr ? BlaeckString(flashName) : BlaeckString(channelName)))
  {
    if (_debugStream != nullptr)
      _debugStream->println(F("Dropped an event whose name holds a control character."));
    _rejectedEventChannelCount++;
    return -1;
  }

  if (channelName != nullptr && strlen(channelName) >= MAX_EVENT_NAME_COUNT)
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Event name too long: "));
      _debugStream->println(channelName);
    }
    _rejectedEventChannelCount++;
    return -1;
  }

  // Declaring an existing name reuses its slot with the metadata cleared. Its event types are
  // kept, so their indices don't change.
  int existing = flashName != nullptr ? _findEventChannel(deviceId, flashName) : _findEventChannel(deviceId, channelName);
  if (existing >= 0)
  {
    _eventChannels[existing].displayName = nullptr;
    _eventChannels[existing].icon = nullptr;
    _eventChannels[existing].deviceClass = nullptr;
    _eventChannels[existing].category = 0;
    _eventChannels[existing].disabledByDefault = false;
    _entityCatalogDirty = true;
    return existing;
  }

  detail::StoredString storedTypes;
  if (!_storeString(storedTypes, eventTypes))
  {
    ++_rejectedEventChannelCount;
    return -1;
  }

  if (!_roomFor(_eventChannels))
  {
    if (flashName != nullptr)
      _warnNoRoom(F("event"), flashName);
    else
      _warnNoRoom(F("event"), channelName);
    _rejectedEventChannelCount++;
    return -1;
  }

  for (uint16_t i = 0; i < _eventChannelSlots(); i++)
  {
    if (!_eventChannels[i].inUse)
    {
      if (!_setChannelName(_eventChannels[i].name, _eventChannels[i].nameInFlash, channelName, flashName))
      {
        if (_debugStream != nullptr)
        {
          _debugStream->print(F("No RAM for the event name: "));
          _debugStream->println(channelName);
        }
        _rejectedEventChannelCount++;
        return -1;
      }
      _eventChannels[i].displayName = nullptr;
      _eventChannels[i].icon = nullptr;
      _eventChannels[i].deviceClass = nullptr;
      _eventChannels[i].category = 0;
      _eventChannels[i].disabledByDefault = false;
      _eventChannels[i].inUse = true;
      _eventChannels[i].deviceId = deviceId;
      _addEventTypesCsv(i, storedTypes);
      _entityCatalogDirty = true;
      return (int)i;
    }
  }

  _warnNoRoom(F("event"), channelName);
  _rejectedEventChannelCount++;
  return -1;
}

void Blaeck::_addEventTypesCsv(uint16_t channelIndex, const detail::StoredString &eventTypes)
{
  // One entry per field, all pointing at the same string, in order.
  uint16_t fieldCount = _flashCsvOptionCount(eventTypes);
  for (uint16_t f = 0; f < fieldCount; f++)
  {
    if (_eventTypeCount >= MAX_TABLE_ENTRIES || !_eventTypes.reserve(_eventTypeCount + 1))
    {
      if (_eventChannels[channelIndex].nameInFlash)
        _warnNoRoom(F("event type"),
                    reinterpret_cast<const __FlashStringHelper *>(_eventChannels[channelIndex].name));
      else
        _warnNoRoom(F("event type"), _eventChannels[channelIndex].name);
      // The remaining fields are dropped too.
      _rejectedEventTypeCount += (uint16_t)(fieldCount - f);
      break;
    }
    _eventTypes[_eventTypeCount].channelIndex = channelIndex;
    _eventTypes[_eventTypeCount].text = eventTypes;
    _eventTypes[_eventTypeCount].field = f;
    _eventTypeCount++;
  }
}

void Blaeck::_eventTypeExtent(const EventTypeEntry &e, unsigned int &start, unsigned int &len)
{
  start = 0;
  len = 0;
  if (e.text == nullptr)
    return;

  BlaeckString p = e.text;
  if (e.field == WHOLE_STRING)
  {
    while (p.read(len) != 0)
      len++;
    return;
  }

  // Skip `field` commas, then measure to the next comma or the end.
  byte seen = 0;
  unsigned int i = 0;
  while (seen < e.field)
  {
    byte c = p.read(i);
    if (c == 0)
      return; // fewer fields than expected: empty extent
    if (c == ',')
      seen++;
    i++;
  }
  start = i;
  byte c;
  while ((c = p.read(start + len)) != 0 && c != ',')
    len++;
}

void Blaeck::_emitEventType0(const EventTypeEntry &e)
{
  unsigned int start, len;
  _eventTypeExtent(e, start, len);
  BlaeckString p = e.text;
  for (unsigned int i = 0; i < len; i++)
    _emitByte(p.read(start + i));
  _emitByte(0);
}

bool Blaeck::_eventTypeEquals(const EventTypeEntry &e, BlaeckString eventType)
{
  if (e.text == nullptr || eventType == nullptr)
    return false;

  unsigned int start, len;
  _eventTypeExtent(e, start, len);

  BlaeckString a = e.text;
  BlaeckString b = eventType;
  for (unsigned int i = 0; i < len; i++)
  {
    byte bc = b.read(i);
    if (bc == 0 || a.read(start + i) != bc)
      return false;
  }
  // Equal only if eventType ends where the field does.
  return b.read(len) == 0;
}

bool Blaeck::_addEventType(byte deviceId, const char *channelName, BlaeckString eventType)
{
  if (eventType == nullptr)
    return false;

  // A blank type is refused, as in addEvent().
  if (_flashCsvHasBlankField(eventType))
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Blank event type dropped on event: "));
      _debugStream->println(channelName != nullptr ? channelName : "");
    }
    _rejectedEventTypeCount++;
    return false;
  }

  int channelIndex = _findEventChannel(deviceId, channelName);
  if (channelIndex < 0)
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Event type dropped, no event added with addEvent(): "));
      _debugStream->println(channelName != nullptr ? channelName : "");
    }
    _rejectedEventTypeCount++;
    return false;
  }

  // A duplicate could never be reported: writeEvent() would always find the first.
  if (_findEventType((byte)channelIndex, eventType) >= 0)
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Duplicate event type ignored on event: "));
      _debugStream->println(channelName);
    }
    return false;
  }

  if (_eventTypeCount >= MAX_TABLE_ENTRIES || !_eventTypes.reserve(_eventTypeCount + 1))
  {
    _warnNoRoom(F("event type"), channelName);
    _rejectedEventTypeCount++;
    return false;
  }

  _eventTypes[_eventTypeCount].channelIndex = (byte)channelIndex;
  if (!_storeString(_eventTypes[_eventTypeCount].text, eventType))
  {
    ++_rejectedEventTypeCount;
    return false;
  }
  _eventTypes[_eventTypeCount].field = WHOLE_STRING;
  _eventTypeCount++;
  // A new type changes the catalog.
  _entityCatalogDirty = true;
  return true;
}

void Blaeck::clearAllEvents()
{
  for (uint16_t i = 0; i < _eventChannelSlots(); i++)
  {
    if (_eventChannels[i].inUse)
      _entityCatalogDirty = true;

    _setChannelName(_eventChannels[i].name, _eventChannels[i].nameInFlash, nullptr, nullptr);
    _eventChannels[i] = EventChannelEntry{};
  }
  for (uint16_t i = 0; i < _eventTypeCount; ++i)
    _eventTypes[i].text = nullptr;
  _eventTypeCount = 0;
}

int Blaeck::_findEventChannel(byte deviceId, const __FlashStringHelper *channelName) const
{
  for (uint16_t i = 0; i < _eventChannelSlots(); i++)
  {
    if (_eventChannels[i].inUse && _eventChannels[i].deviceId == deviceId &&
        _channelNameEqualsFlash(_eventChannels[i].name, _eventChannels[i].nameInFlash, channelName))
      return i;
  }
  return -1;
}

int Blaeck::_findEventChannel(byte deviceId, const char *channelName) const
{
  if (channelName == nullptr || channelName[0] == '\0')
    return -1;

  for (uint16_t i = 0; i < _eventChannelSlots(); i++)
  {
    if (_eventChannels[i].inUse && _eventChannels[i].deviceId == deviceId
        && _channelNameEquals(_eventChannels[i].name, _eventChannels[i].nameInFlash, channelName))
      return (int)i;
  }
  return -1;
}

int Blaeck::_findEventType(uint16_t channelIndex, BlaeckString eventType) const
{
  if (eventType == nullptr)
    return -1;

  // The index is the position among this channel's entries. Compare the text, not the pointer:
  // identical F() literals may sit at different addresses.
  uint16_t index = 0;
  for (uint16_t i = 0; i < _eventTypeCount; i++)
  {
    if (_eventTypes[i].channelIndex != channelIndex)
      continue;

    if (_eventTypeEquals(_eventTypes[i], eventType))
      return (int)index;

    index++;
  }
  return -1;
}

// Compares two flash strings a byte at a time. avr-libc has no flash-to-flash strcmp.
bool Blaeck::_flashStringEquals(const __FlashStringHelper *a, const __FlashStringHelper *b)
{
  if (a == b)
    return true;
  if (a == nullptr || b == nullptr)
    return false;

  PGM_P pa = reinterpret_cast<PGM_P>(a);
  PGM_P pb = reinterpret_cast<PGM_P>(b);
  for (;;)
  {
    byte ca = pgm_read_byte(pa++);
    byte cb = pgm_read_byte(pb++);
    if (ca != cb)
      return false;
    if (ca == 0)
      return true;
  }
}

void Blaeck::_writeEvent(byte deviceId, const char *channelName, BlaeckString eventType)
{
  // Layout: Event (0x85) in the protocol spec. The event's index counts the events of the
  // entity list, which lists them in table order.
  if (!_mayWriteFrame())
    return;

  // Send changed catalogs first. An event sent against an old list can't be corrected later.
  _flushCatalogs();

  int channelIndex = _findEventChannel(deviceId, channelName);
  if (channelIndex < 0)
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Event dropped, not added with addEvent(): "));
      _debugStream->println(channelName != nullptr ? channelName : "");
    }
    return;
  }

  int eventIndex = _findEventType((byte)channelIndex, eventType);
  if (eventIndex < 0)
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Event dropped, type not added with addEvent() or addEventType(): "));
      _debugStream->println(channelName);
    }
    return;
  }

  uint16_t position = 0;
  for (int i = 0; i < channelIndex; i++)
    if (_eventChannels[(uint16_t)i].inUse)
      position++;

  if (!_frameOpen(0x85, 0))
    return;
  _emitByte((byte)(position & 0xFF));
  _emitByte((byte)((position >> 8) & 0xFF));
  _emitByte((byte)(eventIndex & 0xFF));
  _emitByte((byte)((eventIndex >> 8) & 0xFF));
  _frameClose();
}
#else
bool Blaeck::_addEventType(byte, const char *, BlaeckString) { return false; }
void Blaeck::clearAllEvents() {}
// Used by addEvent(), which exists either way.
int Blaeck::_registerEventChannel(byte, const char *, const __FlashStringHelper *, BlaeckString) { return -1; }
void Blaeck::_writeEvent(byte, const char *, BlaeckString) {}
#endif

void Blaeck::_percentDecodeInPlace(char *s)
{
  if (s == nullptr)
    return;

  const char *src = s;
  char *dst = s;
  while (*src != '\0')
  {
    if (*src == '%' && src[1] != '\0' && src[2] != '\0')
    {
      char hi = src[1];
      char lo = src[2];
      int hiVal = (hi >= '0' && hi <= '9') ? hi - '0'
                  : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                  : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10
                                             : -1;
      int loVal = (lo >= '0' && lo <= '9') ? lo - '0'
                  : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                  : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10
                                             : -1;
      if (hiVal >= 0 && loVal >= 0)
      {
        *dst++ = (char)((hiVal << 4) | loVal);
        src += 3;
        continue;
      }
    }
    *dst++ = *src++;
  }
  *dst = '\0';
}

void Blaeck::_setTimedDataState(bool timedActivated, unsigned long timedInterval_ms)
{
  _timedActivated = timedActivated;

  if (_timedActivated)
  {
    _timedInterval_ms = timedInterval_ms;
    _timedFirstTime = true;
  }
}

void BlaeckDeviceBase::write(int signalIndex, bool value, unsigned long long timestamp)
{
  if (_core != nullptr && _core->_storeSigned(signalIndex, value))
    _core->_writeSignalNow(signalIndex, timestamp);
}
void BlaeckDeviceBase::write(int signalIndex, byte value, unsigned long long timestamp)
{
  if (_core != nullptr && _core->_storeUnsigned(signalIndex, value))
    _core->_writeSignalNow(signalIndex, timestamp);
}
void BlaeckDeviceBase::write(int signalIndex, short value, unsigned long long timestamp)
{
  if (_core != nullptr && _core->_storeSigned(signalIndex, value))
    _core->_writeSignalNow(signalIndex, timestamp);
}
void BlaeckDeviceBase::write(int signalIndex, unsigned short value, unsigned long long timestamp)
{
  if (_core != nullptr && _core->_storeUnsigned(signalIndex, value))
    _core->_writeSignalNow(signalIndex, timestamp);
}
void BlaeckDeviceBase::write(int signalIndex, int value, unsigned long long timestamp)
{
  if (_core != nullptr && _core->_storeSigned(signalIndex, value))
    _core->_writeSignalNow(signalIndex, timestamp);
}
void BlaeckDeviceBase::write(int signalIndex, unsigned int value, unsigned long long timestamp)
{
  if (_core != nullptr && _core->_storeUnsigned(signalIndex, value))
    _core->_writeSignalNow(signalIndex, timestamp);
}
void BlaeckDeviceBase::write(int signalIndex, long value, unsigned long long timestamp)
{
  if (_core != nullptr && _core->_storeSigned(signalIndex, value))
    _core->_writeSignalNow(signalIndex, timestamp);
}
void BlaeckDeviceBase::write(int signalIndex, unsigned long value, unsigned long long timestamp)
{
  if (_core != nullptr && _core->_storeUnsigned(signalIndex, value))
    _core->_writeSignalNow(signalIndex, timestamp);
}
void BlaeckDeviceBase::write(int signalIndex, long long value, unsigned long long timestamp)
{
  if (_core != nullptr && _core->_storeLongLong(signalIndex, value))
    _core->_writeSignalNow(signalIndex, timestamp);
}
void BlaeckDeviceBase::write(int signalIndex, float value, unsigned long long timestamp)
{
  if (_core != nullptr && _core->_storeFloating(signalIndex, value))
    _core->_writeSignalNow(signalIndex, timestamp);
}
void BlaeckDeviceBase::write(int signalIndex, double value, unsigned long long timestamp)
{
  if (_core != nullptr && _core->_storeFloating(signalIndex, value))
    _core->_writeSignalNow(signalIndex, timestamp);
}

void BlaeckDeviceBase::write(int signalIndex, const char *value, unsigned long long timestamp)
{
  if (_core != nullptr)
    _core->_writeSignalText(signalIndex, value, false, timestamp);
}

void BlaeckDeviceBase::write(int signalIndex, const __FlashStringHelper *value, unsigned long long timestamp)
{
  if (_core != nullptr)
    _core->_writeSignalText(signalIndex, value, true, timestamp);
}

void Blaeck::_writeSignalText(int signalIndex, const void *value, bool inFlash, unsigned long long timestamp)
{
  if (signalIndex >= 0 && signalIndex < _signalIndex)
  {
    if (Signals[signalIndex].DataType == Blaeck_string)
    {
      Signals[signalIndex].Address = const_cast<void *>(value);
      Signals[signalIndex].TextInFlash = inFlash;
      _writeSignalNow(signalIndex, timestamp);
    }
  }
}

void Blaeck::writeAll(unsigned long long timestamp)
{
  this->writeAll(0, timestamp == BLAECK_NOW ? getTimeStamp() : timestamp);
}

void Blaeck::writeAll(unsigned long msg_id, unsigned long long timestamp)
{
  this->writeData(msg_id, 0, _signalIndex - 1, false, timestamp);
}

void Blaeck::writeData(unsigned long msg_id, int signalIndex_start, int signalIndex_end, bool selectedOnly, unsigned long long timestamp)
{
  if (_signalIndex == 0)
    return;

  if (_beforeWriteCallback != NULL)
    _beforeWriteCallback();
  this->writeDataFrame(msg_id, signalIndex_start, signalIndex_end, selectedOnly, timestamp);
}

void Blaeck::tick(unsigned long long timestamp)
{
  read();
  writeIfDue(timestamp);
}

void Blaeck::writeIfDue(unsigned long long timestamp)
{
  // Read on every call, which also keeps rollover tracking alive when nothing is due.
  if (timestamp == BLAECK_NOW)
    timestamp = getTimeStamp();
  const uint32_t now = static_cast<uint32_t>(millis());
  const uint32_t elapsed = now - _lastIntervalMs;
  const bool initialInterval = _timedActivated && _timedFirstTime;
  const bool intervalDue = _timedActivated &&
      (_timedFirstTime || _timedInterval_ms == 0 || elapsed >= _timedInterval_ms);
  if (intervalDue)
  {
    if (_timedFirstTime || _timedInterval_ms == 0)
      _lastIntervalMs = now;
    else
      _lastIntervalMs += elapsed - (elapsed % _timedInterval_ms);
    if (_signalIndex != 0 && _beforeWriteCallback != nullptr)
      _beforeWriteCallback();
  }
  if (!_mayWriteFrame())
    return;
  if (intervalDue)
    _timedFirstTime = false;

  const uint32_t changeNow = static_cast<uint32_t>(millis());
  bool intervalReport = false;
  for (int i = 0; i < _signalIndex; ++i)
  {
    Signal &s = Signals[i];
    ReportingState *r = s.Reporting;
    const bool interval = intervalDue && (s.IntervalMode == BLAECK_ALWAYS ||
        (s.IntervalMode == BLAECK_ON_CHANGE &&
         (initialInterval || _signalChanged(s, r->intervalDelta))));
    const bool immediate = r != nullptr && r->immediate &&
        (!r->valid || static_cast<uint32_t>(changeNow - r->lastWriteMs) >= r->minIntervalMs) &&
        _signalChanged(s, r->changeDelta);
    s.Selected = interval || immediate;
    intervalReport |= interval;
  }
  writeDataFrame(0, 0, _signalIndex - 1, true, timestamp, intervalReport);
  _writeChangedProperties();
}

// ----- Buffered writes -----

void Blaeck::_bufAllocate()
{
  _bufFree();
  // Sized for the signals added so far; _bufEnsure() grows it if a frame needs more.
  int signalsHeld = _signalIndex > 0 ? _signalIndex : 1;
  _frameBufSize = 60 + signalsHeld * 10;
  // The device list can be larger with long signal names.
  int b0b3_est = 60 + signalsHeld * 30;
  if (b0b3_est > _frameBufSize)
    _frameBufSize = b0b3_est;
  _frameBuf = new (std::nothrow) byte[_frameBufSize];
  if (_frameBuf == nullptr)
  {
    _frameBufSize = 0;
  }
  _bufOverflow = false;
  _bufOverflowWarned = false;
}

bool Blaeck::_bufEnsure(size_t addLen)
{
  if (_frameBuf == nullptr)
  {
    return false;
  }

  if (addLen > (SIZE_MAX - (size_t)_framePos))
  {
    return false;
  }

  size_t needed = (size_t)_framePos + addLen;
  if (needed <= (size_t)_frameBufSize)
  {
    return true;
  }

  size_t newSize = (size_t)_frameBufSize;
  while (newSize < needed)
  {
    if (newSize < 128)
    {
      newSize = 128;
    }
    else
    {
      if (newSize > (SIZE_MAX / 2))
      {
        return false;
      }
      newSize *= 2;
    }
  }

  if (newSize > (size_t)INT_MAX)
  {
    return false;
  }

  byte *newBuf = new (std::nothrow) byte[newSize];
  if (newBuf == nullptr)
  {
    return false;
  }

  memcpy(newBuf, _frameBuf, _framePos);
  delete[] _frameBuf;
  _frameBuf = newBuf;
  _frameBufSize = (int)newSize;
  return true;
}

void Blaeck::_bufFree()
{
  delete[] _frameBuf;
  _frameBuf = nullptr;
  _frameBufSize = 0;
  _framePos = 0;
}

void Blaeck::setBufferedWrites(bool enabled)
{
  _bufferedWritesExplicit = true;
  _bufferedWrites = enabled;
  // Turning it on allocates nothing; the buffer is built by the first buffered frame.
  if (!enabled)
    _bufFree();
}

void Blaeck::_setBufferedWritesDefault(bool enabled)
{
  if (_bufferedWritesExplicit)
    return;
  setBufferedWrites(enabled);
  _bufferedWritesExplicit = false;
}

bool Blaeck::_frameOpen(byte msgKey, unsigned long msgId)
{
  if (!_mayWriteFrame())
    return false;

  _frameWriteFailed = false;

  _frameDirect = !_bufReady();
  if (!_frameDirect)
    _bufReset();
  _frameCrcOn = false;

  // Layout: the envelope in the protocol spec's introduction.
  _putBytes((const byte *)"<blaeck:", 8);
  _frameEscaped = true;
  // Every frame ends with a CRC32 over the key through the last field, not the start marker.
  _crc.restart();
  _frameCrcOn = true;
  _emitByte(msgKey);
  _emitByte(':');
  ulngCvt.val = msgId;
  _emitBytes(ulngCvt.bval, 4);
  _emitByte(':');
  return true;
}

bool Blaeck::_frameClose()
{
  const uint32_t crc = _frameCrcEnd();
  _emitBytes((const byte *)&crc, 4);
  _frameEscaped = false;
  _putBytes((const byte *)"/>\n", 3);
  bool complete;
  if (!_frameDirect)
    complete = _bufSend();
  else
  {
    _flushDirect();
    complete = !_frameWriteFailed;
  }
  if (_frameWriteFailed && !_shortWriteReported && _debugStream != nullptr)
    _debugStream->println(F("Incomplete transport write; changed signals remain due."));
  _shortWriteReported = _frameWriteFailed;
  return complete;
}

void Blaeck::_emitDeviceSignals(byte deviceId)
{
  uint16_t count = 0;
  for (int i = 0; i < _signalIndex; ++i)
    if (Signals[i].DeviceId == deviceId)
      ++count;
  _emitByte((byte)(count & 0xFF));
  _emitByte((byte)((count >> 8) & 0xFF));
  for (int i = 0; i < _signalIndex; ++i)
  {
    const Signal &signal = Signals[i];
    if (signal.DeviceId != deviceId)
      continue;
    _emitSignalName0(signal);
    _emitByte(_dtypeCode(signal.DataType));
  }
}

void Blaeck::_emitDeviceRecord(byte deviceId, byte state, BlaeckString name, BlaeckString hw, BlaeckString fw)
{
  _emitByte(deviceId);
  // Parent: the board, for the board itself and for every device (one level only).
  _emitByte(0);
  // DeviceFlags: no optional fields yet.
  _emitByte(0);
  _emitByte(0);
  _emitByte(state);
  _emitFlashStr0(name);
  _emitFlashStr0(hw);
  _emitFlashStr0(fw);
}

// ----- Frame writers -----

void Blaeck::writeRestarted()
{
  this->writeRestarted(0);
}

void Blaeck::writeRestarted(unsigned long msg_id)
{
  // Checked before the flag is set, so a read() before begin() doesn't use up the notice.
  if (!_mayWriteFrame())
    return;

  if (!_writeRestartedAlreadyDone)
  {
    _writeRestartedAlreadyDone = true;

    if (!_frameOpen(0xC1, msg_id))
      return;
    _emitByte(0);
    _emitByte(DEVICE_EVENT_RESTARTED);
    _frameClose();

    // Send the entity list after the notice, so a host that stayed connected sees what this run
    // declares and that its values are back at their defaults. This runs from read(), so
    // setup() has finished declaring by then.
    this->writeEntities(msg_id);
  }
}

void Blaeck::writeDevices()
{
  this->writeDevices(0);
}

void Blaeck::writeDevices(unsigned long msg_id)
{
  this->writeDevicesFrame(msg_id);
}

void Blaeck::writeDevicesFrame(unsigned long msg_id)
{
  // Layout: Device List (0xB7) in the protocol spec.
  if (!_frameOpen(0xB7, msg_id))
    return;
  _emitStr0(_libraryName());
  _emitStr0(_libraryVersion());
  // The longest command the device can receive, so a host knows how much room is left for
  // parameters.
  const uint16_t payloadMax = (uint16_t)(MAXIMUM_CHAR_COUNT - 1);
  _emitByte((byte)(payloadMax & 0xFF));
  _emitByte((byte)((payloadMax >> 8) & 0xFF));
  _emitByte((byte)(_deviceCount + 1));
  _emitDeviceRecord(0, _writeRestartedAlreadyDone ? 0 : DEVICE_STATE_RESTARTED, _deviceName(),
                    _boardHW != nullptr ? BlaeckString(_boardHW) : BlaeckString(_defaultBoardName()),
                    _orNotAvailable(_boardFW));
  _emitDeviceSignals(0);
  for (byte i = 0; i < _deviceCount; ++i)
  {
    const DeviceEntry &d = _devices[i];
    const byte state = (byte)((d.missing ? DEVICE_STATE_NOT_RESPONDING : 0) |
                              (d.restartPending ? DEVICE_STATE_RESTARTED : 0));
    _emitDeviceRecord(i + 1, state, d.name, _orNotAvailable(d.hwVersion), _orNotAvailable(d.fwVersion));
    _emitDeviceSignals(i + 1);
  }
  if (!_frameClose())
    return;
  // The list told the host every state, so no notice for it is still due. The board's
  // restart counts as reported too; a host that asked for the list has no catalogs to drop.
  _writeRestartedAlreadyDone = true;
  for (byte i = 0; i < _deviceCount; ++i)
  {
    _devices[i].restartPending = false;
    _devices[i].reportedMissing = _devices[i].missing;
  }
}

void Blaeck::writeDataFrame(unsigned long msg_id, int signalIndex_start, int signalIndex_end, bool selectedOnly, unsigned long long timestamp, bool intervalReport)
{
  if (!_mayWriteFrame())
    return;

  // Clamp the range.
  if (signalIndex_start < 0)
    signalIndex_start = 0;
  if (signalIndex_end >= _signalIndex)
    signalIndex_end = _signalIndex - 1;
  if (signalIndex_start > signalIndex_end)
    return; // No valid range

  bool any = false;
  for (int i = signalIndex_start; i <= signalIndex_end; ++i)
  {
    Signal &s = Signals[i];
    if (!selectedOnly)
      s.Selected = true;
    // A missing device's signals are left out; the host learned of it from C1 or B7.
    if (s.Selected && _deviceMissing(s.DeviceId))
    {
      s.Selected = false;
      continue;
    }
    if (s.Selected)
    {
      if (!_prepareSignalSnapshot(s))
        return;
      any = true;
    }
  }
  if (!any)
    return;

  // Layout: Data (0xD3) in the protocol spec.
  if (!_frameOpen(0xD3, msg_id))
    return;

  bool restartFlagSnapshot = _sendRestartFlag;
  _emitByte(_frameFlags(restartFlagSnapshot, intervalReport));
  _emitByte((byte)(_schemaHash & 0xFF));
  _emitByte((byte)((_schemaHash >> 8) & 0xFF));

  _emitByte((byte)_timestampMode);
  if (_timestampMode != BLAECK_NO_TIMESTAMP)
  {
    // Always sent when a timestamp mode is set, even without a clock (it is then 0). Leaving
    // it out would shift every later byte.
    ullCvt.val = timestamp;
    _emitBytes(ullCvt.bval, 8);
  }

  // In device list order, the order a host numbers the signals by.
  for (int dev = 0; dev <= _deviceCount; ++dev)
  for (int i = signalIndex_start; i <= signalIndex_end; i++)
  {
    if (!Signals[i].Selected || Signals[i].DeviceId != dev)
      continue;

    const uint16_t wireIndex = Signals[i].WireIndex;
    _emitByte((byte)(wireIndex & 0xFF));
    _emitByte((byte)((wireIndex >> 8) & 0xFF));

    Signal signal = Signals[i];
    if (signal.Reporting != nullptr)
    {
      _captureSignalSnapshot(Signals[i]);
      if (signal.DataType == Blaeck_string)
      {
        _emitByte(signal.Reporting->textLength);
        _emitBytes(reinterpret_cast<const byte *>(signal.Reporting->text), signal.Reporting->textLength);
      }
      else
      {
        _emitBytes(signal.Reporting->value, _signalValueSize(signal.DataType));
      }
      continue;
    }
    switch (signal.DataType)
    {
    case (Blaeck_bool):   boolCvt.val  = *((bool *)signal.Address);           _emitBytes(boolCvt.bval, 1);  break;
    case (Blaeck_byte):   _emitByte(*((byte *)signal.Address));                                              break;
    // Read at the width the protocol names, as the snapshot branch above already does: a
    // board's int is registered as Blaeck_long, four bytes, and reading it through a long*
    // on a 64-bit host took eight. Every target is little-endian, so the bytes are the value.
    case (Blaeck_short): case (Blaeck_ushort):
    case (Blaeck_int): case (Blaeck_uint):
    case (Blaeck_long): case (Blaeck_ulong):
    case (Blaeck_float): case (Blaeck_double):
    case (Blaeck_longlong):
    {
      byte bytes[8];
      const size_t size = _signalValueSize(signal.DataType);
      memcpy(bytes, signal.Address, size);
      _emitBytes(bytes, size);
      break;
    }
    case (Blaeck_string):
    {
      byte len = static_cast<byte>(_textLength(signal.Address, signal.TextInFlash));
      _emitByte(len);
      if (len > 0)
        _emitTextBytes(signal.Address, signal.TextInFlash, len);
    }
    break;
    }

  }

  const bool complete = _frameClose();
  const uint32_t sentAt = static_cast<uint32_t>(millis());
  for (int i = signalIndex_start; i <= signalIndex_end; ++i)
  {
    Signal &s = Signals[i];
    if (s.Selected && s.Reporting != nullptr)
    {
      // Encoding overwrites the shared snapshot. Invalidate failed frames so
      // their values cannot suppress the next report.
      s.Reporting->valid = complete;
      if (complete)
        s.Reporting->lastWriteMs = sentAt;
    }
  }
  if (complete)
    _sendRestartFlag = false;
}

void Blaeck::setTimestampMode(BlaeckTimestampMode mode, unsigned long long (*clock)())
{
  _timestampMode = mode;

  // Restart micros() rollover tracking.
  _prevMicros = 0;
  _overflowCount = 0;

  switch (mode)
  {
  case BLAECK_MICROS:
    _timestampCallback = clock != nullptr ? clock : _microsWrapper;
    break;
  case BLAECK_UNIX:
    _timestampCallback = clock;
    if (clock == nullptr && _debugStream != nullptr)
      _debugStream->println(F("setTimestampMode(): BLAECK_UNIX needs a clock; timestamps are 0."));
    break;
  case BLAECK_NO_TIMESTAMP:
  default:
    _timestampCallback = nullptr;
    break;
  }
}

bool Blaeck::hasTimestampClock() const
{
  return (_timestampMode != BLAECK_NO_TIMESTAMP && _timestampCallback != nullptr);
}

unsigned long long Blaeck::getTimeStamp()
{
  unsigned long long timestamp = 0;

  if (_timestampMode != BLAECK_NO_TIMESTAMP && hasTimestampClock())
  {
    if (_timestampMode == BLAECK_MICROS)
    {
      // Extend micros() past its rollover, which comes about every 71 minutes.
      unsigned long raw = (unsigned long)_timestampCallback();
      if (raw < _prevMicros)
      {
        _overflowCount++;
      }
      _prevMicros = raw;
      timestamp = (_overflowCount * 4294967296ULL) + raw;
    }
    else if (_timestampMode == BLAECK_UNIX)
    {
      // The callback returns microseconds since the Unix epoch.
      timestamp = _timestampCallback();
    }
  }

  return timestamp;
}

void Blaeck::validatePlatformSizes()
{
#ifdef __AVR__
  // AVR (8-bit)
  static_assert(sizeof(int) == 2, "Blaeck: Expected 2-byte int on AVR");
  static_assert(sizeof(unsigned int) == 2, "Blaeck: Expected 2-byte unsigned int on AVR");
  static_assert(sizeof(double) == 4, "Blaeck: Expected 4-byte double on AVR");
  static_assert(sizeof(double) == sizeof(float), "Blaeck: double should equal float on AVR");
#else
  // 32-bit boards
  static_assert(sizeof(int) == 4, "Blaeck: Expected 4-byte int on 32-bit platforms");
  static_assert(sizeof(unsigned int) == 4, "Blaeck: Expected 4-byte unsigned int on 32-bit platforms");
  static_assert(sizeof(double) == 8, "Blaeck: Expected 8-byte double on 32-bit platforms");
  static_assert(sizeof(double) != sizeof(float), "Blaeck: double should differ from float on 32-bit platforms");
#ifndef BLAECK_NATIVE_TEST
  static_assert(sizeof(int) == sizeof(long), "Blaeck: int/long size mismatch breaks type remapping");
  static_assert(sizeof(unsigned int) == sizeof(unsigned long), "Blaeck: uint/ulong size mismatch breaks type remapping");
#endif
#endif

  // The same on every board
  static_assert(sizeof(bool) == 1, "Blaeck: Expected 1-byte bool");
  static_assert(sizeof(byte) == 1, "Blaeck: Expected 1-byte byte");
  static_assert(sizeof(short) == 2, "Blaeck: Expected 2-byte short");
  static_assert(sizeof(unsigned short) == 2, "Blaeck: Expected 2-byte unsigned short");
  // A 64-bit host running the native test suite has an 8-byte long; no Arduino board does.
#ifndef BLAECK_NATIVE_TEST
  static_assert(sizeof(long) == 4, "Blaeck: Expected 4-byte long");
  static_assert(sizeof(unsigned long) == 4, "Blaeck: Expected 4-byte unsigned long");
#endif
  static_assert(sizeof(float) == 4, "Blaeck: Expected 4-byte float");
}

// ----- F() overloads -----
// State writes look up flash names directly. Registrations retain flash pointers.
// These sit outside the BLAECK_ENABLE_*
// blocks, so they call the real function or its stub, whichever was compiled.

// ----- BlaeckDeviceBase: calls into the board -----

int BlaeckDeviceBase::_registerCommand(const char *command, BlaeckCommandHandler handler,
                                       BlaeckButtonFunction press)
{
  return _core != nullptr ? _core->_registerCommand(_deviceId, command, handler, press) : -1;
}

// ----- BlaeckDeviceBase: calls taking a name -----
// A name is RAM or F() text alike; Blaeck's calls below take the two apart.

int BlaeckDeviceBase::_registerSignal(BlaeckString signalName, dataType type, void *address, bool textInFlash)
{
  if (_core == nullptr)
    return -1;
  return signalName.inFlash()
             ? _core->_registerSignal(_deviceId, reinterpret_cast<const __FlashStringHelper *>(signalName.data()),
                                      type, address, textInFlash)
             : _core->_registerSignal(_deviceId, signalName.data(), type, address, textInFlash);
}

int BlaeckDeviceBase::_registerEventChannel(BlaeckString channelName, BlaeckString eventTypes)
{
  if (_core == nullptr)
    return -1;
  return _core->_registerEventChannel(_deviceId, channelName.inFlash() ? nullptr : channelName.data(),
                                      channelName.inFlash() ? reinterpret_cast<const __FlashStringHelper *>(channelName.data()) : nullptr,
                                      eventTypes);
}

// int and unsigned int are registered by their real width: 2 bytes on AVR, 4 elsewhere. On AVR
// a double is a float, with no gain in precision, so it is registered as one.
#ifdef __AVR__
static const dataType BLAECK_INT_TYPE = Blaeck_int;
static const dataType BLAECK_UINT_TYPE = Blaeck_uint;
static const dataType BLAECK_DOUBLE_TYPE = Blaeck_float;
#else
static const dataType BLAECK_INT_TYPE = Blaeck_long;
static const dataType BLAECK_UINT_TYPE = Blaeck_ulong;
static const dataType BLAECK_DOUBLE_TYPE = Blaeck_double;
#endif

BlaeckSignalRef BlaeckDeviceBase::addSignal(BlaeckString signalName, bool *value) { return BlaeckSignalRef(_core, (int16_t)_registerSignal(signalName, Blaeck_bool, value)); }
BlaeckSignalRef BlaeckDeviceBase::addSignal(BlaeckString signalName, byte *value) { return BlaeckSignalRef(_core, (int16_t)_registerSignal(signalName, Blaeck_byte, value)); }
BlaeckSignalRef BlaeckDeviceBase::addSignal(BlaeckString signalName, short *value) { return BlaeckSignalRef(_core, (int16_t)_registerSignal(signalName, Blaeck_short, value)); }
BlaeckSignalRef BlaeckDeviceBase::addSignal(BlaeckString signalName, unsigned short *value) { return BlaeckSignalRef(_core, (int16_t)_registerSignal(signalName, Blaeck_ushort, value)); }
BlaeckSignalRef BlaeckDeviceBase::addSignal(BlaeckString signalName, int *value) { return BlaeckSignalRef(_core, (int16_t)_registerSignal(signalName, BLAECK_INT_TYPE, value)); }
BlaeckSignalRef BlaeckDeviceBase::addSignal(BlaeckString signalName, unsigned int *value) { return BlaeckSignalRef(_core, (int16_t)_registerSignal(signalName, BLAECK_UINT_TYPE, value)); }
BlaeckSignalRef BlaeckDeviceBase::addSignal(BlaeckString signalName, long *value) { return BlaeckSignalRef(_core, (int16_t)_registerSignal(signalName, Blaeck_long, value)); }
BlaeckSignalRef BlaeckDeviceBase::addSignal(BlaeckString signalName, unsigned long *value) { return BlaeckSignalRef(_core, (int16_t)_registerSignal(signalName, Blaeck_ulong, value)); }
BlaeckSignalRef BlaeckDeviceBase::addSignal(BlaeckString signalName, long long *value) { return BlaeckSignalRef(_core, (int16_t)_registerSignal(signalName, Blaeck_longlong, value)); }
BlaeckSignalRef BlaeckDeviceBase::addSignal(BlaeckString signalName, float *value) { return BlaeckSignalRef(_core, (int16_t)_registerSignal(signalName, Blaeck_float, value)); }
BlaeckSignalRef BlaeckDeviceBase::addSignal(BlaeckString signalName, double *value) { return BlaeckSignalRef(_core, (int16_t)_registerSignal(signalName, BLAECK_DOUBLE_TYPE, value)); }
// Address is void * for every type; a string is only read.
BlaeckSignalRef BlaeckDeviceBase::addSignal(BlaeckString signalName, const char *value) { return BlaeckSignalRef(_core, (int16_t)_registerSignal(signalName, Blaeck_string, const_cast<char *>(value))); }
BlaeckSignalRef BlaeckDeviceBase::addSignal(BlaeckString signalName, const __FlashStringHelper *value) { return BlaeckSignalRef(_core, (int16_t)_registerSignal(signalName, Blaeck_string, const_cast<__FlashStringHelper *>(value), true)); }

BlaeckEventRef BlaeckDeviceBase::addEvent(BlaeckString channelName, BlaeckString eventTypes)
{
  return BlaeckEventRef(_core, (int16_t)_registerEventChannel(channelName, eventTypes));
}

// Blaeck looks events up by RAM name, so an F() name is copied for the lookup.
bool BlaeckDeviceBase::addEventType(BlaeckString channelName, BlaeckString eventType)
{
  if (_core == nullptr)
    return false;
  if (!channelName.inFlash())
    return _core->_addEventType(_deviceId, channelName.data(), eventType);
  char name[Blaeck::MAX_EVENT_NAME_COUNT];
  Blaeck::copyFlashName(reinterpret_cast<const __FlashStringHelper *>(channelName.data()), name, sizeof(name));
  return _core->_addEventType(_deviceId, name, eventType);
}

void BlaeckDeviceBase::writeEvent(BlaeckString channelName, BlaeckString eventType)
{
  if (_core == nullptr)
    return;
  if (!channelName.inFlash())
  {
    _core->_writeEvent(_deviceId, channelName.data(), eventType);
    return;
  }
  char name[Blaeck::MAX_EVENT_NAME_COUNT];
  Blaeck::copyFlashName(reinterpret_cast<const __FlashStringHelper *>(channelName.data()), name, sizeof(name));
  _core->_writeEvent(_deviceId, name, eventType);
}

int BlaeckDeviceBase::findSignalIndex(BlaeckString signalName)
{
  if (_core == nullptr)
    return -1;
  return signalName.inFlash()
             ? _core->_findSignalIndex(_deviceId, reinterpret_cast<const __FlashStringHelper *>(signalName.data()))
             : _core->_findSignalIndex(_deviceId, signalName.data());
}

// By name: looked up, then written by index, which ignores an index of -1. BLAECK_NOW
// becomes the current time only once the value is sent.
void BlaeckDeviceBase::write(BlaeckString signalName, bool value, unsigned long long timestamp) { write(findSignalIndex(signalName), value, timestamp); }
void BlaeckDeviceBase::write(BlaeckString signalName, byte value, unsigned long long timestamp) { write(findSignalIndex(signalName), value, timestamp); }
void BlaeckDeviceBase::write(BlaeckString signalName, short value, unsigned long long timestamp) { write(findSignalIndex(signalName), value, timestamp); }
void BlaeckDeviceBase::write(BlaeckString signalName, unsigned short value, unsigned long long timestamp) { write(findSignalIndex(signalName), value, timestamp); }
void BlaeckDeviceBase::write(BlaeckString signalName, int value, unsigned long long timestamp) { write(findSignalIndex(signalName), value, timestamp); }
void BlaeckDeviceBase::write(BlaeckString signalName, unsigned int value, unsigned long long timestamp) { write(findSignalIndex(signalName), value, timestamp); }
void BlaeckDeviceBase::write(BlaeckString signalName, long value, unsigned long long timestamp) { write(findSignalIndex(signalName), value, timestamp); }
void BlaeckDeviceBase::write(BlaeckString signalName, unsigned long value, unsigned long long timestamp) { write(findSignalIndex(signalName), value, timestamp); }
void BlaeckDeviceBase::write(BlaeckString signalName, long long value, unsigned long long timestamp) { write(findSignalIndex(signalName), value, timestamp); }
void BlaeckDeviceBase::write(BlaeckString signalName, float value, unsigned long long timestamp) { write(findSignalIndex(signalName), value, timestamp); }
void BlaeckDeviceBase::write(BlaeckString signalName, double value, unsigned long long timestamp) { write(findSignalIndex(signalName), value, timestamp); }
void BlaeckDeviceBase::write(BlaeckString signalName, const char *value, unsigned long long timestamp) { write(findSignalIndex(signalName), value, timestamp); }
void BlaeckDeviceBase::write(BlaeckString signalName, const __FlashStringHelper *value, unsigned long long timestamp) { write(findSignalIndex(signalName), value, timestamp); }

// ----- Properties -----

blaeck_detail::PropertyEntry *BlaeckPropertyRefBase::_entry() const
{
  if (_owner == nullptr || _index < 0 || static_cast<uint16_t>(_index) >= _owner->_propertyCount)
    return nullptr;
  return &_owner->_properties[_index];
}

blaeck_detail::PropertyPresentation *BlaeckPropertyRefBase::_presentation() const
{
  blaeck_detail::PropertyEntry *e = _entry();
  if (e == nullptr)
    return nullptr;
  if (e->presentation == nullptr)
  {
    e->presentation = new (std::nothrow) blaeck_detail::PropertyPresentation();
    if (e->presentation == nullptr)
    {
      if (_owner->_rejectedStringCount != UINT16_MAX)
        ++_owner->_rejectedStringCount;
      if (_owner->_debugStream != nullptr)
        _owner->_debugStream->println(F("No RAM for a property's presentation; it is sent without."));
    }
  }
  return e->presentation;
}

void BlaeckPropertyRefBase::_setFlags(uint32_t mask, uint32_t value) const
{
  blaeck_detail::PropertyEntry *e = _entry();
  if (e == nullptr)
    return;
  const uint32_t flags = (e->flags & ~mask) | (value & mask);
  // Only a real change marks the list: a modifier may be called on every loop() pass.
  if (flags != e->flags)
  {
    e->flags = flags;
    _owner->_entityCatalogDirty = true;
  }
}

void BlaeckPropertyRefBase::_setText(detail::StoredString blaeck_detail::PropertyPresentation::*field,
                                     BlaeckString text) const
{
  // An empty text is none; a host may refuse a blank one.
  if (blaeck_detail::flashStrEmpty(text))
    text = nullptr;
  blaeck_detail::PropertyEntry *e = _entry();
  if (e == nullptr || (text == nullptr && e->presentation == nullptr))
    return;
  blaeck_detail::PropertyPresentation *pr = _presentation();
  if (pr == nullptr || !((pr->*field) != text))
    return;
  if (_owner->_storeString(pr->*field, text))
    _owner->_entityCatalogDirty = true;
}

// Reports on debug what a range check refused. A bound the variable cannot state would accept a
// write it cannot store, and the catalog sends a range at the variable's own width, where such a
// bound does not survive either: a fractional bound on a whole-number variable arrives truncated.
static void _reportRangeChecks(Print *debug, bool maxAboveMin, bool boundsFit,
                               bool stepGiven, bool stepPositive, bool stepFits)
{
  if (debug == nullptr)
    return;
  if (!maxAboveMin)
    debug->println(F("withRange(): max is not above min, so no range is set."));
  if (stepGiven && !stepPositive)
    debug->println(F("withRange(): a step must be above 0, so no step is set."));
  if (maxAboveMin && !boundsFit)
    debug->println(F("withRange(): a bound is not a value the variable holds, so no range is set."));
  if (stepPositive && !stepFits)
    debug->println(F("withRange(): the step is not a value the variable holds, so no step is set."));
}

// Puts a checked range on the entry. True if it differs from the one already there, compared
// through the member the variable's own type makes the live one.
static bool _storeRange(blaeck_detail::PropertyEntry &e, const blaeck_detail::RangeBound &mn,
                        const blaeck_detail::RangeBound &mx, const blaeck_detail::RangeBound &st)
{
  const bool same = _isIntegerType(e.type)
                        ? (e.rangeMin.asInteger == mn.asInteger &&
                           e.rangeMax.asInteger == mx.asInteger &&
                           e.rangeStep.asInteger == st.asInteger)
                        : (e.rangeMin.asDouble == mn.asDouble &&
                           e.rangeMax.asDouble == mx.asDouble &&
                           e.rangeStep.asDouble == st.asDouble);
  if (same)
    return false;
  e.rangeMin = mn;
  e.rangeMax = mx;
  e.rangeStep = st;
  return true;
}

void BlaeckPropertyRefBase::_setRange(double mn, double mx, double st) const
{
  blaeck_detail::PropertyEntry *e = _entry();
  if (e == nullptr)
    return;
  const bool fits = _representable(e->type, mn) && _representable(e->type, mx);
  const bool stepFits = _representable(e->type, st);
  _reportRangeChecks(_owner->_debugStream, mx > mn, fits, st != 0.0, st > 0.0, stepFits);
  const bool hasRange = mx > mn && fits;
  const bool hasStep = st > 0.0 && stepFits;
  if (_storeRange(*e, _makeBound(e->type, mn), _makeBound(e->type, mx), _makeBound(e->type, st)))
    _owner->_entityCatalogDirty = true;
  _setFlags(blaeck_detail::PROPERTY_HAS_RANGE | blaeck_detail::PROPERTY_HAS_STEP,
            (hasRange ? blaeck_detail::PROPERTY_HAS_RANGE : 0) |
                (hasStep ? blaeck_detail::PROPERTY_HAS_STEP : 0));
}

// The same range, written as whole numbers and kept as whole numbers. A whole number has no
// fraction to lose, so reaching the variable is all there is to check - and it is checked as a
// whole number, which a board whose double holds 24 bits could not do for a wide bound.
void BlaeckPropertyRefBase::_setRange(long long mn, long long mx, long long st) const
{
  blaeck_detail::PropertyEntry *e = _entry();
  if (e == nullptr)
    return;
  const bool fits = _fitsTypeWhole(e->type, mn) && _fitsTypeWhole(e->type, mx);
  const bool stepFits = _fitsTypeWhole(e->type, st);
  _reportRangeChecks(_owner->_debugStream, mx > mn, fits, st != 0, st > 0, stepFits);
  const bool hasRange = mx > mn && fits;
  const bool hasStep = st > 0 && stepFits;
  if (_storeRange(*e, _makeBound(e->type, mn), _makeBound(e->type, mx), _makeBound(e->type, st)))
    _owner->_entityCatalogDirty = true;
  _setFlags(blaeck_detail::PROPERTY_HAS_RANGE | blaeck_detail::PROPERTY_HAS_STEP,
            (hasRange ? blaeck_detail::PROPERTY_HAS_RANGE : 0) |
                (hasStep ? blaeck_detail::PROPERTY_HAS_STEP : 0));
}

void BlaeckPropertyRefBase::_setDisplayPrecision(uint8_t decimals) const
{
  blaeck_detail::PropertyPresentation *pr = _presentation();
  if (pr == nullptr)
    return;
  if (pr->displayPrecision != decimals)
  {
    pr->displayPrecision = decimals;
    _owner->_entityCatalogDirty = true;
  }
  _setFlags(blaeck_detail::PROPERTY_HAS_DISPLAY_PRECISION, blaeck_detail::PROPERTY_HAS_DISPLAY_PRECISION);
}

void BlaeckPropertyRefBase::_setReporting(double delta, uint32_t minIntervalMs) const
{
  blaeck_detail::PropertyEntry *e = _entry();
  if (e == nullptr || e->reporting == nullptr)
    return;
  if (!(delta >= 0) || isinf(delta))
  {
    if (_owner->_debugStream != nullptr)
      _owner->_debugStream->println(F("writeOnChange(): delta must be 0 or above; kept as it was."));
    return;
  }
  e->reporting->changeDelta = delta;
  e->reporting->minIntervalMs = minIntervalMs;
}

// A name a host sends in a command: ASCII letters, digits, '_', '-' and '.'. Anything else either
// can't be sent (',', '<', '>') or reads differently on the way (spaces, '%', non-ASCII).
static bool _isCommandNameChar(char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '_' || c == '-' || c == '.';
}

bool Blaeck::_hasControlChar(BlaeckString name)
{
  if (name == nullptr)
    return false;
  for (size_t k = 0;; ++k)
  {
    const byte c = static_cast<byte>(name.read(k));
    if (c == 0)
      return false;
    if (c < 0x20 || c == 0x7F)
      return true;
  }
}

bool Blaeck::_nameRefused(BlaeckString name, bool isCommand)
{
  const __FlashStringHelper *why = nullptr;
  size_t length = 0;
  if (name != nullptr)
    while (name.read(length) != 0)
      ++length;
  bool commandChars = true;
  for (size_t k = 0; k < length; ++k)
    if (!_isCommandNameChar(name.read(k)))
      commandChars = false;
  if (length == 0)
    why = F("the name is empty");
  else if (!commandChars)
    why = F("a name a host sends may hold only letters, digits, _, - and .");
  else if (length >= 7 && name.read(0) == 'B' && name.read(1) == 'L' && name.read(2) == 'A' &&
           name.read(3) == 'E' && name.read(4) == 'C' && name.read(5) == 'K' && name.read(6) == '.')
    why = F("BLAECK. is reserved for the built-in commands");
  else if (length >= MAX_PARSED_COMMAND_COUNT)
    why = F("the name is too long to be received");
  else if (_findProperty(name) >= 0)
    why = F("an input or sensor has the name already");
  else if (!isCommand)
  {
    for (uint16_t i = 0; i < _commandSlots(); ++i)
      if (_commandHandlers[i].inUse && BlaeckString(_commandHandlers[i].command) == name)
      {
        why = F("a button or command has the name already");
        break;
      }
  }
  if (why == nullptr)
    return false;
  if (_debugStream != nullptr)
  {
    _debugStream->print(F("Dropped '"));
    if (name != nullptr)
      name.printTo(*_debugStream);
    _debugStream->print(F("': "));
    _debugStream->print(why);
    _debugStream->println('.');
  }
  return true;
}

int Blaeck::_findProperty(BlaeckString name) const
{
  if (name == nullptr)
    return -1;
  for (uint16_t i = 0; i < _propertyCount; ++i)
    if (BlaeckString(_properties[i].name) == name)
      return (int)i;
  return -1;
}

int Blaeck::_registerProperty(byte deviceId, BlaeckString name, uint8_t kind, bool writable, dataType type,
                              void *address, void (*getter)(), uint8_t getterType, uint16_t textSize,
                              BlaeckString options, BlaeckPropertyCallback onChange)
{
#if !BLAECK_ENABLE_IOT
  // Without the IoT part there are no properties; the rest is dropped from the build.
  (void)deviceId, (void)kind, (void)writable, (void)type, (void)address, (void)getter;
  (void)getterType, (void)textSize, (void)options, (void)onChange, (void)name;
  return -1;
#endif
  if (_nameRefused(name, false))
  {
    ++_rejectedPropertyCount;
    return -1;
  }
  if ((address == nullptr && getter == nullptr) ||
      (kind == BLAECK_VALUE_TEXT && getter == nullptr && (textSize < 1 || textSize > 256)))
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("Dropped '"));
      name.printTo(*_debugStream);
      _debugStream->println(F("': no variable, or a text size that isn't 1 to 256."));
    }
    ++_rejectedPropertyCount;
    return -1;
  }
  if (kind == BLAECK_VALUE_ENUM &&
      !blaeck_detail::optionsAccepted(options, _debugStream, name.data(), name.inFlash()))
  {
    ++_rejectedPropertyCount;
    return -1;
  }
  if (_propertyCount >= MAX_TABLE_ENTRIES || !_properties.reserve(_propertyCount + 1))
  {
    _warnNoRoom(F("property"), name);
    ++_rejectedPropertyCount;
    return -1;
  }

  PropertyEntry &p = _properties[_propertyCount];
  // The slot may hold what an earlier, rejected registration left.
  delete p.reporting;
  p.reporting = new (std::nothrow) ReportingState();
  delete p.presentation;
  p.presentation = nullptr;
  if (p.reporting == nullptr || !_storeString(p.name, name) ||
      !_storeString(p.options, kind == BLAECK_VALUE_ENUM ? options : BlaeckString()))
  {
    if (p.reporting == nullptr)
      _warnNoRoom(F("property"), name);
    ++_rejectedPropertyCount;
    return -1;
  }
  p.address = address;
  p.getter = getter;
  p.getterType = getterType;
  p.callback = onChange;
  p.flags = 0;
  p.textSize = kind == BLAECK_VALUE_TEXT ? (getter != nullptr ? 256 : textSize) : 0;
  p.type = type;
  // The member the type makes the live one, so nothing reads the other.
  p.rangeMin = p.rangeMax = p.rangeStep = _makeBound(type, 0.0);
  p.kind = kind;
  p.deviceId = deviceId;
  p.writable = writable;
  _entityCatalogDirty = true;
  return (int)_propertyCount++;
}

void Blaeck::_propertyValue(const PropertyEntry &p, byte *out) const
{
  memset(out, 0, 8);
  switch (p.getterType)
  {
  case blaeck_detail::GETTER_NONE: memcpy(out, p.address, _signalValueSize(p.type)); break;
  case blaeck_detail::GETTER_BOOL: { bool v = reinterpret_cast<bool (*)()>(p.getter)(); memcpy(out, &v, sizeof v); } break;
  case blaeck_detail::GETTER_BYTE: { byte v = reinterpret_cast<byte (*)()>(p.getter)(); memcpy(out, &v, sizeof v); } break;
  case blaeck_detail::GETTER_SHORT: { short v = reinterpret_cast<short (*)()>(p.getter)(); memcpy(out, &v, sizeof v); } break;
  case blaeck_detail::GETTER_USHORT: { unsigned short v = reinterpret_cast<unsigned short (*)()>(p.getter)(); memcpy(out, &v, sizeof v); } break;
  case blaeck_detail::GETTER_INT: { int v = reinterpret_cast<int (*)()>(p.getter)(); memcpy(out, &v, sizeof v); } break;
  case blaeck_detail::GETTER_UINT: { unsigned int v = reinterpret_cast<unsigned int (*)()>(p.getter)(); memcpy(out, &v, sizeof v); } break;
  case blaeck_detail::GETTER_LONG: { long v = reinterpret_cast<long (*)()>(p.getter)(); memcpy(out, &v, sizeof v); } break;
  case blaeck_detail::GETTER_ULONG: { unsigned long v = reinterpret_cast<unsigned long (*)()>(p.getter)(); memcpy(out, &v, sizeof v); } break;
  case blaeck_detail::GETTER_FLOAT: { float v = reinterpret_cast<float (*)()>(p.getter)(); memcpy(out, &v, sizeof v); } break;
  case blaeck_detail::GETTER_DOUBLE: { double v = reinterpret_cast<double (*)()>(p.getter)(); memcpy(out, &v, sizeof v); } break;
  case blaeck_detail::GETTER_LONGLONG: { long long v = reinterpret_cast<long long (*)()>(p.getter)(); memcpy(out, &v, sizeof v); } break;
  default: break;
  }
}

const char *Blaeck::_propertyText(const PropertyEntry &p, bool &inFlash) const
{
  inFlash = false;
  if (p.getterType == blaeck_detail::GETTER_TEXT)
  {
    const char *text = reinterpret_cast<const char *(*)()>(p.getter)();
    return text != nullptr ? text : "";
  }
  return static_cast<const char *>(p.address);
}

// The length of a property's text: up to the terminator, and never past its buffer.
static byte _propertyTextLength(const char *text, uint16_t textSize)
{
  const size_t limit = textSize > 0 ? static_cast<size_t>(textSize - 1) : 255;
  return static_cast<byte>(_textLength(text, false, limit > 255 ? 255 : limit));
}

void Blaeck::_emitPropertyValue(const PropertyEntry &p)
{
  if (p.kind == BLAECK_VALUE_TEXT)
  {
    bool inFlash;
    const char *text = _propertyText(p, inFlash);
    const byte length = _propertyTextLength(text, p.textSize);
    _emitByte(length);
    if (length > 0)
      _emitTextBytes(text, inFlash, length);
    return;
  }
  byte value[8];
  _propertyValue(p, value);
  _emitBytes(value, _signalValueSize(p.type));
}

// Stores v in a variable of the given type. False if it doesn't fit the type.
static bool _storeNumber(void *address, dataType type, double v)
{
  if (!_fitsType(type, v))
    return false;
  switch (type)
  {
  case Blaeck_byte: *(byte *)address = (byte)v; return true;
  case Blaeck_short: case Blaeck_int:
    { int16_t x = (int16_t)v; memcpy(address, &x, 2); } return true;
  case Blaeck_ushort: case Blaeck_uint:
    { uint16_t x = (uint16_t)v; memcpy(address, &x, 2); } return true;
  case Blaeck_long:
    { int32_t x = (int32_t)v; memcpy(address, &x, 4); } return true;
  case Blaeck_ulong:
    { uint32_t x = (uint32_t)v; memcpy(address, &x, 4); } return true;
  case Blaeck_float: { float x = (float)v; memcpy(address, &x, sizeof x); } return true;
  case Blaeck_double: { double x = v; memcpy(address, &x, sizeof x); } return true;
  case Blaeck_longlong:
    { long long x = (long long)v; memcpy(address, &x, sizeof x); } return true;
  default: return false;
  }
}

// Stores a whole number exactly, however few digits the board's double holds. False if it
// doesn't fit the type.
static bool _storeWhole(void *address, dataType type, long long v)
{
  switch (type)
  {
  case Blaeck_byte: if (v < 0 || v > 255) return false; *(byte *)address = (byte)v; return true;
  case Blaeck_short: case Blaeck_int:
    if (v < -32768LL || v > 32767LL)
      return false;
    { int16_t x = (int16_t)v; memcpy(address, &x, 2); } return true;
  case Blaeck_ushort: case Blaeck_uint:
    if (v < 0 || v > 65535LL)
      return false;
    { uint16_t x = (uint16_t)v; memcpy(address, &x, 2); } return true;
  case Blaeck_long:
    if (v < -2147483648LL || v > 2147483647LL)
      return false;
    { int32_t x = (int32_t)v; memcpy(address, &x, 4); } return true;
  case Blaeck_ulong:
    if (v < 0 || v > 4294967295LL)
      return false;
    { uint32_t x = (uint32_t)v; memcpy(address, &x, 4); } return true;
  case Blaeck_longlong: memcpy(address, &v, sizeof v); return true;
  case Blaeck_float: { float x = (float)v; memcpy(address, &x, sizeof x); } return true;
  case Blaeck_double: { double x = (double)v; memcpy(address, &x, sizeof x); } return true;
  default: return false;
  }
}

// A range bound, at the width and the type of the variable it bounds - the same form the
// property's own value goes out in, which is what min, max and step were alone in not doing.
//
// Widening to eight bytes keeps the value and loses the decimal it was written as. A board whose
// double is a float holds 0.01 as 0.00999999977, and a double carrying that exactly is what a
// host offers as a step: Home Assistant counts min + n * step from it and cannot land on 0.05.
// Sent at the variable's own width, the same float reads back as the shortest decimal that names
// it, which is the 0.01 the sketch wrote. A double property is eight bytes wherever it exists:
// on a board whose double is a float, one is registered as a float (BLAECK_DOUBLE_TYPE).
void Blaeck::_emitRangeValue(dataType type, const blaeck_detail::RangeBound &b)
{
  // A bound that does not fit was refused by withRange(), so this cannot be reached with one.
  // Zero is written rather than nothing regardless: the entry's length is measured by writing
  // it twice, and a field that appears in one pass and not the other would misstate it.
  byte value[8];
  memset(value, 0, sizeof value);
  // A whole-number variable's bound is kept as a long long and goes out as one, never through a
  // double: on a board where that is a float, 4000000000 would leave as its 24-bit neighbour.
  if (_isIntegerType(type))
    _storeWhole(value, type, b.asInteger);
  else
    _storeNumber(value, type, b.asDouble);
  _emitBytes(value, _signalValueSize(type));
}

// Reads a whole decimal integer, with an optional sign, into a long long. False if the text is
// anything else or does not fit. avr-libc has no strtoll(), and a double on AVR holds only 24
// bits exactly, so a 64-bit input is read here.
static bool _parseLongLong(const char *text, long long &out)
{
  const bool negative = text[0] == '-';
  const char *p = (text[0] == '-' || text[0] == '+') ? text + 1 : text;
  if (*p == '\0')
    return false;
  const unsigned long long limit = negative ? 9223372036854775808ULL : 9223372036854775807ULL;
  unsigned long long value = 0;
  for (; *p != '\0'; ++p)
  {
    if (*p < '0' || *p > '9')
      return false;
    const unsigned digit = (unsigned)(*p - '0');
    if (value > (limit - digit) / 10)
      return false;
    value = value * 10 + digit;
  }
  out = negative ? (long long)(0ULL - value) : (long long)value;
  return true;
}

// A number as JSON writes one (RFC 8259), with an optional leading '+': a sign, digits without
// leading zeros, an optional fraction and an optional exponent. No spaces, hex, NaN or Infinity.
// Checked here rather than left to strtod(), which accepts more, and differently per platform.
// whole is set for a number with neither fraction nor exponent.
static bool _isNumberText(const char *v, bool &whole)
{
  const char *p = v;
  if (*p == '-' || *p == '+')
    ++p;
  if (*p == '0')
    ++p;
  else if (*p >= '1' && *p <= '9')
    while (*p >= '0' && *p <= '9')
      ++p;
  else
    return false;
  whole = true;
  if (*p == '.')
  {
    ++p;
    if (*p < '0' || *p > '9')
      return false;
    while (*p >= '0' && *p <= '9')
      ++p;
    whole = false;
  }
  if (*p == 'e' || *p == 'E')
  {
    ++p;
    if (*p == '-' || *p == '+')
      ++p;
    if (*p < '0' || *p > '9')
      return false;
    while (*p >= '0' && *p <= '9')
      ++p;
    whole = false;
  }
  return *p == '\0';
}

// The decimal places a step is written with: 0.01 gives 2, 0.5 gives 1, 2 gives 0. A step is
// held as a double, a float on AVR, which no decimal fraction lands on exactly, so the answer is the first
// scaling that leaves a whole number to within the same thousandth the snap itself allows.
// Six is as far as it looks, which is past the point a float distinguishes steps at all.
static int _stepDecimals(double step)
{
  double scale = 1.0;
  for (int decimals = 0; decimals < 6; decimals++)
  {
    const double scaled = step * scale;
    if (fabs(scaled - floor(scaled + 0.5)) < 1e-3)
      return decimals;
    scale *= 10.0;
  }
  return 6;
}

byte Blaeck::_receiveProperty(uint16_t index)
{
#if !BLAECK_ENABLE_IOT
  // Without the IoT part there are no properties; the rest is dropped from the build.
  (void)index;
  return BLAECK_ACK_UNKNOWN;
#endif
  PropertyEntry &p = _properties[index];
  if (_deviceMissing(p.deviceId))
    return BLAECK_ACK_DEVICE_NOT_RESPONDING;
  if (!p.writable)
    return BLAECK_ACK_READ_ONLY;
  if (_parsedParamCount < 1 || _parsedParamPtrs[0] == nullptr)
    return BLAECK_ACK_MISSING_VALUE;
  char *v = const_cast<char *>(_parsedParamPtrs[0]);
  // An empty value is valid only for text, where it clears the field.
  if (v[0] == '\0' && p.kind != BLAECK_VALUE_TEXT)
    return BLAECK_ACK_MISSING_VALUE;

  switch (p.kind)
  {
  case BLAECK_VALUE_NUMBER:
  {
    bool wholeText = false;
    if (!_isNumberText(v, wholeText))
      return BLAECK_ACK_NOT_A_NUMBER;
    // A whole number is read as an integer, exactly, whatever the variable: a double on AVR
    // holds only 24 bits exactly. A whole number needs no step.
    if (wholeText)
    {
      long long whole;
      if (!_parseLongLong(v, whole))
        return BLAECK_ACK_OUT_OF_RANGE;
      // Compared as whole numbers where the variable keeps its range as whole numbers, so a
      // bound past what the board's double names exactly still refuses what it says it does.
      if (p.flags & blaeck_detail::PROPERTY_HAS_RANGE)
      {
        const bool inside = _isIntegerType(p.type)
                                ? (whole >= p.rangeMin.asInteger && whole <= p.rangeMax.asInteger)
                                : ((double)whole >= p.rangeMin.asDouble &&
                                   (double)whole <= p.rangeMax.asDouble);
        if (!inside)
          return BLAECK_ACK_OUT_OF_RANGE;
      }
      if (!_storeWhole(p.address, p.type, whole))
        return BLAECK_ACK_OUT_OF_RANGE;
      return BLAECK_ACK_OK;
    }
    // A 64-bit integer takes digits only: without a 64-bit double on AVR, "1e3" or "12.0" can't
    // be checked exactly.
    if (p.type == Blaeck_longlong)
      return BLAECK_ACK_NOT_AN_INTEGER;
    // The text is a valid number, so strtod() reads all of it the same way everywhere.
    double number = strtod(v, nullptr);
    if (isinf(number))
      return BLAECK_ACK_OUT_OF_RANGE;
    // A fraction cannot be compared as a whole number, so here the range is read as doubles.
    // The variable is narrower than a long long by now, so its bounds survive the reading.
    if ((p.flags & blaeck_detail::PROPERTY_HAS_RANGE) &&
        (number < _boundAsDouble(p.type, p.rangeMin) ||
         number > _boundAsDouble(p.type, p.rangeMax)))
      return BLAECK_ACK_OUT_OF_RANGE;
    // On its step: a value within a thousandth of a step of one is stored as exactly that.
    if (p.flags & blaeck_detail::PROPERTY_HAS_STEP)
    {
      const double rangeMin = _boundAsDouble(p.type, p.rangeMin);
      const double rangeStep = _boundAsDouble(p.type, p.rangeStep);
      const double steps = (number - rangeMin) / rangeStep;
      const double nearest = floor(steps + 0.5);
      if (fabs(steps - nearest) < 1e-3)
      {
        double snapped = rangeMin + nearest * rangeStep;
        // min + n * step is arithmetic on two binary numbers, neither of which is the decimal it was
        // written as, so the sum lands beside the step rather than on it - 0.1 came back as
        // 0.099999994, a whole float step out and worse than the value that arrived. Rounding
        // the sum to the step's own decimals puts it back: the scaling is by an exact power of
        // ten, so the result is the nearest number there is to the decimal intended. Skipped
        // where the scaling would run out of digits, which the range a step this fine can
        // cover does not reach.
        double scale = 1.0;
        for (int i = _stepDecimals(rangeStep); i > 0; i--)
          scale *= 10.0;
        if (fabs(snapped) * scale < 1e15)
          snapped = floor(fabs(snapped) * scale + 0.5) / scale * (snapped < 0.0 ? -1.0 : 1.0);
        number = snapped;
      }
    }
    if (_isIntegerType(p.type))
    {
      if (number != floor(number))
        return BLAECK_ACK_NOT_AN_INTEGER;
    }
    if (!_storeNumber(p.address, p.type, number))
      return BLAECK_ACK_OUT_OF_RANGE;
    return BLAECK_ACK_OK;
  }
  case BLAECK_VALUE_BOOL:
    if (strcmp(v, "0") != 0 && strcmp(v, "1") != 0)
      return BLAECK_ACK_BAD_SWITCH;
    *(bool *)p.address = v[0] == '1';
    return BLAECK_ACK_OK;
  case BLAECK_VALUE_ENUM:
  {
    // An option's name, matched exactly, or its index.
    long option = _flashCsvIndexOf(p.options, v);
    if (option < 0)
    {
      char *end = nullptr;
      long n = strtol(v, &end, 10);
      if (end != v && *end == '\0')
        option = n;
    }
    if (option < 0 || option >= (long)_flashCsvOptionCount(p.options) ||
        !_storeNumber(p.address, p.type, (double)option))
      return BLAECK_ACK_BAD_SELECT;
    return BLAECK_ACK_OK;
  }
  case BLAECK_VALUE_TEXT:
  {
    const size_t length = strlen(v);
    if (length + 1 > p.textSize)
      return BLAECK_ACK_TOO_LONG;
    memcpy(p.address, v, length + 1);
    return BLAECK_ACK_OK;
  }
  default:
    return BLAECK_ACK_UNKNOWN;
  }
}

bool Blaeck::_writePropertyFrame(uint16_t index)
{
#if !BLAECK_ENABLE_IOT
  // Without the IoT part there are no properties; the rest is dropped from the build.
  (void)index;
  return false;
#endif
  if (!_mayWriteFrame() || index >= _propertyCount)
    return false;
  // A host must know the property before a value of it arrives.
  _flushCatalogs();
  PropertyEntry &p = _properties[index];
  ReportingState &r = *p.reporting;

  // The value is read once, so the frame and the new baseline agree.
  byte value[8];
  bool inFlash = false;
  const char *text = nullptr;
  bool baseline = true;
  if (p.kind == BLAECK_VALUE_TEXT)
  {
    text = _propertyText(p, inFlash);
    baseline = _prepareTextSnapshot(r, _propertyTextLength(text, p.textSize));
  }
  else
    _propertyValue(p, value);

  // Layout: Property (0x95) in the protocol spec.
  if (!_frameOpen(0x95, 0))
    return false;
  _emitByte((byte)(index & 0xFF));
  _emitByte((byte)((index >> 8) & 0xFF));
  _emitByte(_dtypeCode(p.type));
  if (p.kind == BLAECK_VALUE_TEXT)
  {
    const byte length = _propertyTextLength(text, p.textSize);
    _emitByte(length);
    if (length > 0)
      _emitTextBytes(text, inFlash, length);
  }
  else
    _emitBytes(value, _signalValueSize(p.type));
  if (!_frameClose())
  {
    r.valid = false;
    return false;
  }
  if (baseline)
  {
    if (p.kind == BLAECK_VALUE_TEXT)
    {
      // Captured up to the same length the frame carried.
      r.textLength = _propertyTextLength(text, p.textSize);
      memcpy(r.text, text, r.textLength);
      r.text[r.textLength] = '\0';
    }
    else
      _captureValue(r, p.type, value, false);
  }
  r.valid = baseline;
  r.lastWriteMs = static_cast<uint32_t>(millis());
  return true;
}

// Whether a property differs from its baseline by at least its writeOnChange() delta.
static bool _propertyChanged(const ReportingState &r, dataType type, const byte *value,
                             const char *text, byte textLength)
{
  if (!r.valid)
    return true;
  if (type == Blaeck_string)
    return textLength != r.textLength || (textLength != 0 && memcmp(text, r.text, textLength) != 0);
  return _valueChanged(r, type, value, false, r.changeDelta);
}

void Blaeck::_writeChangedProperties()
{
#if !BLAECK_ENABLE_IOT
  // Without the IoT part there are no properties; the rest is dropped from the build.
  return;
#endif
  const uint32_t now = static_cast<uint32_t>(millis());
  for (uint16_t i = 0; i < _propertyCount; ++i)
  {
    PropertyEntry &p = _properties[i];
    ReportingState &r = *p.reporting;
    if (_deviceMissing(p.deviceId))
      continue;
    if (r.valid && static_cast<uint32_t>(now - r.lastWriteMs) < r.minIntervalMs)
      continue;
    byte value[8];
    const char *text = nullptr;
    byte textLength = 0;
    if (p.kind == BLAECK_VALUE_TEXT)
    {
      bool inFlash;
      text = _propertyText(p, inFlash);
      textLength = _propertyTextLength(text, p.textSize);
    }
    else
      _propertyValue(p, value);
    if (_propertyChanged(r, p.type, value, text, textLength))
      if (!_writePropertyFrame(i))
        return;
  }
}

void Blaeck::_writePropertyByName(byte deviceId, BlaeckString name)
{
  const int index = _findProperty(name);
  if (index < 0 || _properties[index].deviceId != deviceId)
  {
    if (_debugStream != nullptr)
    {
      _debugStream->print(F("writeProperty(): no property '"));
      if (name != nullptr)
        name.printTo(*_debugStream);
      _debugStream->println(F("' on this device."));
    }
    return;
  }
  _writePropertyFrame((uint16_t)index);
}

void Blaeck::writeEntities()
{
  writeEntities(0);
}

void Blaeck::writeEntities(unsigned long msg_id)
{
  _entityCatalogDirty = false;
  writeEntitiesFrame(msg_id);
}

void Blaeck::writeEntitiesFrame(unsigned long msg_id)
{
  // Layout: Entity List (0x90) in the protocol spec: the properties, then the events, then the
  // buttons, each kind in table order.
  if (!_frameOpen(0x90, msg_id))
    return;
#if BLAECK_ENABLE_IOT
  for (uint16_t i = 0; i < _propertyCount; ++i)
  {
    const PropertyEntry &p = _properties[i];
    const blaeck_detail::PropertyPresentation *pr = p.presentation;
    uint32_t flags = p.flags | (p.writable ? 0x3UL : 0x1UL);
    if (pr != nullptr)
    {
      if (pr->unit != nullptr)
        flags |= blaeck_detail::PROPERTY_HAS_UNIT;
      if (pr->displayName != nullptr)
        flags |= blaeck_detail::PROPERTY_HAS_DISPLAY_NAME;
      if (pr->icon != nullptr)
        flags |= blaeck_detail::PROPERTY_HAS_ICON;
      if (pr->deviceClass != nullptr)
        flags |= blaeck_detail::PROPERTY_HAS_DEVICE_CLASS;
    }
    else
      flags &= ~blaeck_detail::PROPERTY_HAS_DISPLAY_PRECISION;

    // The value is read once, before the entry is measured: a getter may answer differently
    // the second time, and the length must match the bytes that follow it.
    bool textInFlash = false;
    const char *text = nullptr;
    byte textLength = 0;
    byte value[8];
    if (p.kind == BLAECK_VALUE_TEXT)
    {
      text = _propertyText(p, textInFlash);
      textLength = _propertyTextLength(text, p.textSize);
    }
    else
      _propertyValue(p, value);

    _emitEntry(p.deviceId, 0, [&]()
    {
      _emitFlashStr0(p.name);
      _emitByte(p.kind);
      _emitByte((byte)(flags & 0xFF));
      _emitByte((byte)((flags >> 8) & 0xFF));
      _emitByte((byte)((flags >> 16) & 0xFF));
      _emitByte((byte)((flags >> 24) & 0xFF));
      _emitByte(_dtypeCode(p.type));
      if (p.kind == BLAECK_VALUE_TEXT)
      {
        _emitByte(textLength);
        if (textLength > 0)
          _emitTextBytes(text, textInFlash, textLength);
      }
      else
        _emitBytes(value, _signalValueSize(p.type));
      if (p.kind == BLAECK_VALUE_ENUM)
        _emitFlashStr0(p.options);
      // A text holds at most 255 bytes; a text from a function has no buffer to be smaller.
      if (p.kind == BLAECK_VALUE_TEXT)
        _emitByte(p.textSize > 0 ? (byte)(p.textSize - 1) : (byte)255);
      if (flags & blaeck_detail::PROPERTY_HAS_RANGE)
      {
        _emitRangeValue(p.type, p.rangeMin);
        _emitRangeValue(p.type, p.rangeMax);
      }
      if (flags & blaeck_detail::PROPERTY_HAS_STEP)
        _emitRangeValue(p.type, p.rangeStep);
      if (flags & blaeck_detail::PROPERTY_HAS_UNIT)
        _emitFlashStr0(pr->unit);
      if (flags & blaeck_detail::PROPERTY_HAS_DISPLAY_NAME)
        _emitFlashStr0(pr->displayName);
      if (flags & blaeck_detail::PROPERTY_HAS_ICON)
        _emitFlashStr0(pr->icon);
      if (flags & blaeck_detail::PROPERTY_HAS_DEVICE_CLASS)
        _emitFlashStr0(pr->deviceClass);
      if (flags & blaeck_detail::PROPERTY_HAS_DISPLAY_PRECISION)
        _emitByte(pr->displayPrecision);
    });
  }

  for (uint16_t i = 0; i < _eventChannelSlots(); i++)
  {
    const EventChannelEntry &e = _eventChannels[i];
    if (!e.inUse)
      continue;

    // Laid out as a button's: display name, icon, device class, category, disabled by default.
    uint16_t flags = (uint16_t)((e.category & 0x03) << 3);
    if (e.displayName != nullptr)
      flags |= 0x0001;
    if (e.icon != nullptr)
      flags |= 0x0002;
    if (e.deviceClass != nullptr)
      flags |= 0x0004;
    if (e.disabledByDefault)
      flags |= 0x0020;

    uint16_t typeCount = 0;
    for (uint16_t t = 0; t < _eventTypeCount; t++)
      if (_eventTypes[t].channelIndex == i)
        typeCount++;

    _emitEntry(e.deviceId, 1, [&]()
    {
      if (e.nameInFlash)
        _emitFlashStr0(reinterpret_cast<const __FlashStringHelper *>(e.name));
      else
        _emitStr0(e.name);
      _emitByte((byte)(flags & 0xFF));
      _emitByte((byte)((flags >> 8) & 0xFF));
      if (flags & 0x0001)
        _emitFlashStr0(e.displayName);
      if (flags & 0x0002)
        _emitFlashStr0(e.icon);
      if (flags & 0x0004)
        _emitFlashStr0(e.deviceClass);
      _emitByte((byte)(typeCount & 0xFF));
      _emitByte((byte)((typeCount >> 8) & 0xFF));
      for (uint16_t t = 0; t < _eventTypeCount; t++)
        if (_eventTypes[t].channelIndex == i)
          _emitEventType0(_eventTypes[t]);
    });
  }

  for (uint16_t i = 0; i < _commandSlots(); i++)
  {
    const CommandHandlerEntry &e = _commandHandlers[i];
    // Plain commands are not listed.
    if (!e.inUse || e.press == nullptr)
      continue;

    uint16_t flags = (uint16_t)((e.category & 0x03) << 3);
    if (e.displayName != nullptr)
      flags |= 0x0001;
    if (e.icon != nullptr)
      flags |= 0x0002;
    if (e.deviceClass != nullptr)
      flags |= 0x0004;
    if (e.disabledByDefault)
      flags |= 0x0020;

    _emitEntry(e.deviceId, 2, [&]()
    {
      _emitStr0(e.command);
      _emitByte((byte)(flags & 0xFF));
      _emitByte((byte)((flags >> 8) & 0xFF));
      if (flags & 0x0001)
        _emitFlashStr0(e.displayName);
      if (flags & 0x0002)
        _emitFlashStr0(e.icon);
      if (flags & 0x0004)
        _emitFlashStr0(e.deviceClass);
    });
  }
#endif
  if (!_frameClose())
    return;
  // The list carried each current value, so none is due again until it changes.
  const uint32_t now = static_cast<uint32_t>(millis());
  for (uint16_t i = 0; i < _propertyCount; ++i)
  {
    PropertyEntry &p = _properties[i];
    ReportingState &r = *p.reporting;
    r.valid = false;
    if (p.kind == BLAECK_VALUE_TEXT)
    {
      bool inFlash;
      const char *text = _propertyText(p, inFlash);
      const byte length = _propertyTextLength(text, p.textSize);
      if (!_prepareTextSnapshot(r, length))
        continue;
      r.textLength = length;
      memcpy(r.text, text, length);
      r.text[length] = '\0';
    }
    else
    {
      byte value[8];
      _propertyValue(p, value);
      _captureValue(r, p.type, value, false);
    }
    r.valid = true;
    r.lastWriteMs = now;
  }
}

int BlaeckDeviceBase::_registerProperty(BlaeckString name, uint8_t kind, bool writable, dataType type,
                                        void *address, void (*getter)(), uint8_t getterType,
                                        uint16_t textSize, BlaeckString options,
                                        BlaeckPropertyCallback onChange)
{
  return _core != nullptr ? _core->_registerProperty(_deviceId, name, kind, writable, type, address, getter,
                                                     getterType, textSize, options, onChange)
                          : -1;
}

void BlaeckDeviceBase::writeProperty(BlaeckString name)
{
  if (_core != nullptr)
    _core->_writePropertyByName(_deviceId, name);
}

#define BLAECK_NUMBER_INPUT(T, DT)                                                                    \
  BlaeckNumberPropertyRef BlaeckDeviceBase::addNumberInput(BlaeckString name, T *value, BlaeckPropertyCallback onChange) \
  {                                                                                                   \
    return BlaeckNumberPropertyRef(_core, (int16_t)_registerProperty(name, BLAECK_VALUE_NUMBER, true, DT, value, \
                                   nullptr, blaeck_detail::GETTER_NONE, 0, BlaeckString(), onChange)); \
  }
BLAECK_NUMBER_INPUT(byte, Blaeck_byte)
BLAECK_NUMBER_INPUT(short, Blaeck_short)
BLAECK_NUMBER_INPUT(unsigned short, Blaeck_ushort)
BLAECK_NUMBER_INPUT(int, BLAECK_INT_TYPE)
BLAECK_NUMBER_INPUT(unsigned int, BLAECK_UINT_TYPE)
BLAECK_NUMBER_INPUT(long, Blaeck_long)
BLAECK_NUMBER_INPUT(unsigned long, Blaeck_ulong)
BLAECK_NUMBER_INPUT(long long, Blaeck_longlong)
BLAECK_NUMBER_INPUT(float, Blaeck_float)
BLAECK_NUMBER_INPUT(double, BLAECK_DOUBLE_TYPE)
#undef BLAECK_NUMBER_INPUT

BlaeckTextPropertyRef BlaeckDeviceBase::addTextInput(BlaeckString name, char *buffer, size_t size,
                                                     BlaeckPropertyCallback onChange)
{
  return BlaeckTextPropertyRef(_core, (int16_t)_registerProperty(name, BLAECK_VALUE_TEXT, true, Blaeck_string, buffer,
                               nullptr, blaeck_detail::GETTER_NONE, size > 0xFFFF ? 0 : (uint16_t)size,
                               BlaeckString(), onChange));
}

BlaeckPropertyRef BlaeckDeviceBase::addSwitch(BlaeckString name, bool *value, BlaeckPropertyCallback onChange)
{
  return BlaeckPropertyRef(_core, (int16_t)_registerProperty(name, BLAECK_VALUE_BOOL, true, Blaeck_bool, value,
                           nullptr, blaeck_detail::GETTER_NONE, 0, BlaeckString(), onChange));
}

#define BLAECK_SELECT(T, DT)                                                                          \
  BlaeckPropertyRef BlaeckDeviceBase::addSelect(BlaeckString name, T *index, BlaeckString options,   \
                                                BlaeckPropertyCallback onChange)                      \
  {                                                                                                   \
    return BlaeckPropertyRef(_core, (int16_t)_registerProperty(name, BLAECK_VALUE_ENUM, true, DT, index, \
                             nullptr, blaeck_detail::GETTER_NONE, 0, options, onChange));             \
  }
BLAECK_SELECT(byte, Blaeck_byte)
BLAECK_SELECT(short, Blaeck_short)
BLAECK_SELECT(unsigned short, Blaeck_ushort)
BLAECK_SELECT(int, BLAECK_INT_TYPE)
BLAECK_SELECT(unsigned int, BLAECK_UINT_TYPE)
BLAECK_SELECT(long, Blaeck_long)
BLAECK_SELECT(unsigned long, Blaeck_ulong)
#undef BLAECK_SELECT

#define BLAECK_NUMBER_SENSOR(T, DT, G)                                                                \
  BlaeckNumberPropertyRef BlaeckDeviceBase::addSensor(BlaeckString name, T *value)                  \
  {                                                                                                   \
    return BlaeckNumberPropertyRef(_core, (int16_t)_registerProperty(name, BLAECK_VALUE_NUMBER, false, DT, value, \
                                   nullptr, blaeck_detail::GETTER_NONE, 0, BlaeckString(), nullptr)); \
  }                                                                                                   \
  BlaeckNumberPropertyRef BlaeckDeviceBase::addSensor(BlaeckString name, T (*value)())              \
  {                                                                                                   \
    return BlaeckNumberPropertyRef(_core, (int16_t)_registerProperty(name, BLAECK_VALUE_NUMBER, false, DT, nullptr, \
                                   reinterpret_cast<void (*)()>(value), G, 0, BlaeckString(), nullptr)); \
  }
BLAECK_NUMBER_SENSOR(byte, Blaeck_byte, blaeck_detail::GETTER_BYTE)
BLAECK_NUMBER_SENSOR(short, Blaeck_short, blaeck_detail::GETTER_SHORT)
BLAECK_NUMBER_SENSOR(unsigned short, Blaeck_ushort, blaeck_detail::GETTER_USHORT)
BLAECK_NUMBER_SENSOR(int, BLAECK_INT_TYPE, blaeck_detail::GETTER_INT)
BLAECK_NUMBER_SENSOR(unsigned int, BLAECK_UINT_TYPE, blaeck_detail::GETTER_UINT)
BLAECK_NUMBER_SENSOR(long, Blaeck_long, blaeck_detail::GETTER_LONG)
BLAECK_NUMBER_SENSOR(unsigned long, Blaeck_ulong, blaeck_detail::GETTER_ULONG)
BLAECK_NUMBER_SENSOR(long long, Blaeck_longlong, blaeck_detail::GETTER_LONGLONG)
BLAECK_NUMBER_SENSOR(float, Blaeck_float, blaeck_detail::GETTER_FLOAT)
BLAECK_NUMBER_SENSOR(double, BLAECK_DOUBLE_TYPE, blaeck_detail::GETTER_DOUBLE)
#undef BLAECK_NUMBER_SENSOR

BlaeckPropertyRef BlaeckDeviceBase::addSensor(BlaeckString name, bool *value)
{
  return BlaeckPropertyRef(_core, (int16_t)_registerProperty(name, BLAECK_VALUE_BOOL, false, Blaeck_bool, value,
                           nullptr, blaeck_detail::GETTER_NONE, 0, BlaeckString(), nullptr));
}

BlaeckPropertyRef BlaeckDeviceBase::addSensor(BlaeckString name, bool (*value)())
{
  return BlaeckPropertyRef(_core, (int16_t)_registerProperty(name, BLAECK_VALUE_BOOL, false, Blaeck_bool, nullptr,
                           reinterpret_cast<void (*)()>(value), blaeck_detail::GETTER_BOOL, 0, BlaeckString(),
                           nullptr));
}

#define BLAECK_ENUM_SENSOR(T, DT, G)                                                                  \
  BlaeckPropertyRef BlaeckDeviceBase::addSensor(BlaeckString name, T *index, BlaeckString options)  \
  {                                                                                                   \
    return BlaeckPropertyRef(_core, (int16_t)_registerProperty(name, BLAECK_VALUE_ENUM, false, DT, index, \
                             nullptr, blaeck_detail::GETTER_NONE, 0, options, nullptr));              \
  }                                                                                                   \
  BlaeckPropertyRef BlaeckDeviceBase::addSensor(BlaeckString name, T (*index)(), BlaeckString options) \
  {                                                                                                   \
    return BlaeckPropertyRef(_core, (int16_t)_registerProperty(name, BLAECK_VALUE_ENUM, false, DT, nullptr, \
                             reinterpret_cast<void (*)()>(index), G, 0, options, nullptr));          \
  }
BLAECK_ENUM_SENSOR(byte, Blaeck_byte, blaeck_detail::GETTER_BYTE)
BLAECK_ENUM_SENSOR(short, Blaeck_short, blaeck_detail::GETTER_SHORT)
BLAECK_ENUM_SENSOR(unsigned short, Blaeck_ushort, blaeck_detail::GETTER_USHORT)
BLAECK_ENUM_SENSOR(int, BLAECK_INT_TYPE, blaeck_detail::GETTER_INT)
BLAECK_ENUM_SENSOR(unsigned int, BLAECK_UINT_TYPE, blaeck_detail::GETTER_UINT)
BLAECK_ENUM_SENSOR(long, Blaeck_long, blaeck_detail::GETTER_LONG)
BLAECK_ENUM_SENSOR(unsigned long, Blaeck_ulong, blaeck_detail::GETTER_ULONG)
#undef BLAECK_ENUM_SENSOR

BlaeckTextPropertyRef BlaeckDeviceBase::addSensor(BlaeckString name, const char *buffer, size_t size)
{
  return BlaeckTextPropertyRef(_core, (int16_t)_registerProperty(name, BLAECK_VALUE_TEXT, false, Blaeck_string,
                               const_cast<char *>(buffer), nullptr, blaeck_detail::GETTER_NONE,
                               size > 0xFFFF ? 0 : (uint16_t)size, BlaeckString(), nullptr));
}

BlaeckTextPropertyRef BlaeckDeviceBase::addSensor(BlaeckString name, const char *(*value)())
{
  return BlaeckTextPropertyRef(_core, (int16_t)_registerProperty(name, BLAECK_VALUE_TEXT, false, Blaeck_string,
                               nullptr, reinterpret_cast<void (*)()>(value), blaeck_detail::GETTER_TEXT, 0,
                               BlaeckString(), nullptr));
}

} // namespace blaeck
