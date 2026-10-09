/*
        File: Blaeck.h
        Author: Sebastian Strobl

    Self-describing telemetry over a Stream or a supplied TCP server.
    One device owns its signals, commands, channels and connection state.
*/

#ifndef BLAECK_H
#define BLAECK_H

#include <Arduino.h>
#include "detail/BlaeckDefaults.h"
#include "BlaeckVersion.h"
#include "detail/BlaeckServerAdapter.h"
#include "detail/BlaeckCRC32.h"
#include "detail/BlaeckString.h"
#include "detail/BlaeckChunkList.h"
#include <new>
#include <string.h>
#include <limits.h>

// "Host" in these comments means the program on the other end of the link, which reads
// what the device sends and may pass commands back to it.

// Compile-time settings. Each one below can be overridden, e.g.
//   #define BLAECK_COMMAND_MAX_CHARS_DEFAULT 128
//
// An override has to reach both your sketch and the library's .cpp files. Several of these
// size members of the class, and if the files see different values, memory is corrupted
// without any error.
//
//   PlatformIO:   build_flags = -DBLAECK_COMMAND_MAX_CHARS_DEFAULT=128
//   Arduino IDE:  a config header in the sketch folder is not found without extra setup.
//                 detail/BlaeckDefaults.h loads it, and docs/configuration.md lists the ways.
#ifndef BLAECK_COMMAND_MAX_CHARS_DEFAULT
  // Command buffer bytes, including the terminator. 128 fits a 32-byte text value even
  // when percent-encoded. There are two fixed buffers plus one per allocated TCP slot,
  // so AVR boards smaller than a Mega get 48.
  #if defined(__AVR__)
    #if defined(RAMEND) && (RAMEND >= 0x10FF)
      #define BLAECK_COMMAND_MAX_CHARS_DEFAULT 128
    #else
      #define BLAECK_COMMAND_MAX_CHARS_DEFAULT 48
    #endif
  #else
    #define BLAECK_COMMAND_MAX_CHARS_DEFAULT 128
  #endif
#endif

static_assert(BLAECK_COMMAND_MAX_CHARS_DEFAULT >= 1 &&
                  BLAECK_COMMAND_MAX_CHARS_DEFAULT <= 65535UL,
              "BLAECK_COMMAND_MAX_CHARS_DEFAULT must be between 1 and 65535 bytes.");


// The IoT part: inputs, sensors, buttons and events, listed in the 0x90 Entity List. Set it to
// 0 to save SRAM and flash on a small board. The API stays, so a sketch compiles either way:
// their add… calls store nothing, and the entity list goes out empty so a host does not wait
// for it. Signals and plain commands work either way.
#ifndef BLAECK_ENABLE_IOT
  #define BLAECK_ENABLE_IOT 1
#endif

// The built-in commands. read() matches against these names, and the build fails if one is
// too long for the parse buffer. A new built-in has to be added to the list below as well.
#define BLAECK_BUILTIN_WRITE_DATA "BLAECK.WRITE_DATA"
#define BLAECK_BUILTIN_GET_DEVICES "BLAECK.GET_DEVICES"
#define BLAECK_BUILTIN_WRITE_ENTITIES "BLAECK.WRITE_ENTITIES"
#define BLAECK_BUILTIN_INTERVAL_START "BLAECK.INTERVAL_START"
#define BLAECK_BUILTIN_INTERVAL_STOP "BLAECK.INTERVAL_STOP"
#define BLAECK_BUILTIN_DATA_START "BLAECK.DATA_START"
#define BLAECK_BUILTIN_DATA_STOP "BLAECK.DATA_STOP"
#define BLAECK_BUILTIN_ENTITIES_START "BLAECK.ENTITIES_START"
#define BLAECK_BUILTIN_ENTITIES_STOP "BLAECK.ENTITIES_STOP"

// Sent as the device name when the sketch sets none.
#define BLAECK_DEVICE_NAME_UNNAMED "Unnamed"

#define BLAECK_BUILTIN_COMMAND_LIST(X)  \
  X(BLAECK_BUILTIN_WRITE_DATA)          \
  X(BLAECK_BUILTIN_GET_DEVICES)         \
  X(BLAECK_BUILTIN_WRITE_ENTITIES)      \
  X(BLAECK_BUILTIN_INTERVAL_START)      \
  X(BLAECK_BUILTIN_INTERVAL_STOP)       \
  X(BLAECK_BUILTIN_DATA_START)          \
  X(BLAECK_BUILTIN_DATA_STOP)           \
  X(BLAECK_BUILTIN_ENTITIES_START)      \
  X(BLAECK_BUILTIN_ENTITIES_STOP)


#ifndef BLAECK_USB_PACKET_BYTES
  #define BLAECK_USB_PACKET_BYTES 64
#endif

// Nagle's algorithm off where the server/client supports it. Set it false in
// BlaeckConfig.h to favour throughput instead.
#ifndef BLAECK_TCP_NO_DELAY_DEFAULT
  #define BLAECK_TCP_NO_DELAY_DEFAULT true
#endif

// How long closing a connection waits for the peer to close its end, where the client
// supports setConnectionTimeout() (the Ethernet library: 1000 ms otherwise). The FIN goes out
// at once either way; this only bounds the wait for an unresponsive or dead peer.
#ifndef BLAECK_TCP_STOP_TIMEOUT_MS
  #define BLAECK_TCP_STOP_TIMEOUT_MS 100
#endif

// One namespace and set of types for both transports.
namespace blaeck
{

typedef enum DataType : uint8_t
{
  Blaeck_bool,
  Blaeck_byte,
  Blaeck_short,
  Blaeck_ushort,
  Blaeck_int,
  Blaeck_uint,
  Blaeck_long,
  Blaeck_ulong,
  Blaeck_float,
  Blaeck_double,
  Blaeck_string,
  Blaeck_longlong
} dataType;

// The enumerators are not the wire codes and nothing treats them as such: _dtypeCode() is
// the one mapping, and the schema hash, the device list and the property frames all go through
// it. Reorder this list or insert a type and nothing on the wire moves.

// How a sensor's value behaves over time, so a host knows whether to keep statistics on it.
// NONE, the default, means no statistics.
enum BlaeckStateClass
{
  BLAECK_STATE_CLASS_NONE = 0,
  // Goes up and down, meaningful at any moment.
  BLAECK_STATE_CLASS_MEASUREMENT = 1,
  // A running sum.
  BLAECK_STATE_CLASS_TOTAL = 2,
  // A running sum that only grows, and may reset to zero.
  BLAECK_STATE_CLASS_TOTAL_INCREASING = 3,
  // An angle, averaged the short way round: 350 and 10 average to 0, not 180.
  BLAECK_STATE_CLASS_MEASUREMENT_ANGLE = 4
};

enum BlaeckIntervalMode : uint8_t
{
  BLAECK_OFF,
  BLAECK_ALWAYS,
  BLAECK_ON_CHANGE
};

/*!
  @brief   A zero threshold: any difference from the last sent value qualifies.

  The minimum reporting interval still applies. This is a threshold, not a mode;
  numeric zero has the same meaning.

  @code
    device.addSignal(F("Temperature"), &Temperature)
        .writeAtInterval(BLAECK_ON_CHANGE, BLAECK_ANY_CHANGE)
        .writeOnChange(BLAECK_ANY_CHANGE);
  @endcode
*/
constexpr double BLAECK_ANY_CHANGE = 0;

// The default of every timestamp parameter: blaeck takes the timestamp from the timestamp mode
// when the value is sent (micros(), or the clock given to setTimestampMode()). Never a real
// timestamp - in microseconds it lies about 585,000 years ahead - so 0 stays one.
constexpr unsigned long long BLAECK_NOW = ~0ULL;

struct ReportingState
{
  byte value[sizeof(long long)] = {};
  double intervalDelta = 0;
  double changeDelta = 0;
  uint32_t minIntervalMs = 100;
  uint32_t lastWriteMs = 0;
  bool immediate = false;
  bool valid = false;
  bool memoryError = false;
  char *text = nullptr;
  uint16_t textCapacity = 0;
  byte textLength = 0;
  ~ReportingState() { delete[] text; }
};

struct Signal
{
  // A heap copy, or a flash pointer when NameInFlash. Read it only through the _signalName*
  // helpers: on AVR, reading flash as RAM returns garbage without crashing.
  const char *SignalName = nullptr;
  dataType DataType;
  void *Address;
  // Bit-fields to keep the entry small. C++11 allows no initializer on them, so they are set
  // when the signal is registered.
  uint8_t IntervalMode : 2;
  uint8_t Selected : 1;
  uint8_t NameInFlash : 1;
  uint8_t TextInFlash : 1;
  // Set when NameSuffix is in use, so a suffix of 0 still counts.
  uint8_t HasSuffix : 1;
  // Appended to the name as decimal digits, e.g. Sine_ + 3 gives Sine_3.
  uint8_t NameSuffix;
  // The device from addDevice() the signal belongs to, 0 for the board itself.
  uint8_t DeviceId = 0;
  // The signal's number on the wire: its position in the device list, which groups signals by
  // device. Set by _computeSchemaHash() whenever the signals change.
  uint16_t WireIndex = 0;
  ReportingState *Reporting = nullptr;
};

enum BlaeckTimestampMode
{
  BLAECK_NO_TIMESTAMP = 0,
  BLAECK_MICROS = 1,
  BLAECK_UNIX = 2
};

// paramCount is 0 for a command sent without parameters.
typedef void (*BlaeckCommandHandler)(const char *command, const char *const *params, byte paramCount);
// Returns true if it took a command nothing else has, which is then accepted; false leaves it unknown.
typedef bool (*BlaeckAnyCommandHandler)(const char *command, const char *const *params, byte paramCount);

// Runs on each press of a button. A press carries no value; fixed arguments go in a lambda:
// addButton("ACTIVATE_ALL", []() { activateRange(1, 40); }).
typedef void (*BlaeckButtonFunction)();

// Where a host files a control or value. CONFIG is a device setting, DIAGNOSTIC is information
// about the device. Either keeps it off a host's default views.
enum BlaeckEntityCategory
{
  BLAECK_CAT_NONE = 0,      // a main control (default)
  BLAECK_CAT_CONFIG = 1,
  BLAECK_CAT_DIAGNOSTIC = 2
};

// How a host should show a number input. Only a hint; the range still bounds it.
enum BlaeckNumberMode
{
  BLAECK_NUMBER_MODE_AUTO = 0,   // the host decides (default)
  BLAECK_NUMBER_MODE_BOX = 1,    // a typed field
  BLAECK_NUMBER_MODE_SLIDER = 2
};

// How a host should show a text input. PASSWORD only masks the field on screen; the value
// still travels as plain text.
enum BlaeckTextMode
{
  BLAECK_TEXT_MODE_PLAIN = 0,   // default
  BLAECK_TEXT_MODE_PASSWORD = 1
};

// Why a command was accepted or rejected, sent back to the host after each command.
enum BlaeckCommandAckReason
{
  BLAECK_ACK_OK = 0,            // accepted
  BLAECK_ACK_UNKNOWN = 1,       // no input, sensor, button or command of that name
  BLAECK_ACK_OUT_OF_RANGE = 2,  // number outside [min, max], or outside what its variable holds
  BLAECK_ACK_BAD_SWITCH = 3,    // switch value not 0 or 1
  BLAECK_ACK_BAD_SELECT = 4,    // not one of the select's options
  BLAECK_ACK_TOO_LONG = 5,      // text longer than its buffer holds
  BLAECK_ACK_MISSING_VALUE = 6, // an input without its value
  BLAECK_ACK_TRUNCATED = 7,     // too long or too many parameters to receive whole
  BLAECK_ACK_DEVICE_NOT_RESPONDING = 8, // its device from addDevice() is marked missing
  BLAECK_ACK_NOT_AN_INTEGER = 9, // a number with a fraction for an input bound to an integer
  BLAECK_ACK_READ_ONLY = 10,     // a sensor, which a host cannot set
  BLAECK_ACK_NOT_A_NUMBER = 11   // a number input got a value that isn't a number
};

// Runs after a host has set an input, not when the sketch changes the variable itself. The
// variable already holds the new value.
typedef void (*BlaeckPropertyCallback)();

// What a property's value is, as listed in the entity list. With the access, it is what the
// sketch declared: addNumberInput() is a writable number, addSensor() with a bool a read-only
// bool, and so on.
enum BlaeckValueKind : uint8_t
{
  BLAECK_VALUE_NUMBER = 0,
  BLAECK_VALUE_BOOL = 1,
  BLAECK_VALUE_ENUM = 2,
  BLAECK_VALUE_TEXT = 3
};


class BlaeckSignalRef;
class BlaeckCommandRefBase;
class BlaeckButtonRef;
class BlaeckEventRef;
class BlaeckPropertyRef;
class BlaeckNumberPropertyRef;
class BlaeckTextPropertyRef;
class BlaeckDeviceRef;
class Blaeck;

// Returned by begin() to set the connection limit and the debug stream, e.g.
//
//   device.begin(server).withClients(2).withDebugStream(&Serial);
//
// Tables need no size: each grows as entries are added.
//
// Defined before Blaeck so that editors can offer its methods on the chain.
class BlaeckBeginRef
{
public:
  explicit BlaeckBeginRef(Blaeck *owner) : _owner(owner) {}

  /*!
    @brief   Sets how many connections the device accepts at once.

    Hosts and terminals together. Each connection takes a receive buffer of
    BLAECK_COMMAND_MAX_CHARS_DEFAULT bytes, 128 on a Mega. A connection beyond the
    limit is closed at once. Set it before the first tick(); later it is refused.

    @note    Keep at least 2, so a host reconnecting after a dropped link finds a
             free slot and takes over from its dead connection. With 1, it waits
             until the network stack gives the dead connection up.

    @param   count  1 to 255. The default is 4.
    @return  The same handle, for chaining.

    @code
      device.begin(server).withClients(2);
    @endcode
  */
  BlaeckBeginRef &withClients(byte count);

  /*!
    @brief   Sets a stream where the library reports what it rejected and why.

    Without one, problems such as a rejected name show only in hasRejections().

    @param   debugStream  Where to print: a serial port, or anything else that can print,
                          such as a display.
    @return  The same handle, for chaining.

    @code
      device.begin(Serial).withDebugStream(&Serial);
    @endcode
  */
  BlaeckBeginRef &withDebugStream(Print *debugStream);

private:
  Blaeck *_owner;
};

// The records behind each table. Declared here, before Blaeck, because the handle
// classes need them, and in a namespace so they can't clash with names in a sketch.
namespace blaeck_detail
{
// Longest command name, terminator included. Every command entry holds an array this long.
#if defined(__AVR__)
static const byte MAX_COMMAND_NAME_COUNT = 24;
#else
static const byte MAX_COMMAND_NAME_COUNT = 40;
#endif

// EventTypeEntry::field value meaning the whole string rather than one field of it.
static const byte WHOLE_STRING = 0xFF;

// True for F(""). Modifiers treat an empty string as not set. Read with pgm_read_byte,
// since on AVR the pointer is a flash address.
inline bool flashStrEmpty(BlaeckString value)
{
  return value != nullptr && value.read() == 0;
}

// Checks the options of a select or an enum sensor: at least one entry and no blank ones.
// Prints why on debug when it refuses. `name` is the property named in that message.
bool optionsAccepted(BlaeckString optionsCsv, Print *debug,
                     const char *name, bool nameInFlash);

// A plain command from onCommand() or a button from addButton(). Both are found by name, so
// they share one table; a button has press set and no handler.
struct CommandHandlerEntry
{
  char command[MAX_COMMAND_NAME_COUNT];
  BlaeckCommandHandler handler = nullptr;
  BlaeckButtonFunction press = nullptr;
  bool inUse = false;
  // The device from addDevice() the command belongs to, 0 for the board itself.
  uint8_t deviceId = 0;
  // Buttons only: how a host shows it.
  detail::StoredString deviceClass;
  detail::StoredString icon;
  detail::StoredString displayName;
  uint8_t category = BLAECK_CAT_NONE;
  bool disabledByDefault = false;
};

// A property's presentation, kept apart so that a property without any costs one pointer.
struct PropertyPresentation
{
  detail::StoredString unit;
  detail::StoredString displayName;
  detail::StoredString icon;
  detail::StoredString deviceClass;
  uint8_t displayPrecision = 0;
};

// Bits of a property's flag word in the entity list. The access (bits 0-1) and the has-bits of
// the presentation texts are filled in when the list is written; the rest are kept here.
static const uint32_t PROPERTY_HAS_RANGE = 1UL << 2;
static const uint32_t PROPERTY_HAS_STEP = 1UL << 3;
static const uint32_t PROPERTY_HAS_UNIT = 1UL << 4;
static const uint32_t PROPERTY_HAS_DISPLAY_NAME = 1UL << 5;
static const uint32_t PROPERTY_HAS_ICON = 1UL << 6;
static const uint32_t PROPERTY_HAS_DEVICE_CLASS = 1UL << 7;
static const uint8_t PROPERTY_STATE_CLASS_SHIFT = 8;
static const uint32_t PROPERTY_STATE_CLASS_MASK = 7UL << 8;
static const uint32_t PROPERTY_HAS_DISPLAY_PRECISION = 1UL << 11;
static const uint8_t PROPERTY_CATEGORY_SHIFT = 12;
static const uint32_t PROPERTY_CATEGORY_MASK = 3UL << 12;
static const uint32_t PROPERTY_DISABLED_BY_DEFAULT = 1UL << 14;
static const uint32_t PROPERTY_FORCE_UPDATE = 1UL << 15;
static const uint8_t PROPERTY_MODE_SHIFT = 16;
static const uint32_t PROPERTY_MODE_MASK = 3UL << 16;

// A property getter's C++ return type. A getter is stored as void (*)() and called as this.
enum : uint8_t
{
  GETTER_NONE = 0,
  GETTER_BOOL, GETTER_BYTE, GETTER_SHORT, GETTER_USHORT, GETTER_INT, GETTER_UINT,
  GETTER_LONG, GETTER_ULONG, GETTER_FLOAT, GETTER_DOUBLE, GETTER_TEXT, GETTER_LONGLONG
};

// A range bound, held as the variable's own kind of number. A whole-number variable keeps whole
// bounds exactly, however few digits the board's double has: on a board where a double is a
// float, only 24 bits of one are exact, so a bound past 16,777,216 on a long variable would be
// kept as a neighbour of the one the sketch wrote. Which member is the live one is the
// variable's own type, so the union needs no tag of its own.
union RangeBound
{
  double asDouble;
  long long asInteger;
  // Zeroes the wider member, which is every bit of the other one too.
  RangeBound() : asInteger(0) {}
};

// Whether an argument to withRange() is a whole number as written, which is what decides whether
// a range is kept as whole numbers or as doubles, and whether it has a sign. Spelled out here
// rather than taken from <type_traits>, which an AVR build has no complete copy of.
template <typename T>
struct WholeArg
{
  static const bool value = false;
  static const bool isSigned = true;
};

#define BLAECK_WHOLE_ARG(T, SIGNED)      \
  template <>                            \
  struct WholeArg<T>                     \
  {                                      \
    static const bool value = true;      \
    static const bool isSigned = SIGNED; \
  }
BLAECK_WHOLE_ARG(char, true);
BLAECK_WHOLE_ARG(signed char, true);
BLAECK_WHOLE_ARG(short, true);
BLAECK_WHOLE_ARG(int, true);
BLAECK_WHOLE_ARG(long, true);
BLAECK_WHOLE_ARG(long long, true);
BLAECK_WHOLE_ARG(unsigned char, false);
BLAECK_WHOLE_ARG(unsigned short, false);
BLAECK_WHOLE_ARG(unsigned int, false);
BLAECK_WHOLE_ARG(unsigned long, false);
BLAECK_WHOLE_ARG(unsigned long long, false);
#undef BLAECK_WHOLE_ARG

// A whole argument as the long long a whole range is kept in. An unsigned one above what a long
// long holds stops there, which is the widest bound any variable has anyway: no variable holds
// more, so nothing is admitted that would not have been.
template <typename T>
long long wholeArgValue(T v)
{
  return WholeArg<T>::isSigned || (unsigned long long)v <= 9223372036854775807ULL
             ? (long long)v
             : 9223372036854775807LL;
}

// Picks which of the two range functions a withRange() call reaches. A tag, rather than a
// second overload of withRange() itself, because an int converts to a double and to a long long
// alike, so a bare withRange(0, 10, 1) would name neither.
template <bool AllWhole>
struct RangeArgs
{
};

// One input or sensor.
struct PropertyEntry
{
  detail::StoredString name;
  // Where the value lives: a variable or text buffer, or a getter, stored as void (*)() and
  // called as the function type the value's type says.
  void *address = nullptr;
  void (*getter)() = nullptr;
  BlaeckPropertyCallback callback = nullptr;
  // An enum's options, comma-separated.
  detail::StoredString options;
  // In the variable's own type, at its width, as the catalog sends them. Widening a range to a
  // fixed eight bytes would keep the value and lose the decimal it was written as: on a board
  // whose double is a float, a step of 0.01 would reach a host as 0.009999999776482582.
  // A whole-number variable's bounds live in the union's long long, where a board whose double
  // holds 24 bits does not round them; type says which member to read.
  RangeBound rangeMin;
  RangeBound rangeMax;
  RangeBound rangeStep;
  // The baseline a change is measured from. Allocated when the property is added.
  ReportingState *reporting = nullptr;
  PropertyPresentation *presentation = nullptr;
  uint32_t flags = 0;
  // A text property's buffer size, terminator included.
  uint16_t textSize = 0;
  dataType type = Blaeck_float;
  uint8_t kind = BLAECK_VALUE_NUMBER;
  uint8_t deviceId = 0;
  // The getter's C++ return type, one of the GETTER_ codes, so it is called as declared.
  uint8_t getterType = 0;
  bool writable = false;

  PropertyEntry() {}
  PropertyEntry(const PropertyEntry &) = delete;
  PropertyEntry &operator=(const PropertyEntry &) = delete;
  ~PropertyEntry()
  {
    delete reporting;
    delete presentation;
  }
};

struct EventChannelEntry
{
  // A heap copy, or a flash pointer when nameInFlash. Read it through the helpers only.
  const char *name = nullptr;
  bool nameInFlash = false;
  detail::StoredString displayName;
  detail::StoredString icon;
  detail::StoredString deviceClass;
  // 0 none, 1 config, 2 diagnostic: the entity category, as on buttons and properties.
  uint8_t category = 0;
  bool disabledByDefault = false;
  bool inUse = false;
  // The device from addDevice() the channel belongs to, 0 for the board itself.
  uint8_t deviceId = 0;
};

// A device added with addDevice(). Its DeviceID in the protocol is its index plus one.
struct DeviceEntry
{
  detail::StoredString name;
  detail::StoredString hwVersion;
  detail::StoredString fwVersion;
  bool missing = false;
  // What a host was last told, in C1 or B7; a difference is sent as C1.
  bool reportedMissing = false;
  // Set by writeRestarted(), cleared once C1 or B7 has told a host.
  bool restartPending = false;
  // A write() dropped while missing was reported on the debug stream; markPresent() resets it.
  bool dropNoted = false;
};

struct EventTypeEntry
{
  uint16_t channelIndex = 0;
  // addEventType() stores a whole string; CSV entries share one stored list.
  detail::StoredString text;
  byte field = WHOLE_STRING;
};

} // namespace blaeck_detail

// The shared part of the button handle. A rejected registration returns a handle that ignores
// every call. With BLAECK_ENABLE_IOT=0 the modifiers store nothing.
class BlaeckCommandRefBase
{
protected:
  BlaeckCommandRefBase(Blaeck *owner, int16_t index) : _owner(owner), _index(index) {}

  // The entry this handle names, or nullptr when registration was rejected.
  blaeck_detail::CommandHandlerEntry * _entry() const;

  // Marks the entity list as changed, so it is sent again.
  void _markDirty() const;

  void _setDeviceClass(BlaeckString deviceClass)
  {
#if BLAECK_ENABLE_IOT
    if (blaeck_detail::flashStrEmpty(deviceClass))
      deviceClass = nullptr;
    if (auto *e = _entry())
    {
      if (e->deviceClass != deviceClass)
      {
        if (!_storeString(e->deviceClass, deviceClass))
          return;
        _markDirty();
      }
    }
#else
    (void)deviceClass;
#endif
  }

  void _setIcon(BlaeckString icon)
  {
#if BLAECK_ENABLE_IOT
    if (blaeck_detail::flashStrEmpty(icon))
      icon = nullptr;
    if (auto *e = _entry())
    {
      if (e->icon != icon)
      {
        if (!_storeString(e->icon, icon))
          return;
        _markDirty();
      }
    }
#else
    (void)icon;
#endif
  }

  void _setDisplayName(BlaeckString displayName)
  {
#if BLAECK_ENABLE_IOT
    if (blaeck_detail::flashStrEmpty(displayName))
      displayName = nullptr;
    if (auto *e = _entry())
    {
      if (e->displayName != displayName)
      {
        if (!_storeString(e->displayName, displayName))
          return;
        _markDirty();
      }
    }
#else
    (void)displayName;
#endif
  }

  void _setCategory(uint8_t category)
  {
#if BLAECK_ENABLE_IOT
    if (auto *e = _entry())
    {
      if (e->category != category)
      {
        e->category = category;
        _markDirty();
      }
    }
#else
    (void)category;
#endif
  }

  void _setDisabledByDefault(bool on)
  {
#if BLAECK_ENABLE_IOT
    if (auto *e = _entry())
    {
      if (e->disabledByDefault != on)
      {
        e->disabledByDefault = on;
        _markDirty();
      }
    }
#else
    (void)on;
#endif
  }

  bool _storeString(detail::StoredString &slot, BlaeckString value);

  Blaeck *_owner;
  int16_t _index;
};

// Modifiers every command handle has, buttons included. TYPE is the deriving handle, so each
// call returns that type and the chain keeps its kind's methods. A template rather than a
// macro, so each method keeps its doc comment.
template <class TYPE>
class BlaeckCommandRefShared : public BlaeckCommandRefBase
{
public:
  /*!
    @brief   Sets the label a host shows instead of the command name.

    Only the label changes. When the button is pressed, the host still sends the
    command name, such as STATUS, so adding a label later breaks nothing.

    @param   displayName  The label.
    @return  The same handle, for chaining.

    @code
      device.addButton("STATUS", onStatus).withDisplayName(F("Request status"));
    @endcode
  */
  TYPE &withDisplayName(BlaeckString displayName)
  {
    _setDisplayName(displayName);
    return _self();
  }

  /*!
    @brief   Sets the icon a host shows next to the control.

    Where a device class fits, prefer it: a host picks a matching icon from it.

    @param   icon  A Material Design Icons name.
    @return  The same handle, for chaining.

    @code
      device.addButton("CALIBRATE", onCalibrate).withIcon(F("mdi:tune"));
    @endcode
  */
  TYPE &withIcon(BlaeckString icon)
  {
    _setIcon(icon);
    return _self();
  }

  /*!
    @brief   Marks the command as a device setting.

    A host usually keeps settings off its default dashboard.

    @return  The same handle, for chaining.

    @code
      device.addButton("FACTORY_RESET", onFactoryReset).config();
    @endcode
  */
  TYPE &config()
  {
    _setCategory((uint8_t)BLAECK_CAT_CONFIG);
    return _self();
  }

  /*!
    @brief   Marks the command as a diagnostic control, such as reboot or identify.

    A host usually keeps these off its default dashboard.

    @return  The same handle, for chaining.

    @code
      device.addButton("REBOOT", onReboot).diagnostic();
    @endcode
  */
  TYPE &diagnostic()
  {
    _setCategory((uint8_t)BLAECK_CAT_DIAGNOSTIC);
    return _self();
  }

  /*!
    @brief   Asks a host to create the control disabled, until someone enables it.

    The command itself still works when sent.

    @param   on  false undoes it.
    @return  The same handle, for chaining.

    @code
      device.addButton("CALIBRATE", onCalibrate).disabledByDefault();
    @endcode
  */
  TYPE &disabledByDefault(bool on = true)
  {
    _setDisabledByDefault(on);
    return _self();
  }

protected:
  BlaeckCommandRefShared(Blaeck *owner, int16_t index) : BlaeckCommandRefBase(owner, index) {}

  TYPE &_self() { return *static_cast<TYPE *>(this); }
};

// The handle addButton() returns.
class BlaeckButtonRef : public BlaeckCommandRefShared<BlaeckButtonRef>
{
public:
  BlaeckButtonRef(Blaeck *owner, int16_t index) : BlaeckCommandRefShared<BlaeckButtonRef>(owner, index) {}

  /*!
    @brief   Says what pressing the button does: restart, identify or update.

    Changes only the icon and wording a host uses.

    @param   deviceClass  "restart", "identify" or "update". No other values are
                          valid for a button.
    @return  The same handle, for chaining.

    @note    These buttons usually belong under diagnostic() as well.

    @code
      device.addButton("REBOOT", onReboot).withDeviceClass(F("restart")).diagnostic();
    @endcode
  */
  BlaeckButtonRef &withDeviceClass(BlaeckString deviceClass)
  {
    _setDeviceClass(deviceClass);
    return *this;
  }
};

// The handle addSignal() returns, for how the signal is reported. A rejected registration
// returns a handle that ignores every call.
class BlaeckSignalRef
{
public:
  /*!
    @brief   Creates an empty handle, to keep a signal's handle in a global.

    Assign what addSignal() returns, and the sketch can change how the signal is
    reported later. Until then, calls on it do nothing.

    @code
      BlaeckSignalRef outputSignal;  // file scope

      void setup() { outputSignal = device.addSignal(F("Output"), &Output); }
      void loop()  { outputSignal.writeOnChange(0.5); }
    @endcode
  */
  BlaeckSignalRef() : _owner(nullptr), _index(-1) {}

  /*!
    @brief   Selects how this signal participates in host-interval reports.

    BLAECK_ALWAYS is the default. BLAECK_ON_CHANGE compares against the last sent
    value when the interval is due. BLAECK_OFF excludes this signal from interval
    reports. This replaces the interval policy, independently of writeOnChange().
    Every INTERVAL_START makes an initial interval report due: all interval-enabled
    signals are included, without change filtering, even when already active.
    Explicit writes bypass both policies and update their shared baseline.

    @param   mode   BLAECK_ALWAYS, BLAECK_ON_CHANGE or BLAECK_OFF.
    @param   delta  Nonnegative finite numeric threshold; BLAECK_ANY_CHANGE (zero)
                    means any difference.
                    Ignored for boolean and text signals.
    @return  The same handle, for chaining.

    @code
      device.addSignal(F("Temperature"), &Temperature)
          .writeAtInterval(BLAECK_ON_CHANGE, 0.1);
    @endcode
  */
  BlaeckSignalRef &writeAtInterval(BlaeckIntervalMode mode, double delta = BLAECK_ANY_CHANGE)
  {
    _setInterval(mode, delta);
    return *this;
  }

  /*!
    @brief   Enables prompt reporting of changes, independently of host activation.

    Checked by tick(). Interval reporting remains separately
    configured by writeAtInterval(), and defaults to BLAECK_ALWAYS. The first
    value bypasses the rate limit. Later changes compare against the last value
    sent by any data write. Intermediate values are not queued.
    Calling this again replaces the threshold and rate limit, or re-enables
    change reporting after writeOnChange(BLAECK_OFF).

    @param   delta          Nonnegative finite numeric threshold; BLAECK_ANY_CHANGE
                            (zero) means any difference. Ignored for boolean and
                            text signals, where callers use BLAECK_ANY_CHANGE.
    @param   minIntervalMs  Minimum time since the last data write for this signal,
                            in milliseconds. Defaults to 100; zero removes the limit.
    @return  The same handle, for chaining.

    @code
      device.addSignal(F("Pulse"), &Pulse)
          .writeAtInterval(BLAECK_OFF).writeOnChange(BLAECK_ANY_CHANGE);
    @endcode
  */
  BlaeckSignalRef &writeOnChange(double delta, uint32_t minIntervalMs = 100)
  {
    _setOnChange(delta, minIntervalMs);
    return *this;
  }

  /*!
    @brief   Disables prompt change reporting with BLAECK_OFF.

    Leaves interval reporting and explicit writes unchanged. Frees change-tracking
    storage unless interval filtering still needs it. Re-enable with a numeric
    threshold or BLAECK_ANY_CHANGE; an existing shared baseline and rate-limit clock
    are retained, otherwise the next eligible report sends an initial value.

    @param   mode  BLAECK_OFF. Other modes are rejected with a policy warning,
                    leaving the previous policy intact.
    @return  The same handle, for chaining.

    @code
      auto signal = device.addSignal(F("Temperature"), &Temperature);
      signal.writeOnChange(0.1);
      signal.writeOnChange(BLAECK_OFF);
    @endcode
  */
  BlaeckSignalRef &writeOnChange(BlaeckIntervalMode mode)
  {
    _setOnChange(mode);
    return *this;
  }

  // A mode with a rate limit must not fall through to the numeric-threshold overload.
  BlaeckSignalRef &writeOnChange(BlaeckIntervalMode mode, uint32_t minIntervalMs) = delete;

  /*!
    @brief   Adds a number to the end of the signal's name.

    For a series of signals sharing a prefix: Sine_ with suffix 3 is Sine_3. Unlike
    a name built with snprintf, this keeps nothing on the heap.

    @param   suffix  0 to 255.
    @return  The same handle, for chaining.

    @code
      for (int i = 0; i < 8; i++)
        device.addSignal(F("Sine_"), &sine[i]).withNameSuffix(i + 1);
    @endcode
  */
  BlaeckSignalRef &withNameSuffix(uint8_t suffix)
  {
    _setNameSuffix(suffix);
    return *this;
  }

private:
  // Private, so only addSignal() can make a handle that names a signal.
  BlaeckSignalRef(Blaeck *owner, int16_t index) : _owner(owner), _index(index) {}
  friend class Blaeck;
  friend class BlaeckDeviceBase;

  void _setNameSuffix(uint8_t suffix);
  void _setInterval(BlaeckIntervalMode mode, double delta);
  void _setOnChange(double delta, uint32_t minIntervalMs);
  void _setOnChange(BlaeckIntervalMode mode);
  Blaeck *_owner;
  int16_t _index;
};

class BlaeckEventRef
{
public:
  BlaeckEventRef(Blaeck *owner, int16_t index) : _owner(owner), _index(index) {}

  /*!
    @brief   Sets the icon a host shows next to the channel.

    @param   icon  A Material Design Icons name.
    @return  The same handle, for chaining.

    @code
      device.addEvent(F("Activity"), F("idle,resumed")).withIcon(F("mdi:pulse"));
    @endcode
  */
  BlaeckEventRef withIcon(BlaeckString icon);

  /*!
    @brief   Sets the label a host shows instead of the name.

    The name stays what the event is known by, so the label can change without a host seeing
    a new event.

    @param   displayName  Any text.
    @return  The same handle, for chaining.

    @code
      device.addEvent(F("Doorbell"), F("ring")).withDisplayName(F("Front door"));
    @endcode
  */
  BlaeckEventRef withDisplayName(BlaeckString displayName);

  /*!
    @brief   Files the event under the device's configuration.

    @param   on  false undoes it.
    @return  The same handle, for chaining.

    @note    Home Assistant files events under diagnostics or nowhere, so it shows a config
             event as an ordinary one.

    @code
      device.addEvent(F("Calibrated"), F("done")).config();
    @endcode
  */
  BlaeckEventRef config(bool on = true);

  /*!
    @brief   Files the event under the device's diagnostics.

    @param   on  false undoes it.
    @return  The same handle, for chaining.

    @code
      device.addEvent(F("Faults"), F("brownout,watchdog")).diagnostic();
    @endcode
  */
  BlaeckEventRef diagnostic(bool on = true);

  /*!
    @brief   Sets what kind of events the channel reports: button, doorbell or motion.

    @param   deviceClass  F("button"), F("doorbell") or F("motion"). Anything else can
                          make a host drop the channel.
    @return  The same handle, for chaining.

    @note    A doorbell channel should have a "ring" event type. A button channel may
             use the standard names press_start, press_end, long_press_start,
             long_press_end, multi_press_ongoing and multi_press_end, but needn't.

    @code
      device.addEvent(F("Doorbell"), F("ring")).withDeviceClass(F("doorbell"));
    @endcode
  */
  BlaeckEventRef withDeviceClass(BlaeckString deviceClass);

  /*!
    @brief   Asks a host to create the channel disabled, until someone enables it.

    The device sends events either way. Events aren't kept, so enabling the channel
    later doesn't show earlier ones.

    @param   on  false undoes it.
    @return  The same handle, for chaining.

    @code
      device.addEvent(F("Debug"), F("trace")).disabledByDefault();
    @endcode
  */
  BlaeckEventRef disabledByDefault(bool on = true);

private:
  void _setCategory(uint8_t category, bool on);
  Blaeck *_owner;
  int16_t _index;
};

// What the board and each device from addDevice() declare and report through: signals,
// properties, commands and events, and the writes that find them by name. Blaeck
// inherits it for the board, BlaeckDeviceRef for a device. Names are looked up within the
// device the call is made on, so `pump.write("Flow", v)` finds only the pump's signal.
// The handles addNumberInput(), addTextInput(), addSwitch(), addSelect() and addSensor()
// return. Each exposes the modifiers that fit its kind of value. A rejected property returns a
// handle that ignores every call.
class BlaeckPropertyRefBase
{
protected:
  BlaeckPropertyRefBase(Blaeck *owner, int16_t index) : _owner(owner), _index(index) {}

  // The entry this handle names, or nullptr when registration was rejected. Defined out of
  // line, like the helpers below, because Blaeck is incomplete here.
  blaeck_detail::PropertyEntry *_entry() const;
  // The entry's presentation, allocated on first use. nullptr without an entry or RAM.
  blaeck_detail::PropertyPresentation *_presentation() const;
  // Sets the bits in mask to value, marking the entity list changed if they differ.
  void _setFlags(uint32_t mask, uint32_t value) const;
  void _setText(detail::StoredString blaeck_detail::PropertyPresentation::*field, BlaeckString text) const;
  void _setRange(double mn, double mx, double st) const;
  void _setRange(long long mn, long long mx, long long st) const;
  // The two spellings of a range, chosen by the arguments' own types, so bare whole numbers,
  // decimals and a mix of the two each reach the one that keeps them.
  template <typename A, typename B, typename C>
  void _setRangeArgs(A mn, B mx, C st, blaeck_detail::RangeArgs<true>) const
  {
    _setRange(blaeck_detail::wholeArgValue(mn), blaeck_detail::wholeArgValue(mx),
              blaeck_detail::wholeArgValue(st));
  }
  template <typename A, typename B, typename C>
  void _setRangeArgs(A mn, B mx, C st, blaeck_detail::RangeArgs<false>) const
  {
    _setRange((double)mn, (double)mx, (double)st);
  }
  void _setDisplayPrecision(uint8_t decimals) const;
  void _setReporting(double delta, uint32_t minIntervalMs) const;

  Blaeck *_owner;
  int16_t _index;
};

// Modifiers every property handle has. TYPE is the deriving handle, so each call returns that
// type and the chain keeps its kind's methods.
template <class TYPE>
class BlaeckPropertyRefShared : public BlaeckPropertyRefBase
{
public:
  /*!
    @brief   Sets the label a host shows instead of the name.

    Only the label changes. A host still sets the value by its name, so adding a label
    later breaks nothing.

    @param   displayName  The label. RAM text is copied; an F() literal stays in flash.
    @return  The same handle, for chaining.

    @code
      device.addSwitch(F("OutputEnabled"), &enabled).withDisplayName(F("Output enabled"));
    @endcode
  */
  TYPE &withDisplayName(BlaeckString displayName)
  {
    _setText(&blaeck_detail::PropertyPresentation::displayName, displayName);
    return _self();
  }

  /*!
    @brief   Sets the icon a host shows next to the value.

    @param   icon  A Material Design Icons name, such as "mdi:tag".
    @return  The same handle, for chaining.

    @code
      device.addTextInput(F("Label"), label, sizeof(label)).withIcon(F("mdi:tag"));
    @endcode
  */
  TYPE &withIcon(BlaeckString icon)
  {
    _setText(&blaeck_detail::PropertyPresentation::icon, icon);
    return _self();
  }

  /*!
    @brief   Says what the value is, in a host's vocabulary.

    A host picks its icon and wording from it, such as "temperature" or "door". A name
    the host doesn't know costs that one entry, so declare nothing rather than guess.

    @param   deviceClass  The class name.
    @return  The same handle, for chaining.

    @code
      device.addSensor(F("DoorOpen"), &doorOpen).withDeviceClass(F("door"));
    @endcode
  */
  TYPE &withDeviceClass(BlaeckString deviceClass)
  {
    _setText(&blaeck_detail::PropertyPresentation::deviceClass, deviceClass);
    return _self();
  }

  /*!
    @brief   Files the property with the device's settings rather than its main values.

    @return  The same handle, for chaining.

    @code
      device.addTextInput(F("Label"), label, sizeof(label)).config();
    @endcode
  */
  TYPE &config()
  {
    _setFlags(blaeck_detail::PROPERTY_CATEGORY_MASK,
              (uint32_t)BLAECK_CAT_CONFIG << blaeck_detail::PROPERTY_CATEGORY_SHIFT);
    return _self();
  }

  /*!
    @brief   Files the property as information about the device rather than what it does.

    @return  The same handle, for chaining.

    @code
      device.addSensor(F("Uptime"), &uptime).diagnostic();
    @endcode
  */
  TYPE &diagnostic()
  {
    _setFlags(blaeck_detail::PROPERTY_CATEGORY_MASK,
              (uint32_t)BLAECK_CAT_DIAGNOSTIC << blaeck_detail::PROPERTY_CATEGORY_SHIFT);
    return _self();
  }

  /*!
    @brief   Registers the property with a host switched off, until someone enables it.

    @param   on  False to undo it.
    @return  The same handle, for chaining.

    @code
      device.addSensor(F("BuildNumber"), &buildNumber).disabledByDefault();
    @endcode
  */
  TYPE &disabledByDefault(bool on = true)
  {
    _setFlags(blaeck_detail::PROPERTY_DISABLED_BY_DEFAULT,
              on ? blaeck_detail::PROPERTY_DISABLED_BY_DEFAULT : 0);
    return _self();
  }

  /*!
    @brief   Asks a host to record every value it receives, even one equal to the last.

    @param   on  False to undo it.
    @return  The same handle, for chaining.

    @code
      device.addSensor(F("Heartbeat"), &beat).forceUpdate();
    @endcode
  */
  TYPE &forceUpdate(bool on = true)
  {
    _setFlags(blaeck_detail::PROPERTY_FORCE_UPDATE, on ? blaeck_detail::PROPERTY_FORCE_UPDATE : 0);
    return _self();
  }

  /*!
    @brief   Sets when a change is sent: by how much the value must change, and how often.

    A property is always sent when it changes, checked on every tick(). Without this call,
    any change counts and it is sent at most every 100 ms. A host's write is sent at once,
    and so is writeProperty().

    @param   delta          How much a number must change; 0 or BLAECK_ANY_CHANGE for any.
                            Ignored for bool, enum and text.
    @param   minIntervalMs  At least this long between two sends; 0 for no limit.
    @return  The same handle, for chaining.

    @code
      device.addSensor(F("Temperature"), &temperature).writeOnChange(0.1, 1000);
    @endcode
  */
  TYPE &writeOnChange(double delta, uint32_t minIntervalMs = 100)
  {
    _setReporting(delta, minIntervalMs);
    return _self();
  }

protected:
  BlaeckPropertyRefShared(Blaeck *owner, int16_t index) : BlaeckPropertyRefBase(owner, index) {}

private:
  TYPE &_self() { return static_cast<TYPE &>(*this); }
};

// The handle for a switch, a select, and a sensor of a bool or an enum.
class BlaeckPropertyRef : public BlaeckPropertyRefShared<BlaeckPropertyRef>
{
public:
  /*!
    @brief   Creates an empty handle, to keep a property's handle in a global.

    Assign what addSwitch(), addSelect() or addSensor() returns. Until then, calls on it
    do nothing.
  */
  BlaeckPropertyRef() : BlaeckPropertyRefShared<BlaeckPropertyRef>(nullptr, -1) {}
  BlaeckPropertyRef(Blaeck *owner, int16_t index) : BlaeckPropertyRefShared<BlaeckPropertyRef>(owner, index) {}
};

// The handle for a number input and a number sensor.
class BlaeckNumberPropertyRef : public BlaeckPropertyRefShared<BlaeckNumberPropertyRef>
{
public:
  /*!
    @brief   Creates an empty handle, to keep a property's handle in a global.

    Assign what addNumberInput() or addSensor() returns. Until then, calls on it do
    nothing.

    @code
      BlaeckNumberPropertyRef outputSensor;
      outputSensor = device.addSensor(F("Output"), &output);
    @endcode
  */
  BlaeckNumberPropertyRef() : BlaeckPropertyRefShared<BlaeckNumberPropertyRef>(nullptr, -1) {}
  BlaeckNumberPropertyRef(Blaeck *owner, int16_t index)
      : BlaeckPropertyRefShared<BlaeckNumberPropertyRef>(owner, index) {}

  /*!
    @brief   Sets the values an input accepts, and the step it is stored on.

    A value outside [min, max] is refused. With a step, a value within a thousandth of a
    step of min + n * step is stored as exactly that, so 0.9 arriving as 0.90000004
    stays 0.9; a value further off is kept as sent.

    A bound or a step the variable cannot state exactly is dropped, and says so on debug:
    a range reaching past the variable would admit a write that cannot be stored, and a
    fraction on a whole-number variable would arrive truncated - 0.5 as 0. So an integer
    input takes whole bounds and a whole step. The catalog carries min, max and step in
    the variable's own type, at its width.

    Written as whole numbers, a range is kept as whole numbers, exactly, however few digits
    the board's double has: withRange(0, 4000000000, 1) on an unsigned long says what it
    means on an AVR, where a double holds 24 bits. Written with a decimal anywhere, all
    three are kept as doubles.

    @param   min   Lowest accepted value; must be within what the variable holds.
    @param   max   Highest accepted value; must be above min and within what the variable holds.
    @param   step  The step a host offers and the value is stored on; 0 for none.
    @return  The same handle, for chaining.

    @code
      device.addNumberInput(F("Setpoint"), &setpoint).withRange(5.0f, 30.0f, 0.5f);
      device.addNumberInput(F("Build"), &buildNumber).withRange(0, 4000000000, 1);
    @endcode
  */
  template <typename A, typename B, typename C>
  BlaeckNumberPropertyRef &withRange(A min, B max, C step)
  {
    _setRangeArgs(min, max, step,
                  blaeck_detail::RangeArgs<blaeck_detail::WholeArg<A>::value &&
                                           blaeck_detail::WholeArg<B>::value &&
                                           blaeck_detail::WholeArg<C>::value>());
    return *this;
  }

  /*!
    @brief   Sets the values an input accepts, leaving it on no step.

    As withRange(A, B, C) with a step of 0: a host offers the whole range, and a value
    inside it is stored as it arrived.

    @param   min   Lowest accepted value; must be within what the variable holds.
    @param   max   Highest accepted value; must be above min and within what the variable holds.
    @return  The same handle, for chaining.

    @code
      device.addNumberInput(F("Offset"), &offset).withRange(-10.0f, 10.0f);
    @endcode
  */
  template <typename A, typename B>
  BlaeckNumberPropertyRef &withRange(A min, B max)
  {
    return withRange(min, max, 0);
  }

  /*!
    @brief   Sets the unit a host shows after the value.

    @param   unit  The unit, such as "\xC2\xB0" "C" for degrees Celsius.
    @return  The same handle, for chaining.

    @code
      device.addSensor(F("Temperature"), &temperature).withUnit(F("\xC2\xB0" "C"));
    @endcode
  */
  BlaeckNumberPropertyRef &withUnit(BlaeckString unit)
  {
    _setText(&blaeck_detail::PropertyPresentation::unit, unit);
    return *this;
  }

  /*!
    @brief   Says how a host should treat the values over time.

    @param   stateClass  BLAECK_STATE_CLASS_MEASUREMENT for a reading, or one of the
                         total classes for a meter.
    @return  The same handle, for chaining.

    @code
      device.addSensor(F("Energy"), &energy).withStateClass(BLAECK_STATE_CLASS_TOTAL_INCREASING);
    @endcode
  */
  BlaeckNumberPropertyRef &withStateClass(BlaeckStateClass stateClass)
  {
    _setFlags(blaeck_detail::PROPERTY_STATE_CLASS_MASK,
              ((uint32_t)stateClass << blaeck_detail::PROPERTY_STATE_CLASS_SHIFT) &
                  blaeck_detail::PROPERTY_STATE_CLASS_MASK);
    return *this;
  }

  /*!
    @brief   Sets how many decimal places a host shows.

    @param   decimals  Places after the point; 0 shows an integer.
    @return  The same handle, for chaining.

    @code
      device.addSensor(F("Temperature"), &temperature).withDisplayPrecision(1);
    @endcode
  */
  BlaeckNumberPropertyRef &withDisplayPrecision(uint8_t decimals)
  {
    _setDisplayPrecision(decimals);
    return *this;
  }

  /*!
    @brief   Sets how a host shows an input: a typed box or a slider.

    @param   mode  BLAECK_NUMBER_MODE_BOX, BLAECK_NUMBER_MODE_SLIDER or, the default,
                   BLAECK_NUMBER_MODE_AUTO.
    @return  The same handle, for chaining.

    @code
      device.addNumberInput(F("Amplitude"), &amplitude)
          .withRange(0.0f, 100.0f, 1.0f).withMode(BLAECK_NUMBER_MODE_SLIDER);
    @endcode
  */
  BlaeckNumberPropertyRef &withMode(BlaeckNumberMode mode)
  {
    _setFlags(blaeck_detail::PROPERTY_MODE_MASK,
              ((uint32_t)mode << blaeck_detail::PROPERTY_MODE_SHIFT) & blaeck_detail::PROPERTY_MODE_MASK);
    return *this;
  }
};

// The handle for a text input and a text sensor.
class BlaeckTextPropertyRef : public BlaeckPropertyRefShared<BlaeckTextPropertyRef>
{
public:
  /*!
    @brief   Creates an empty handle, to keep a property's handle in a global.

    Assign what addTextInput() or addSensor() returns. Until then, calls on it do
    nothing.
  */
  BlaeckTextPropertyRef() : BlaeckPropertyRefShared<BlaeckTextPropertyRef>(nullptr, -1) {}
  BlaeckTextPropertyRef(Blaeck *owner, int16_t index)
      : BlaeckPropertyRefShared<BlaeckTextPropertyRef>(owner, index) {}

  /*!
    @brief   Asks a host to mask an input while it is typed.

    Only the field on screen is masked; the value still travels as plain text.

    @param   mode  BLAECK_TEXT_MODE_PASSWORD, or BLAECK_TEXT_MODE_PLAIN, the default.
    @return  The same handle, for chaining.

    @code
      device.addTextInput(F("Token"), token, sizeof(token)).withMode(BLAECK_TEXT_MODE_PASSWORD);
    @endcode
  */
  BlaeckTextPropertyRef &withMode(BlaeckTextMode mode)
  {
    _setFlags(blaeck_detail::PROPERTY_MODE_MASK,
              ((uint32_t)mode << blaeck_detail::PROPERTY_MODE_SHIFT) & blaeck_detail::PROPERTY_MODE_MASK);
    return *this;
  }
};

class BlaeckDeviceBase
{
public:
  // ----- Signals -----

  /*!
    @brief   Adds a variable to be sampled and logged over time.

    A signal is a reading that is sent on every interval and kept as history: a name
    and a type, nothing else. To show the value on a dashboard, add a sensor on the
    same variable with addSensor(); for a setting a host changes, use an input.

    The variable is read each time data is sent, so it must be a global.

    @param   signalName  The name a host logs the signal under, its column name. RAM text is
                         copied; an F() literal stays in flash.
    @param   value       The variable. There is an overload for each type. Text is
                         pointed at, not copied; an F() text stays in flash, and later
                         write() calls may replace it with RAM or flash text.
    @return  A handle for how the signal is reported. It can be ignored, or kept in a
             global to change that later.
    @note    If there is no RAM for it, the signal is dropped and the handle ignores
             every call; hasRejections() reports it.

    @code
      device.addSignal(F("Temperature [C]"), &Temperature);
      device.addSensor(F("Temperature"), &Temperature).withUnit(F("\xC2\xB0" "C"));
    @endcode
  */
  BlaeckSignalRef addSignal(BlaeckString signalName, bool *value);
  BlaeckSignalRef addSignal(BlaeckString signalName, byte *value);
  BlaeckSignalRef addSignal(BlaeckString signalName, short *value);
  BlaeckSignalRef addSignal(BlaeckString signalName, unsigned short *value);
  BlaeckSignalRef addSignal(BlaeckString signalName, int *value);
  BlaeckSignalRef addSignal(BlaeckString signalName, unsigned int *value);
  BlaeckSignalRef addSignal(BlaeckString signalName, long *value);
  BlaeckSignalRef addSignal(BlaeckString signalName, unsigned long *value);
  BlaeckSignalRef addSignal(BlaeckString signalName, long long *value);
  BlaeckSignalRef addSignal(BlaeckString signalName, float *value);
  BlaeckSignalRef addSignal(BlaeckString signalName, double *value);
  BlaeckSignalRef addSignal(BlaeckString signalName, const char *value);
  BlaeckSignalRef addSignal(BlaeckString signalName, const __FlashStringHelper *value);

  // ----- Properties -----
  // A property is a current value a host shows (a sensor) or shows and sets (an input). It is
  // not logged: for history, add a signal on the same variable. A property is sent when it
  // changes, checked on every tick(); see writeOnChange() on the handle.

  /*!
    @brief   Adds a number a host can set, stored in a variable.

    A value from a host is checked before it is stored: it must be a number, within
    withRange() if one is set, and without a fraction for an integer variable.

    @param   name      The name a host shows and sets the value by. RAM text is copied;
                       an F() literal stays in flash. Unique on the board among inputs,
                       sensors, buttons and commands.
    @param   value     The variable, a global. There is an overload for each number type.
    @param   onChange  Optional. Runs after a host has set the value.
    @return  A handle for the range, unit and presentation.
    @note    A rejected name or a board out of RAM drops the property, and the handle
             ignores every call; hasRejections() reports it.

    @code
      device.addNumberInput(F("Setpoint"), &setpoint)
          .withRange(5.0f, 30.0f, 0.5f)
          .withUnit(F("\xC2\xB0" "C"));
    @endcode
  */
  BlaeckNumberPropertyRef addNumberInput(BlaeckString name, byte *value, BlaeckPropertyCallback onChange = nullptr);
  BlaeckNumberPropertyRef addNumberInput(BlaeckString name, short *value, BlaeckPropertyCallback onChange = nullptr);
  BlaeckNumberPropertyRef addNumberInput(BlaeckString name, unsigned short *value, BlaeckPropertyCallback onChange = nullptr);
  BlaeckNumberPropertyRef addNumberInput(BlaeckString name, int *value, BlaeckPropertyCallback onChange = nullptr);
  BlaeckNumberPropertyRef addNumberInput(BlaeckString name, unsigned int *value, BlaeckPropertyCallback onChange = nullptr);
  BlaeckNumberPropertyRef addNumberInput(BlaeckString name, long *value, BlaeckPropertyCallback onChange = nullptr);
  BlaeckNumberPropertyRef addNumberInput(BlaeckString name, unsigned long *value, BlaeckPropertyCallback onChange = nullptr);
  BlaeckNumberPropertyRef addNumberInput(BlaeckString name, long long *value, BlaeckPropertyCallback onChange = nullptr);
  BlaeckNumberPropertyRef addNumberInput(BlaeckString name, float *value, BlaeckPropertyCallback onChange = nullptr);
  BlaeckNumberPropertyRef addNumberInput(BlaeckString name, double *value, BlaeckPropertyCallback onChange = nullptr);

  /*!
    @brief   Adds text a host can set, stored in a buffer.

    A host's text is decoded and copied into the buffer, terminator included. Longer
    text than the buffer holds is refused; an empty value clears it.

    @param   name      The name a host shows and sets the value by.
    @param   buffer    The buffer, a global.
    @param   size      Its size, terminator included, such as sizeof(label). At most 256.
    @param   onChange  Optional. Runs after a host has set the value.
    @return  A handle for the presentation.

    @code
      char label[33] = "lab-heater";
      device.addTextInput(F("Label"), label, sizeof(label)).config();
    @endcode
  */
  BlaeckTextPropertyRef addTextInput(BlaeckString name, char *buffer, size_t size, BlaeckPropertyCallback onChange = nullptr);

  /*!
    @brief   Adds an on/off switch a host can set, stored in a bool.

    A host sends 0 or 1; anything else is refused.

    @param   name      The name a host shows and sets the value by.
    @param   value     The variable, a global.
    @param   onChange  Optional. Runs after a host has set the value.
    @return  A handle for the presentation.

    @code
      device.addSwitch(F("OutputEnabled"), &enabled);
    @endcode
  */
  BlaeckPropertyRef addSwitch(BlaeckString name, bool *value, BlaeckPropertyCallback onChange = nullptr);

  /*!
    @brief   Adds a choice from a list a host can set, stored as the option's index.

    A host sends an option's name or its index; the variable always holds the index,
    counted from 0. Anything else is refused.

    @param   name      The name a host shows and sets the value by.
    @param   index     The variable, a global of any integer type.
    @param   options   The options, comma-separated. At least one, none blank.
    @param   onChange  Optional. Runs after a host has set the value.
    @return  A handle for the presentation.

    @code
      device.addSelect(F("Mode"), &mode, F("Off,Heat,Auto"));
    @endcode
  */
  BlaeckPropertyRef addSelect(BlaeckString name, byte *index, BlaeckString options, BlaeckPropertyCallback onChange = nullptr);
  BlaeckPropertyRef addSelect(BlaeckString name, short *index, BlaeckString options, BlaeckPropertyCallback onChange = nullptr);
  BlaeckPropertyRef addSelect(BlaeckString name, unsigned short *index, BlaeckString options, BlaeckPropertyCallback onChange = nullptr);
  BlaeckPropertyRef addSelect(BlaeckString name, int *index, BlaeckString options, BlaeckPropertyCallback onChange = nullptr);
  BlaeckPropertyRef addSelect(BlaeckString name, unsigned int *index, BlaeckString options, BlaeckPropertyCallback onChange = nullptr);
  BlaeckPropertyRef addSelect(BlaeckString name, long *index, BlaeckString options, BlaeckPropertyCallback onChange = nullptr);
  BlaeckPropertyRef addSelect(BlaeckString name, unsigned long *index, BlaeckString options, BlaeckPropertyCallback onChange = nullptr);

  /*!
    @brief   Adds a value a host shows but cannot set.

    The argument decides the kind: a number variable, a bool, an integer index with its
    options, a text buffer with its size, or a function returning any of these. A function
    is called on every check, so it must return quickly and must not send anything itself.
    A host that tries to set a sensor is refused.

    @param   name   The name a host shows. Unique on the board among inputs, sensors,
                    buttons and commands.
    @param   value  The variable or function.
    @return  A handle for the presentation.

    @code
      device.addSensor(F("Temperature"), &temperature)
          .withUnit(F("\xC2\xB0" "C"))
          .writeOnChange(0.1, 1000);
    @endcode
  */
  BlaeckNumberPropertyRef addSensor(BlaeckString name, byte *value);
  BlaeckNumberPropertyRef addSensor(BlaeckString name, short *value);
  BlaeckNumberPropertyRef addSensor(BlaeckString name, unsigned short *value);
  BlaeckNumberPropertyRef addSensor(BlaeckString name, int *value);
  BlaeckNumberPropertyRef addSensor(BlaeckString name, unsigned int *value);
  BlaeckNumberPropertyRef addSensor(BlaeckString name, long *value);
  BlaeckNumberPropertyRef addSensor(BlaeckString name, unsigned long *value);
  BlaeckNumberPropertyRef addSensor(BlaeckString name, long long *value);
  BlaeckNumberPropertyRef addSensor(BlaeckString name, float *value);
  BlaeckNumberPropertyRef addSensor(BlaeckString name, double *value);
  BlaeckNumberPropertyRef addSensor(BlaeckString name, byte (*value)());
  BlaeckNumberPropertyRef addSensor(BlaeckString name, short (*value)());
  BlaeckNumberPropertyRef addSensor(BlaeckString name, unsigned short (*value)());
  BlaeckNumberPropertyRef addSensor(BlaeckString name, int (*value)());
  BlaeckNumberPropertyRef addSensor(BlaeckString name, unsigned int (*value)());
  BlaeckNumberPropertyRef addSensor(BlaeckString name, long (*value)());
  BlaeckNumberPropertyRef addSensor(BlaeckString name, unsigned long (*value)());
  BlaeckNumberPropertyRef addSensor(BlaeckString name, long long (*value)());
  BlaeckNumberPropertyRef addSensor(BlaeckString name, float (*value)());
  BlaeckNumberPropertyRef addSensor(BlaeckString name, double (*value)());

  /*!
    @brief   Adds an on/off value a host shows but cannot set.

    @param   name   The name a host shows.
    @param   value  A bool variable, or a function returning bool.
    @return  A handle for the presentation.

    @code
      device.addSensor(F("DoorOpen"), &doorOpen).withDeviceClass(F("door"));
    @endcode
  */
  BlaeckPropertyRef addSensor(BlaeckString name, bool *value);
  BlaeckPropertyRef addSensor(BlaeckString name, bool (*value)());

  /*!
    @brief   Adds one of a list of states, which a host shows by name but cannot set.

    The variable or function gives the option's index, counted from 0. Without the
    options, the same variable would be a number sensor.

    @param   name     The name a host shows.
    @param   index    An integer variable, or a function returning one.
    @param   options  The options, comma-separated. At least one, none blank.
    @return  A handle for the presentation.

    @code
      device.addSensor(F("State"), &stateIndex, F("Idle,Heating,Cooling"));
    @endcode
  */
  BlaeckPropertyRef addSensor(BlaeckString name, byte *index, BlaeckString options);
  BlaeckPropertyRef addSensor(BlaeckString name, short *index, BlaeckString options);
  BlaeckPropertyRef addSensor(BlaeckString name, unsigned short *index, BlaeckString options);
  BlaeckPropertyRef addSensor(BlaeckString name, int *index, BlaeckString options);
  BlaeckPropertyRef addSensor(BlaeckString name, unsigned int *index, BlaeckString options);
  BlaeckPropertyRef addSensor(BlaeckString name, long *index, BlaeckString options);
  BlaeckPropertyRef addSensor(BlaeckString name, unsigned long *index, BlaeckString options);
  BlaeckPropertyRef addSensor(BlaeckString name, byte (*index)(), BlaeckString options);
  BlaeckPropertyRef addSensor(BlaeckString name, short (*index)(), BlaeckString options);
  BlaeckPropertyRef addSensor(BlaeckString name, unsigned short (*index)(), BlaeckString options);
  BlaeckPropertyRef addSensor(BlaeckString name, int (*index)(), BlaeckString options);
  BlaeckPropertyRef addSensor(BlaeckString name, unsigned int (*index)(), BlaeckString options);
  BlaeckPropertyRef addSensor(BlaeckString name, long (*index)(), BlaeckString options);
  BlaeckPropertyRef addSensor(BlaeckString name, unsigned long (*index)(), BlaeckString options);

  /*!
    @brief   Adds text a host shows but cannot set.

    @param   name    The name a host shows.
    @param   buffer  The text buffer, a global. Its text is sent up to the terminator.
    @param   size    Its size, terminator included. At most 256.
    @return  A handle for the presentation.

    @code
      char lastError[40] = "";
      device.addSensor(F("LastError"), lastError, sizeof(lastError)).diagnostic();
    @endcode
  */
  BlaeckTextPropertyRef addSensor(BlaeckString name, const char *buffer, size_t size);

  /*!
    @brief   Adds text from a function, which a host shows but cannot set.

    @param   name   The name a host shows.
    @param   value  Returns the text. A static buffer is fine; nullptr means empty.
    @return  A handle for the presentation.

    @code
      device.addSensor(F("Status"), statusText);
    @endcode
  */
  BlaeckTextPropertyRef addSensor(BlaeckString name, const char *(*value)());

  /*!
    @brief   Sends a property's current value now, changed or not.

    tick() sends a property when it changes; call this to send one at a moment the sketch
    chooses, such as a short pulse tick() could miss.

    @warning Not from an interrupt: it writes a frame. Set a flag there and call this in
             loop().

    @param   name  The property's name.

    @code
      device.writeProperty(F("Endstop"));
    @endcode
  */
  void writeProperty(BlaeckString name);

  // ----- Events -----
  // With BLAECK_ENABLE_IOT=0 these compile but do nothing.

  /*!
    @brief   Adds an event, for reporting things that happen.

    @param   channelName  The name a host shows, unique among this device's events. RAM
                          text is copied; an F() literal stays in flash.
    @param   eventTypes   What the event can report, comma-separated.
    @return  A handle for describing how a host shows the event.

    @warning An event with no types, or with a blank one, is refused.

    @code
      device.addEvent(F("Activity"), F("idle_warning,resumed"))
          .withIcon(F("mdi:pulse"));
    @endcode
  */
  BlaeckEventRef addEvent(BlaeckString channelName, BlaeckString eventTypes);

  /*!
    @brief   Adds one more type to an existing event.

    For types that depend on the hardware fitted.

    @param   channelName  An event added with addEvent().
    @param   eventType    The new type.
    @return  False if the type is blank or a duplicate, the event doesn't exist, or
             there is no RAM for it. Each is reported on the debug stream.

    @code
      device.addEvent(F("Activity"), F("idle_warning,resumed"));
      if (hasBatteryMonitor)
        device.addEventType(F("Activity"), F("low_battery"));
    @endcode
  */
  bool addEventType(BlaeckString channelName, BlaeckString eventType);

  /*!
    @brief   Reports an event.

    A host shows it, but it isn't logged as data.

    @param   channelName  An event added with addEvent().
    @param   eventType    One of that event's types.

    @warning An unknown event or type is dropped, with a note on the debug stream.
             Types are case-sensitive.

    @code
      device.writeEvent(F("Activity"), F("idle_warning"));
    @endcode
  */
  void writeEvent(BlaeckString channelName, BlaeckString eventType);

  // ----- Data Write -----

  /*!
    @brief   Sets a signal's value and sends it right away.

    Separate from the timed interval, so a value can go out the moment something
    happens. There is an overload for each type.

    @param   signalName  The signal's name, RAM or F() text.
    @param   value       The new value.
    @param   timestamp   Sample time in microseconds in the timestamp mode's epoch.
                         Leave it out to have blaeck take it from the timestamp mode
                         when the value is sent.
    @note    For a short pulse, write both the rise and the fall. The logged data
             then shows how long it lasted.

    @code
      if (triggered && !Pulse)
      {
        Pulse = true;
        pulseSince = millis();
        device.write("Pulse", Pulse);
      }
      if (Pulse && millis() - pulseSince >= 2000)
      {
        Pulse = false;
        device.write("Pulse", Pulse);
      }
    @endcode
  */
  void write(BlaeckString signalName, bool value, unsigned long long timestamp = BLAECK_NOW);
  void write(BlaeckString signalName, byte value, unsigned long long timestamp = BLAECK_NOW);
  void write(BlaeckString signalName, short value, unsigned long long timestamp = BLAECK_NOW);
  void write(BlaeckString signalName, unsigned short value, unsigned long long timestamp = BLAECK_NOW);
  void write(BlaeckString signalName, int value, unsigned long long timestamp = BLAECK_NOW);
  void write(BlaeckString signalName, unsigned int value, unsigned long long timestamp = BLAECK_NOW);
  void write(BlaeckString signalName, long value, unsigned long long timestamp = BLAECK_NOW);
  void write(BlaeckString signalName, unsigned long value, unsigned long long timestamp = BLAECK_NOW);
  void write(BlaeckString signalName, long long value, unsigned long long timestamp = BLAECK_NOW);
  void write(BlaeckString signalName, float value, unsigned long long timestamp = BLAECK_NOW);
  void write(BlaeckString signalName, double value, unsigned long long timestamp = BLAECK_NOW);

  /*!
    @brief   Points a text signal at a value and sends it right away.

    @param   signalName  The registered text signal's name, RAM or F() text.
    @param   value       Null-terminated text, or F() text, which stays in flash.
    @param   timestamp   Sample time in microseconds. Leave it out to have blaeck take it
                         from the timestamp mode when the value is sent.
    @warning RAM text is not copied. Keep its buffer valid until replaced by another
             text write or the signal is removed. Later reports read the same memory.
             Use a global/static buffer or a string literal, not a local array or
             a temporary String's c_str().

    @code
      device.addSignal(F("Status"), "Idle");
      device.write("Status", "Running");
    @endcode
  */
  void write(BlaeckString signalName, const char *value, unsigned long long timestamp = BLAECK_NOW);
  void write(BlaeckString signalName, const __FlashStringHelper *value, unsigned long long timestamp = BLAECK_NOW);
  void write(BlaeckString signalName, decltype(nullptr), unsigned long long timestamp = BLAECK_NOW)
  {
    write(signalName, static_cast<const char *>(nullptr), timestamp);
  }

  /*!
    @brief   Returns a signal's index, for the faster by-index calls.

    Calls by name compare against every signal's name, so look the index up once in
    setup() for anything that runs often.

    @param   signalName  The name the signal was added with, RAM or F() text,
                         including any numeric suffix.
    @return  Its index, or -1 if there is no signal by that name.

    @code
      int tempIndex = device.findSignalIndex("Temperature");
      device.write(tempIndex, readSensor());
    @endcode
  */
  int findSignalIndex(BlaeckString signalName);

  /*!
    @brief   Sets a signal's value and sends it right away, looking it up by index.

    The same overloads as by name; text as in write(signalName, value).

    @param   signalIndex  The signal's index, from findSignalIndex().
    @param   value        The new value.
    @param   timestamp    Sample time in microseconds. Leave it out to have blaeck take it
                          from the timestamp mode when the value is sent.

    @code
      device.addSignal(F("Status"), "Idle");
      int statusIndex = device.findSignalIndex("Status");
      device.write(statusIndex, "Running");
    @endcode
  */
  void write(int signalIndex, bool value, unsigned long long timestamp = BLAECK_NOW);
  void write(int signalIndex, byte value, unsigned long long timestamp = BLAECK_NOW);
  void write(int signalIndex, short value, unsigned long long timestamp = BLAECK_NOW);
  void write(int signalIndex, unsigned short value, unsigned long long timestamp = BLAECK_NOW);
  void write(int signalIndex, int value, unsigned long long timestamp = BLAECK_NOW);
  void write(int signalIndex, unsigned int value, unsigned long long timestamp = BLAECK_NOW);
  void write(int signalIndex, long value, unsigned long long timestamp = BLAECK_NOW);
  void write(int signalIndex, unsigned long value, unsigned long long timestamp = BLAECK_NOW);
  void write(int signalIndex, long long value, unsigned long long timestamp = BLAECK_NOW);
  void write(int signalIndex, float value, unsigned long long timestamp = BLAECK_NOW);
  void write(int signalIndex, double value, unsigned long long timestamp = BLAECK_NOW);
  void write(int signalIndex, const char *value, unsigned long long timestamp = BLAECK_NOW);
  void write(int signalIndex, const __FlashStringHelper *value, unsigned long long timestamp = BLAECK_NOW);
  void write(int signalIndex, decltype(nullptr), unsigned long long timestamp = BLAECK_NOW)
  {
    write(signalIndex, static_cast<const char *>(nullptr), timestamp);
  }

  // ----- Command callback -----

  /*!
    @brief   Registers a command whose parameters the handler reads as it likes.

    A host doesn't list it; whoever sends it knows its name and parameters. For a value
    a host sets, use an input such as addNumberInput(); for a press, addButton().

    @param   command  The command name. It can't start with `#` or `BLAECK.`.
    @param   handler  Called with the parameters as received.

    @note    A command that can't be registered (no RAM, a name that is too long,
             reserved or taken) is reported on the debug stream and in
             hasRejections().

    @code
      device.onCommand("SwitchLED", onSwitchLED);
    @endcode
  */
  void onCommand(const char *command, BlaeckCommandHandler handler);

  /*!
    @brief   Adds a button, which a host shows and presses.

    A press carries no value. A host presses it by sending its name, `<STATUS>`;
    parameters sent with it are ignored. With BLAECK_ENABLE_IOT=0 it is refused.

    @param   name   The button's name, unique on the board among inputs, sensors,
                    buttons and commands. It can't start with `#` or `BLAECK.`.
    @param   press  Called on each press. A lambda binds fixed arguments, so one
                    function can serve several buttons.
    @return  A handle for describing how a host shows the button.

    @code
      device.addButton("STATUS", onStatus);
    @endcode
  */
  BlaeckButtonRef addButton(const char *name, BlaeckButtonFunction press);

protected:
  BlaeckDeviceBase(Blaeck *core, byte deviceId) : _core(core), _deviceId(deviceId) {}
  // Never deleted through this type, so the destructor need not be virtual.
  ~BlaeckDeviceBase() = default;

  // The Blaeck that holds the tables: for the board Blaeck itself, for a device the board
  // that added it. nullptr in a default or rejected device handle, which ignores every call.
  Blaeck *_core;
  // 0 for the board, 1 and up for a device from addDevice().
  byte _deviceId;

private:
  // Blaeck's calls of the same names, for this device. A handle without a board registers
  // nothing (-1) and sends nothing.
  int _registerSignal(BlaeckString signalName, dataType type, void *address, bool textInFlash = false);
  int _registerCommand(const char *command, BlaeckCommandHandler handler, BlaeckButtonFunction press);
  int _registerEventChannel(BlaeckString channelName, BlaeckString eventTypes);
  int _registerProperty(BlaeckString name, uint8_t kind, bool writable, dataType type, void *address,
                        void (*getter)(), uint8_t getterType, uint16_t textSize, BlaeckString options,
                        BlaeckPropertyCallback onChange);
};

// The handle for a device from addDevice(): another board, or a part of this one, that a host
// shows as its own device. The sketch fetches its values itself, over any link it likes, and
// blaeck reports them under the device. Register the device's signals, commands and channels
// through the handle, as on the board. A default or rejected handle ignores every call.
class BlaeckDeviceRef : public BlaeckDeviceBase
{
public:
  BlaeckDeviceRef() : BlaeckDeviceBase(nullptr, 0) {}

  /*!
    @brief   Sets the device's hardware name or revision. Defaults to "n/a".

    @param   hwVersion  The name. RAM text is copied; an F() literal stays in flash.
    @return  The same handle, for chaining.

    @code
      pump = device.addDevice(F("Pump controller")).withHWVersion(F("Arduino Nano"));
    @endcode
  */
  BlaeckDeviceRef &withHWVersion(BlaeckString hwVersion);

  /*!
    @brief   Sets the device's firmware version. Defaults to "n/a".

    @param   fwVersion  The version. RAM text is copied; an F() literal stays in flash.
    @return  The same handle, for chaining.

    @code
      pump = device.addDevice(F("Pump controller")).withFWVersion(F("1.2"));
    @endcode
  */
  BlaeckDeviceRef &withFWVersion(BlaeckString fwVersion);

  /*!
    @brief   Reports that the device stopped answering.

    A host is told at once, or as soon as one can receive frames, so it can show the
    device as unavailable. Until markPresent(), the device's signals are left out of
    data frames, write() calls for them are dropped, and its commands are refused. The
    sketch decides when a device counts as missing; calling it again changes nothing.

    @code
      if (!pumpAnswered)
        pump.markMissing();
    @endcode
  */
  void markMissing();

  /*!
    @brief   Reports that the device answers again.

    A host is told, and the device's signals return to data frames. Each signal that
    reports on change is sent again at the next chance, since the host lost track of it.
    Its properties are not resent; send them again with writeProperty() if they may have
    changed. Calling it on a device that was not missing changes nothing.

    @code
      if (pumpAnswered)
        pump.markPresent();
    @endcode
  */
  void markPresent();

  /*!
    @brief   Whether markMissing() is in effect.

    @return  True between markMissing() and markPresent().

    @code
      if (pump.isMissing())
        Serial.println(F("Pump controller is not answering."));
    @endcode
  */
  bool isMissing() const;

  /*!
    @brief   Tells a host that the device has restarted.

    Sends a restart notice for the device, at once or as soon as a host can receive
    frames, so a host can report it. The board itself is unaffected. The sketch has to
    notice the restart, for example from an uptime counter the device reports, and send
    the device's current values again, since they may be back at their defaults.

    @code
      if (reading.uptimeMs < lastPumpUptime)
        pump.writeRestarted();
    @endcode
  */
  void writeRestarted();

  /*!
    @brief   Sends every signal of this device now, regardless of the interval.

    Use it when a reading from the device arrives, so the frame carries the time of that
    reading and only the device's signals. Nothing is sent while the device is marked
    missing. The before-write callback does not run.

    @param   timestamp  In microseconds, in the epoch of the timestamp mode. Leave it out
                        to have blaeck take it from the timestamp mode when the frame is
                        sent.

    @code
      if (readFlowFromPump(pumpFlow))
        pump.writeAll();
    @endcode
  */
  void writeAll(unsigned long long timestamp = BLAECK_NOW);

private:
  BlaeckDeviceRef(Blaeck *owner, byte id) : BlaeckDeviceBase(owner, id) {}

  friend class Blaeck;
  friend class BlaeckDeviceBase;
  friend class BlaeckSignalRef;
  friend class BlaeckCommandRefBase;
  friend class BlaeckEventRef;
  friend class BlaeckPropertyRefBase;
};

// Text to the attached Stream or connected TCP terminals.
class BlaeckTerminal : public Print
{
public:
  explicit BlaeckTerminal(Blaeck *owner) : _owner(owner) {}

  /*!
    @brief   Sends one byte to every connected terminal.

    Everything printed to Terminal goes through this; a sketch rarely calls it directly.

    @param   b  The byte.
    @return  1.

    @code
      device.Terminal.write('.');
    @endcode
  */
  size_t write(uint8_t b) override;

  /*!
    @brief   Sends bytes to every connected terminal.

    @param   buffer  The bytes.
    @param   size    How many.
    @return  size.

    @code
      device.Terminal.write((const uint8_t *)"ok\n", 3);
    @endcode
  */
  size_t write(const uint8_t *buffer, size_t size) override;

  using Print::write;

private:
  Blaeck *_owner;
};

// Some hosts only record values and need nothing but signal names and types. Others also
// build controls and displays, and only those use what withUnit(), withIcon() and the other
// descriptive calls declare.
class Blaeck : public BlaeckDeviceBase
{
public:
  Blaeck();
  ~Blaeck();
  Blaeck(const Blaeck &) = delete;
  Blaeck &operator=(const Blaeck &) = delete;

  /*!
    @brief   Sets the name a host lists the board under. Defaults to "Unnamed".

    An empty name is sent as "Unnamed" too. Accepts F() literals, which stay in flash, and
    ordinary strings, which are copied.

    @param   name  The board's name.
    @return  The board, for chaining.

    @code
      device.withName(F("Greenhouse")).withHWVersion(F("Mega")).withFWVersion(F("1.0"));
    @endcode
  */
  Blaeck &withName(BlaeckString name);

  /*!
    @brief   Sets the hardware's name or revision. Defaults to the selected build target.

    Recognised boards use a friendly name, otherwise the core's ARDUINO_BOARD string is used
    if available, or "n/a". The default describes the target selected when compiling, not the
    physical board or PCB revision.

    @param   hwVersion  The hardware's name or revision.
    @return  The board, for chaining.

    @code
      device.withHWVersion(F("Weather Station PCB v2"));
    @endcode
  */
  Blaeck &withHWVersion(BlaeckString hwVersion);

  /*!
    @brief   Sets the firmware's version. Defaults to "n/a".

    @param   fwVersion  The firmware's version.
    @return  The board, for chaining.

    @code
      device.withFWVersion(F("1.0"));
    @endcode
  */
  Blaeck &withFWVersion(BlaeckString fwVersion);

  // ----- Signals -----

  /*!
    @brief   Removes every signal, so a new set can be added.

    The table keeps its memory for the new ones. The rejection counts are reset too.

    @warning A host learns of the new signals from the next device list it asks for.
             A host that is logging stops when the signals change: the schema hash of
             the next data frame no longer matches its columns.

    @code
      device.clearAllSignals();
      device.addSignal(F("Temperature"), &Temperature);
    @endcode
  */
  void clearAllSignals();

  /*!
    @brief   The number of signals added. Valid indexes run from 0 to SignalCount - 1.

    @note    Read it only. Assigning to it breaks the count.

    @code
      Serial.println(device.SignalCount);
    @endcode
  */
  int SignalCount;

  // ----- Device Restarted -----

  /*!
    @brief   Tells a host that the device has just started.

    Sent once per boot, so a host knows to drop what it held from before. tick()
    sends it on its first call; call this only to send it earlier. If a host asks for
    the device list first, the list reports the restart instead, and this sends nothing.

    The entity list follows it, so a host that stayed connected gets it without
    asking.

    @code
      device.writeRestarted();
    @endcode
  */
  void writeRestarted();

  // ----- Devices -----

  /*!
    @brief   Adds a device that a host shows below this one, such as a second board.

    blaeck only reports the device. The sketch talks to it, over I2C, UART or anything
    else, keeps the variables of its signals up to date, and forwards its commands.
    Register the device's signals, commands and channels through the returned handle,
    the same calls as on the board. Add devices in setup(): a host reads the device list
    when it connects.

    A host names the device after the board and the device name, so keep the name
    unique and stable. Signal and channel names only need to be unique within the board
    or within one device; command names within the whole board.

    @param   name  The name a host shows. RAM text is copied; an F() literal stays in flash.
    @return  A handle for the device. If there is no RAM for it, 254 devices exist
             already, or the name is empty or taken, the device is dropped and the
             handle ignores every call; hasRejections() reports it.

    @code
      BlaeckDeviceRef pump = device.addDevice(F("Pump controller"));
      pump.addSignal(F("Flow"), &pumpFlow);
    @endcode
  */
  BlaeckDeviceRef addDevice(BlaeckString name);

  // ----- Events -----
  // With BLAECK_ENABLE_IOT=0 these compile but do nothing.

  /*!
    @brief   Removes every event and event type, so a new set can be added.

    The tables keep their memory. The new list is sent to the host automatically.

    @code
      device.clearAllEvents();
      device.addEvent(F("Activity"), F("idle_warning,resumed"));
    @endcode
  */
  void clearAllEvents();

  // ----- Data Write All -----

  /*!
    @brief   Sends every signal's value now, regardless of the interval.

    The device also does this when a host sends <BLAECK.WRITE_DATA>.

    @param   timestamp  In microseconds, in the epoch of the timestamp mode. Leave it out
                        to have blaeck take it from the timestamp mode when the frame is
                        sent.

    @code
      if (Temperature > 40.0f)
        device.writeAll();
    @endcode
  */
  void writeAll(unsigned long long timestamp = BLAECK_NOW);

  // ----- Tick -----

  /*!
    @brief   Handles incoming commands, then sends what is due. Call it on every
             loop() pass; it is the only call loop() needs.

    First it runs any command that has arrived: the built-in BLAECK.* commands, an
    input's new value, a button or the sketch's handlers. Then it sends the signals
    whose reporting is due - on the host's interval once it has sent INTERVAL_START, and
    signals with writeOnChange() as they change - and the inputs and sensors that
    changed. Nothing is sent when nothing is due.

    @param   timestamp  The data frame's time, in microseconds in the timestamp mode's
                        epoch. Leave it out to have blaeck take it from the timestamp
                        mode when the frame is sent. Scheduling and rate limits use
                        millis() either way.

    @code
      void loop()
      {
        Temperature = readSensor();
        device.tick();
      }
    @endcode
  */
  void tick(unsigned long long timestamp = BLAECK_NOW);



  // ----- Command callback  -----

  /*!
    @brief   Registers a handler that runs for every command.

    It runs after any matching handler, for built-ins and refused commands too, for logging or
    forwarding. A board has one; a second call replaces it. It returns whether it took the
    command: a command nothing else has is then acknowledged as accepted. A handler that only
    logs returns false, so a typo is still answered as unknown.

    @param   handler  Called for every command; returns true if it took one.

    @code
      bool handleAnyCommand(const char *command, const char *const *params, byte count)
      {
        if (strncmp(command, "PUMP_", 5) != 0)
          return false;
        Serial2.print(command);
        for (byte i = 0; i < count; i++)
        {
          Serial2.print(',');
          Serial2.print(params[i]);
        }
        Serial2.println();
        return true;
      }

      void setup()
      {
        device.onAnyCommand(handleAnyCommand);
      }
    @endcode
  */
  void onAnyCommand(BlaeckAnyCommandHandler handler);

  /*!
    @brief   Removes every plain command from onCommand(), and onAnyCommand().

    Buttons stay: they go with clearAllControls(). The table keeps its memory for new ones.

    @code
      device.clearAllCommands();
      device.onCommand("LED", onLED);
    @endcode
  */
  void clearAllCommands();

  /*!
    @brief   Removes every control: the inputs and the buttons.

    What a host shows as controls: addNumberInput(), addTextInput(), addSwitch(),
    addSelect() and addButton(). Sensors stay. The tables keep their memory, and the new
    list is sent to the host.

    @warning Handles returned before the call no longer refer to what they did.

    @code
      device.clearAllControls();
      device.addSwitch(F("Enabled"), &enabled);
    @endcode
  */
  void clearAllControls();

  /*!
    @brief   Removes every sensor from addSensor().

    Controls stay. The table keeps its memory, and the new list is sent to the host.

    @warning Handles returned before the call no longer refer to what they did.

    @code
      device.clearAllSensors();
      device.addSensor(F("Temperature"), &temperature);
    @endcode
  */
  void clearAllSensors();

  /*!
    @brief   Compares a string in RAM with an F() literal, without copying either.

    Comparing with a plain literal would keep that literal in SRAM.

    @param   ram    The string, such as a handler's command argument.
    @param   flash  An F() or PROGMEM literal.
    @return  True if they are equal.

    @code
      device.onAnyCommand([](const char *command, const char *const *params, byte count)
      {
        if (device.equalsFlash(command, F("RESET")))
          Uptime = 0;
        return false;
      });
    @endcode
  */
  static bool equalsFlash(const char *ram, const __FlashStringHelper *flash);

  /*!
    @brief   Writes a float as text into a buffer.

    For building a status text. On AVR, "%f" prints "?" by default, and dtostrf()
    is missing on some cores; this works everywhere and never writes past the
    buffer.

    @param   value     The number.
    @param   decimals  Digits after the point.
    @param   out       The buffer.
    @param   outSize   Size of the buffer, including the terminator.
    @return  out, so it can be passed straight to snprintf().

    @note    A float holds about seven significant digits.

    @code
      char freq[10];
      device.toText(Frequency, 2, freq, sizeof(freq));
    @endcode
  */
  static char *toText(float value, byte decimals, char *out, byte outSize);

  // Stores a channel name: an F() name as its pointer, a RAM name as a heap copy. Frees
  // what the slot held before.
  static bool _setChannelName(const char *&slot, bool &inFlash, const char *ram, const __FlashStringHelper *flash);
  // Equality against a stored channel name, whichever memory it lives in.
  static bool _channelNameEquals(const char *stored, bool inFlash, const char *candidate);
  static bool _channelNameEqualsFlash(const char *stored, bool inFlash, const __FlashStringHelper *candidate);

  /*!
    @brief   Reports rejected declarations, configuration and signal-reporting failures.

    @return  True if a declaration was dropped, a reporting policy was rejected,
             or configuration text or a change-tracking snapshot could not be allocated.

    @code
      if (device.hasRejections())
        device.printRejections(&Serial);
    @endcode
  */
  bool hasRejections() const;
  /*!
    @brief   Prints rejection counts and affected table capacities.

    Prints nothing when there were no rejections. A rejection can mean an invalid
    declaration or insufficient memory.
    Also includes configuration text and signal snapshot allocation failures.
    Enable withDebugStream() for details when a failure occurs.

    @param   out  Where to print. The data port is fine when called from setup().
    @return  True if anything was printed.

    @code
      device.printRejections(&Serial);
    @endcode
  */
  bool printRejections(Print *out);

  /*!
    @brief   Copies the name of a select's option at a given position.

    Works for addSelect() and for addSensor() with options.

    @param   name     The select's or sensor's name.
    @param   index    Position in its options, starting at 0.
    @param   out      Where the name is copied. Left empty if this returns false.
    @param   outSize  Size of out, including the terminator.
    @return  False if it has no options, the index is past the end, or the name
             doesn't fit. A name is never cut short.

    @code
      char name[12];
      device.getSelectOptionNameAt(F("Waveform"), waveIndex, name, sizeof(name));
    @endcode
  */
  bool getSelectOptionNameAt(BlaeckString name, byte index, char *out, byte outSize) const;

  /*!
    @brief   Returns the position of an option in a select's list.

    Case-sensitive. Works for addSelect() and for addSensor() with options.

    @param   name        The select's or sensor's name.
    @param   optionName  The option to look for.
    @return  Its position, starting at 0, or -1 if it has no options or no such
             option.
    @note    Useful for restoring a setting saved as a name. A saved index would point
             at the wrong option if a later firmware reorders the list.

    @code
      char saved[12];
      EEPROM.get(addr, saved);
      long i = device.getSelectOptionIndexOf(F("Waveform"), saved);
      waveIndex = (i >= 0) ? (byte)i : 0;
    @endcode
  */
  long getSelectOptionIndexOf(BlaeckString name, const char *optionName) const;

  /*!
    @brief   Sets a function to refresh values before interval and full snapshots.

    Runs before filtering a due host interval, even if no signals qualify, and before
    writeAll(), including host requests. It does not run for single-signal write()
    or the every-tick writeOnChange() check. It runs from loop(), not an interrupt.

    @param   callback  The refresh function, or nullptr to remove it.

    @code
      device.onBeforeWrite(readAllSensors);
    @endcode
  */
  void onBeforeWrite(void (*callback)());

  /*!
    @brief   Sets a function to call when the host sends BLAECK.DATA_START.

    The host sends it when it starts receiving data, such as when logging starts. By then
    the board sends data again and every writeOnChange() signal is due to send its current
    value, so a write() here, of an Explicit signal for example, goes out too. It is not
    called after a restart; setup() runs then.

    @param   callback  The function, or nullptr to remove it.

    @code
      device.onDataStart([]() { device.write("Mode", mode); });
    @endcode
  */
  void onDataStart(void (*callback)());

  /*!
    @brief   Sets a function to call when the host sends BLAECK.DATA_STOP.

    Runs before the board stops sending data on its own, so a last write() still goes
    out. A host that crashes or loses the link never sends it, so don't use it to make
    anything safe.

    @param   callback  The function, or nullptr to remove it.

    @code
      device.onDataStop([]() { device.Terminal.println(F("stopped")); });
    @endcode
  */
  void onDataStop(void (*callback)());

  /*!
    @brief   Sets a function to call when the host sends BLAECK.INTERVAL_START.

    @param   callback  Receives the interval in milliseconds; nullptr removes it.

    @code
      device.onIntervalStart([](uint32_t ms) { device.Terminal.println(ms); });
    @endcode
  */
  void onIntervalStart(void (*callback)(uint32_t intervalMs));

  /*!
    @brief   Sets a function to call when the interval stops: on BLAECK.INTERVAL_STOP, and
             on BLAECK.DATA_STOP while the interval runs.

    @param   callback  The function, or nullptr to remove it.

    @code
      device.onIntervalStop([]() { device.Terminal.println(F("interval stopped")); });
    @endcode
  */
  void onIntervalStop(void (*callback)());

  /*!
    @brief   Sets whether and how data is timestamped.

    BLAECK_NO_TIMESTAMP, the default, sends none and the host uses arrival time.
    BLAECK_MICROS uses micros(), extended so it keeps counting past its 71-minute
    rollover, or the clock given. BLAECK_UNIX needs a clock: a function returning
    microseconds since the Unix epoch, such as one reading an RTC or NTP time. An RTC that
    counts seconds has to be multiplied by 1000000.

    @param   mode   A BlaeckTimestampMode value.
    @param   clock  The function the timestamps are read from; nullptr for micros().

    @warning Set it in setup(). Changing it later restarts the count, so timestamps
             before and after don't line up.

    @note    BLAECK_MICROS notices a rollover only when the clock is read. tick() and
             writeIfDue() read it on every call, so a running loop() is enough; a sketch
             that passes its own timestamp to them instead needs data at least every
             71 minutes, or BLAECK_UNIX.

    @code
      device.setTimestampMode(BLAECK_MICROS);
      device.setTimestampMode(BLAECK_UNIX, unixMicros);
    @endcode
  */
  void setTimestampMode(BlaeckTimestampMode mode, unsigned long long (*clock)() = nullptr);

  /*!
    @brief   Reports whether timestamps will be real.

    @return  True if a timestamp mode is set and it has a clock to read.

    @warning BLAECK_UNIX without a clock sends 0 as every timestamp.

    @code
      if (!device.hasTimestampClock())
        Serial.println(F("no clock - timestamps will be zero"));
    @endcode
  */
  bool hasTimestampClock() const;

  /*!
    @brief   Sets whether each frame is built in RAM and sent in one write.

    Buffering uses SRAM; without it, bytes go out as they are produced. Off by
    default for Serial on AVR, on for Serial elsewhere and for TCP on every board.

    @param   enabled  true to buffer.

    @code
      device.setBufferedWrites(true);
    @endcode
  */
  void setBufferedWrites(bool enabled);
  /*!
    @brief   Reports whether frames are buffered in RAM before sending.

    @return  True if buffering is on, by default or through setBufferedWrites().

    @code
      Serial.println(device.isBufferedWrites() ? F("buffered") : F("direct"));
    @endcode
  */
  bool isBufferedWrites() const { return _bufferedWrites; }

  /*!
    @brief   Attaches the library to an already-open Stream.

    Call begin() only once per instance, normally in setup(). Further calls, even
    after end(), report BeginAlreadyCalled and leave the transport and catalogs
    unchanged. Their returned handles ignore all chained settings. A latched
    OutOfMemory error takes precedence.
    The caller owns the stream and keeps it alive until end().

    @param   stream  The Stream opened by the sketch.
    @return  A setup handle; an ignored handle if begin() was already called.

    @code
      Serial.begin(115200);
      device.begin(Serial);
    @endcode
  */
  BlaeckBeginRef begin(Stream &stream);

  /*!
    @brief   Attaches the library to an already-started TCP server.

    The sketch owns the server, starts it and keeps it alive until end(). Do not let
    another consumer accept from the same server. A connection becomes a host when it sends
    a BLAECK. command, such as <BLAECK.GET_DEVICES>, and receives frames from then on.
    Every other connection is a terminal: it receives the text sent to Terminal, and
    its commands run but aren't answered.
    Call begin() only once per instance, even if initialization fails or end() is
    called. Further calls report BeginAlreadyCalled without changing the transport
    or catalogs, and their handles ignore chained settings. A latched OutOfMemory
    error takes precedence. Reconnecting TCP clients does not require another begin().

    @param   server  A server with accept() returning a Client-derived value.
    @return  A handle for setting the number of connections and a debug stream. Each
             has a default, so the handle can be ignored.

    @code
      server.begin();
      device.begin(server)
          .withClients(4)
          .withDebugStream(&device.Terminal);
    @endcode
  */
  template<class Server>
  auto begin(Server &server) -> decltype(server.accept(), BlaeckBeginRef(this))
  {
    if (!_beginOnce())
      return BlaeckBeginRef(nullptr);
    _resetSignalCatalog();
    _tcpSelected = true;
    _setBufferedWritesDefault(BLAECK_TCP_BUFFERED_WRITES_DEFAULT);
    _adapter = new (std::nothrow) blaeck::detail::TypedServerAdapter<Server>(
        server, BLAECK_TCP_NO_DELAY_DEFAULT, BLAECK_TCP_STOP_TIMEOUT_MS);
    _setTransportError(_adapter != nullptr ? TransportError::None : TransportError::OutOfMemory);
    return BlaeckBeginRef(this);
  }

  // Migration guard: start a server yourself, then pass that object rather than its port.
  BlaeckBeginRef begin(uint16_t port) = delete;

  /*!
    @brief   Detaches the stream or closes accepted TCP clients.

    Never stops the caller's stream or server. Also called by the destructor.
    Safe to call repeatedly, but does not allow another begin() on this instance.

    @code
      device.end();
    @endcode
  */
  void end();

  enum class TransportError : byte
  {
    None,
    NotStarted,
    OutOfMemory,
    InvalidClientCount,
    ClientLimitLocked,
    NotServer,
    BeginAlreadyCalled
  };

  /*!
    @brief   Returns the transport's current error status.

    Separate from table-registration rejections reported by hasRejections().
    TCP client storage is allocated on the first tick(), so check afterward.

    @code
      if (device.transportError() != Blaeck::TransportError::None)
        device.printTransportError(&Serial);
    @endcode
  */
  TransportError transportError() const { return _transportError; }
  /*!
    @brief   Prints the current transport error, if any.

    @param   out  Where to print. A null pointer produces no output.
    @return  True if an error was printed.

    @code
      device.printTransportError(&Serial);
    @endcode
  */
  bool printTransportError(Print *out) const;

  /*!
    @brief   Text to the attached Stream, or to every connected TCP terminal.

    TCP hosts never receive it. Pass it to withDebugStream() to see on a terminal what
    the library refuses and which commands arrive.

    @note    A terminal that connects but never reads can fill its send buffer, and a
             write to it may then wait. Short lines don't get there.

    @code
      device.Terminal.println("LED is ON.");
    @endcode
  */
  BlaeckTerminal Terminal;

  /*!
    @brief   Sets a function to call when a connection opens.

    @param   callback  Receives the connection's slot, starting at 0.

    @code
      device.onConnect(onClientConnected);
    @endcode
  */
  void onConnect(void (*callback)(byte clientNo));

  /*!
    @brief   Sets a function to call when a connection closes.

    A connection that dies without closing, such as one whose cable was pulled, is
    noticed only when the network stack gives up on it.

    @param   callback  Receives the connection's slot, starting at 0.

    @code
      device.onDisconnect(onClientDisconnected);
    @endcode
  */
  void onDisconnect(void (*callback)(byte clientNo));

protected:
  // The two halves of tick(). Protected, so a test can call them apart through a subclass.
  // Handles an incoming command, if one has arrived; sends no data.
  void read();
  // Sends the signals and properties whose reporting is due. The first interval after every
  // INTERVAL_START includes every interval signal without change filtering.
  void writeIfDue(unsigned long long timestamp = BLAECK_NOW);

  void _setBufferedWritesDefault(bool enabled);

  // Empties the signal table and resets its counts before attaching a connection.
  void _resetSignalCatalog();

  // ----- Connection I/O -----
  // True once a frame may be written, e.g. the transport has a stream.
  bool _transportReady() const;
  // Writes frame bytes as they are produced, when buffered writes are off.
  void _writeDirect(const byte *data, size_t len);
  // Ends an unbuffered frame.
  void _flushDirect();
  // Sends the finished frame in _frameBuf, _framePos bytes long.
  void _sendBuffered();
  // Feeds received bytes to _receiveByte(_receiver, ...). True when _receiver holds a
  // complete command.
  bool _receiveCommand();
  // Called when a received command's name starts with BLAECK., before anything answers it.
  // A transport with several connections makes the sender the host here.
  void _builtinCommandReceived();
  // False for a terminal, which never receives frames and so gets no acknowledgement.
  bool _requesterIsHost() const;
  // Sent in the device frames.
  const char *_libraryName() const { return "blaeck"; }
  const char *_libraryVersion() const { return BLAECK_VERSION; }

  unsigned long long getTimeStamp();
  void setSignalName(int signalIndex, const char *signalName);
  // Sets a signal's name: a heap copy of ram, or the flash pointer. Exactly one is non-null.
  // Frees the copy the slot held before.
  void _setSignalName(int signalIndex, const char *ram, const __FlashStringHelper *flash);
  // Frees the name copies the signal table owns.
  void _freeSignalOwned();
  // Signal names are read only through these helpers, which handle flash and RAM names.
  bool _signalNameEquals(const Signal &s, const char *name, bool nameInFlash = false) const;
  // Where _emitSignalName() sends the bytes: into the frame, the schema hash or the debug stream.
  enum NameSink : uint8_t
  {
    NAME_SINK_FRAME,
    NAME_SINK_HASH,
    NAME_SINK_DEBUG
  };
  void _emitSignalName(const Signal &s, NameSink sink);
  void _emitNameByte(byte c, NameSink sink);
  // The name suffix as decimal digits, without a terminator. out must hold three chars.
  static byte _signalSuffixDigits(const Signal &s, char *out);
  void _signalNameFeedHash(const Signal &s);
  void _emitSignalName0(const Signal &s);
  void _setTimedDataState(bool timedActivated, unsigned long timedInterval_ms);
  void _parseCommandTokens(const char *raw);
  // Runs the registered handler for the command _parseCommandTokens() last parsed.
  void _dispatchRegisteredHandlers(bool sendAck = true);

  // Acknowledges a command. The hashes let a host match the ack to what it sent.
  void _writeCommandAck(const char *rawCommand, byte status, byte reasonCode);
  static uint32_t _fnv1a32(const char *s);
  uint16_t _computeSchemaHash();
  inline void _schemaHashFeedByte(byte b)
  {
    _schemaHashAccum ^= ((uint16_t)b << 8);
    for (byte k = 0; k < 8; k++)
    {
      if (_schemaHashAccum & 0x8000)
        _schemaHashAccum = (_schemaHashAccum << 1) ^ 0x1021;
      else
        _schemaHashAccum <<= 1;
    }
  }

  void _setSignalInterval(int16_t index, BlaeckIntervalMode mode, double delta);
  void _setSignalOnChange(int16_t index, double delta, uint32_t minIntervalMs);
  void _setSignalOnChange(int16_t index, BlaeckIntervalMode mode);
  ReportingState *_ensureReporting(int16_t index);
  void _reportSignalPolicyError(const __FlashStringHelper *message);
  void _resetReportingBaselines();
  bool _prepareTextSnapshot(ReportingState &reporting, size_t length);
  bool _prepareSignalSnapshot(Signal &signal);
  void _captureSignalSnapshot(Signal &signal);
  bool _signalChanged(const Signal &signal, double delta) const;

  void writeAll(unsigned long messageID, unsigned long long timestamp);

  // Store a value in a signal, converted to its type. False if there is no such signal or it
  // holds text.
  bool _storeSigned(int signalIndex, long value);
  bool _storeUnsigned(int signalIndex, unsigned long value);
  bool _storeLongLong(int signalIndex, long long value);
  bool _storeFloating(int signalIndex, double value);

  void writeData(unsigned long messageID, int signalIndex_start, int signalIndex_end, bool selectedOnly, unsigned long long timestamp);
  void writeDataFrame(unsigned long MessageID, int signalIndex_start, int signalIndex_end, bool selectedOnly, unsigned long long timestamp, bool intervalReport = false);

  // Forms that echo the message id of the request they answer. Only read() has one.
  void writeRestarted(unsigned long messageID);
  void writeDevices(unsigned long messageID);

  // Add a signal and return its index, or -1 if it was rejected. All addSignal() overloads
  // end up here.
  int _registerSignal(byte deviceId, const char *signalName, dataType type, void *address, bool textInFlash = false);
  int _registerSignal(byte deviceId, const __FlashStringHelper *signalName, dataType type, void *address, bool textInFlash = false);
  // Exactly one of ram and flash is non-null.
  int _registerSignalCommon(byte deviceId, const char *ram, const __FlashStringHelper *flash,
                            dataType type, void *address, bool textInFlash);
  void _writeSignalText(int signalIndex, const void *value, bool inFlash, unsigned long long timestamp);
  void _emitTextBytes(const void *text, bool inFlash, size_t length);
  // The lookups behind BlaeckDeviceBase's findSignalIndex(), addEventType() and writeEvent().
  int _findSignalIndex(byte deviceId, const char *signalName);
  int _findSignalIndex(byte deviceId, const __FlashStringHelper *signalName);
  bool _addEventType(byte deviceId, const char *channelName, BlaeckString eventType);
  void _writeEvent(byte deviceId, const char *channelName, BlaeckString eventType);
  // Registers a plain command (handler set) or a button (press set) and returns its table
  // index, or -1 if it was rejected (counted, and reported on the debug stream).
  int _registerCommand(byte deviceId, const char *command, BlaeckCommandHandler handler, BlaeckButtonFunction press);
  // Empties an entry, so registering a name again starts from scratch.
  static void _resetCommand(blaeck_detail::CommandHandlerEntry &e);
  // Adds an event and returns its index, or -1 if it was rejected. Adding an existing
  // name reuses its slot and keeps its types. Exactly one of channelName and flashName is set;
  // a flash name is kept as a pointer, a RAM name is copied.
  int _registerEventChannel(byte deviceId, const char *channelName, const __FlashStringHelper *flashName, BlaeckString eventTypes);
  // Adds one event type per comma-separated field, in order.
  void _addEventTypesCsv(uint16_t channelIndex, const detail::StoredString &eventTypes);
  static void _percentDecodeInPlace(char *s);
  static long _flashCsvIndexOf(BlaeckString csv, const char *value);
  // Number of fields in a comma-separated string.
  static uint16_t _flashCsvOptionCount(BlaeckString csv);

  // True if any field is empty or only spaces. Such a list is refused, because dropping the
  // field would shift every later field's index.
  static bool _flashCsvHasBlankField(BlaeckString csv);
#if BLAECK_ENABLE_IOT
  // Index of an added event, or -1 when the name was never added.
  int _findEventChannel(byte deviceId, const char *channelName) const;
  int _findEventChannel(byte deviceId, const __FlashStringHelper *channelName) const;
  // Position of an event type within its own channel's list, or -1 when that
  // channel never declared it.
  int _findEventType(uint16_t channelIndex, BlaeckString eventType) const;
  // Compares two flash strings. strcmp_P() can't, because it reads its first argument from RAM.
  static bool _flashStringEquals(const __FlashStringHelper *a, const __FlashStringHelper *b);
#endif

  void writeDevicesFrame(unsigned long MessageID);


  static void validatePlatformSizes();

  Print *_debugStream = nullptr;
  bool _storeString(detail::StoredString &slot, BlaeckString value);
  uint16_t _rejectedStringCount = 0;
  detail::ChunkList<Signal> Signals;
  int _signalIndex = 0;
  uint16_t _rejectedSignalCount = 0;
  uint16_t _rejectedSignalPolicyCount = 0;
  uint16_t _rejectedCommandCount = 0;
  uint16_t _rejectedEventChannelCount = 0;
  uint16_t _rejectedEventTypeCount = 0;
  uint16_t _rejectedDeviceCount = 0;

  bool _writeRestartedAlreadyDone = false;
  bool _sendRestartFlag = true;
  // Set while answering BLAECK.WRITE_DATA, so the data frame can say it was requested.
  bool _frameRequested = false;

  // Bit 0: restart; bit 1: requested; bit 2: interval report. Bits 3-7 stay clear.
  byte _frameFlags(bool restarted, bool intervalReport) const
  {
    return (byte)((restarted ? 0x01 : 0x00) |
                  (_frameRequested ? 0x02 : (intervalReport ? 0x04 : 0x00)));
  }

  // For extending micros() past its rollover in BLAECK_MICROS mode.
  unsigned long _prevMicros = 0;
  unsigned long long _overflowCount = 0;

  bool _timedActivated = false;
  bool _timedFirstTime = true;
  uint32_t _lastIntervalMs = 0;
  unsigned long _timedInterval_ms = 1000;

  // ── Table limits ──────────────────────────────────────────────────
  // For an entry that found no room, prints what was dropped and why.
  void _warnNoRoom(const __FlashStringHelper *what, const char *droppedName);
  void _warnNoRoom(const __FlashStringHelper *what, const __FlashStringHelper *droppedName);
  void _warnNoRoom(const __FlashStringHelper *what, BlaeckString droppedName);
  // One line of printRejections(), for a table that dropped something.
  void _printRejectionLine(Print *out, const __FlashStringHelper *what, uint16_t dropped);

  // The largest size any table accepts. Handles store their index as int16_t, with negative
  // values meaning rejected. In practice RAM runs out long before this.
  static const uint16_t MAX_TABLE_ENTRIES = INT16_MAX;

  // Makes sure a slot table has a free slot, growing it by a chunk if every slot is in use.
  // False if the table is at MAX_TABLE_ENTRIES or RAM ran out.
  template <class T>
  static bool _roomFor(detail::ChunkList<T> &table)
  {
    for (uint16_t i = 0; i < table.capacity(); ++i)
      if (!table[i].inUse)
        return true;
    return table.capacity() < MAX_TABLE_ENTRIES && table.reserve(table.capacity() + 1);
  }

  // Device IDs are one byte and 0 is the board itself.
  // Device IDs 1-254: the device list counts the board and its devices in one byte.
  static const byte MAX_DEVICES = 254;
  // C1 Device Notification events.
  static const byte DEVICE_EVENT_RESTARTED = 0x01;
  static const byte DEVICE_EVENT_NOT_RESPONDING = 0x02;
  static const byte DEVICE_EVENT_RESPONDING = 0x03;
  // B7 DeviceState bits.
  static const byte DEVICE_STATE_NOT_RESPONDING = 0x01;
  static const byte DEVICE_STATE_RESTARTED = 0x02;

  // Fixed name and buffer lengths. They set the layout of each entry, so they can't change at
  // runtime.
  static const uint16_t MAXIMUM_CHAR_COUNT = BLAECK_COMMAND_MAX_CHARS_DEFAULT;
  static const byte MAX_COMMAND_PARAM_COUNT = 10;
  static const byte MAX_COMMAND_NAME_COUNT = blaeck_detail::MAX_COMMAND_NAME_COUNT;
  // Room for the longest built-in command name, which may be longer than a sketch's command
  // names are allowed to be. The assertion below fails the build if one doesn't fit.
  static const byte MAX_BUILTIN_COMMAND_COUNT = 28;
#define BLAECK_ASSERT_BUILTIN_FITS(name)                        \
  static_assert(sizeof(name) <= MAX_BUILTIN_COMMAND_COUNT,      \
                "MAX_BUILTIN_COMMAND_COUNT must fit every name in BLAECK_BUILTIN_COMMAND_LIST");
  BLAECK_BUILTIN_COMMAND_LIST(BLAECK_ASSERT_BUILTIN_FITS)
#undef BLAECK_ASSERT_BUILTIN_FITS
  static const byte MAX_PARSED_COMMAND_COUNT =
      MAX_COMMAND_NAME_COUNT > MAX_BUILTIN_COMMAND_COUNT ? MAX_COMMAND_NAME_COUNT
                                                         : MAX_BUILTIN_COMMAND_COUNT;
  // Longest event name, terminator included. Defined even without the IoT part, because
  // addEvent() still compiles.
#if defined(__AVR__)
  static const byte MAX_EVENT_NAME_COUNT = 16;
#else
  static const byte MAX_EVENT_NAME_COUNT = 32;
#endif
  // One command being collected between its markers. Apart from the parse results, so a
  // transport with several connections can hold one for each.
  struct Receiver
  {
    char chars[MAXIMUM_CHAR_COUNT];
    uint16_t ndx = 0;
    bool inProgress = false;
    // Set once the command's characters no longer fit and are being dropped. The receive
    // loop is the only place that can see it happen.
    bool overflowed = false;
  };
  Receiver _receiver;
  // Takes one byte into r; true when it ended a command, which r.chars then holds.
  bool _receiveByte(Receiver &r, char c);

  detail::BlaeckCRC32 _crc;
  uint16_t _schemaHash = 0;
  uint16_t _schemaHashAccum = 0;

  // ── Buffered writes ───────────────────────────────────────────────
  bool _bufferedWrites;
  bool _bufferedWritesExplicit = false;
  byte *_frameBuf = nullptr;
  int _framePos = 0;
  int _frameBufSize = 0;
  bool _bufOverflow = false;
  bool _bufOverflowWarned = false;

  void _bufAllocate();
  // True when frames are buffered and the buffer exists. It is allocated on first use,
  // sized from the signals added by then.
  bool _bufReady()
  {
    if (!_bufferedWrites)
      return false;
    if (_frameBuf == nullptr)
      _bufAllocate();
    return _frameBuf != nullptr;
  }
  // Set at start and by DATA_STOP, cleared by DATA_START, so a board nobody listens to stays
  // quiet. Answers to commands go out either way.
  bool _dataStopped = true;
  // Set at start and by ENTITIES_STOP, cleared by ENTITIES_START. Answers to commands go out
  // either way.
  bool _entitiesStopped = true;
  // Set while a command is handled, so whatever it causes goes out even when stopped.
  bool _answering = false;

  // Checked once per frame, when it opens.
  bool _mayWriteFrame() { return _transportReady(); }

  // For data frames the board sends on its own. Held back after DATA_STOP, unless a command
  // caused them.
  bool _mayWriteData() { return (!_dataStopped || _answering) && _mayWriteFrame(); }

  // For property changes and events the board sends on its own. Held back after ENTITIES_STOP,
  // unless a command caused them. Device notices and catalogs are not affected.
  bool _mayWriteEntityFrame() { return (!_entitiesStopped || _answering) && _mayWriteFrame(); }

  // The board's name, or "Unnamed" when none or an empty one is set.
  BlaeckString _deviceName() const
  {
    const BlaeckString name = _boardName;
    return (name == nullptr || name.read(0) == 0)
               ? BlaeckString(BLAECK_DEVICE_NAME_UNNAMED)
               : name;
  }

  bool _bufEnsure(size_t addLen);
  void _bufFree();
  void _bufReset()
  {
    _framePos = 0;
    _bufOverflow = false;
    _bufOverflowWarned = false;
  }
  // ── Frame output ──────────────────────────────────────────────────
  // Every frame is written through these. Between _frameOpen() and _frameClose() the bytes go
  // into the buffer, or straight to the stream if buffering is off or the buffer couldn't be
  // allocated.
  bool _frameDirect = false;
  bool _frameWriteFailed = false;
  bool _shortWriteReported = false;
  // On while a data frame's CRC is being computed.
  bool _frameCrcOn = false;
  // The board's identity, from withName(), withHWVersion() and withFWVersion(); unset is the default.
  detail::StoredString _boardName;
  detail::StoredString _boardHW;
  detail::StoredString _boardFW;
  // Set while an entity-list entry is written once to count its bytes; see _emitEntry().
  bool _measuring = false;
  size_t _measured = 0;
  // Set between the start and the end marker, where <, \, CR and LF are escaped.
  bool _frameEscaped = false;

  // Starts a frame. False if no frame may be written (no host yet).
  bool _frameOpen(byte msgKey, unsigned long msgId);
  // False if buffering failed or the transport did not accept every byte.
  bool _frameClose();
  uint32_t _frameCrcEnd()
  {
    _frameCrcOn = false;
    return _crc.calc();
  }
  // Bytes as they go on the wire: to the transport, or into the frame buffer.
  void _putBytes(const byte *data, size_t len)
  {
    if (len == 0)
      return;
    if (_frameDirect)
      _writeDirect(data, len);
    else if (_bufEnsure(len))
    {
      memcpy(_frameBuf + _framePos, data, len);
      _framePos += len;
    }
    else
      _bufOverflow = true;
  }
  // Within a frame, a byte that would read as a frame's start (<), an escape (backslash) or a
  // line's end (CR, LF) goes as a backslash and the byte XOR 0x20. Layout: Escaping in the
  // protocol spec.
  static bool _mustEscape(byte b) { return b == '<' || b == '\\' || b == '\r' || b == '\n'; }
  // Frame content; the CRC covers it as it is, before escaping.
  void _emitByte(byte b) { _emitBytes(&b, 1); }
  void _emitBytes(const byte *data, size_t len)
  {
    // Measuring an entity-list entry: count what would be written, write nothing.
    if (_measuring)
    {
      _measured += len;
      return;
    }
    if (_frameCrcOn)
      _crc.add(data, len);
    if (!_frameEscaped)
    {
      _putBytes(data, len);
      return;
    }
    size_t run = 0;
    for (size_t i = 0; i < len; ++i)
    {
      if (!_mustEscape(data[i]))
        continue;
      _putBytes(data + run, i - run);
      const byte escaped[2] = {'\\', (byte)(data[i] ^ 0x20)};
      _putBytes(escaped, 2);
      run = i + 1;
    }
    _putBytes(data + run, len - run);
  }
  void _emitStr(const char *s)
  {
    // nullptr writes nothing, as Arduino's Print does.
    if (s == nullptr)
      return;
    _emitBytes((const byte *)s, strlen(s));
  }
  void _emitStr0(const char *s)
  {
    _emitStr(s);
    _emitByte(0);
  }
  void _emitFlashStr(const __FlashStringHelper *s)
  {
    if (s == nullptr)
      return;
    PGM_P p = reinterpret_cast<PGM_P>(s);
    byte c;
    while ((c = pgm_read_byte(p++)) != 0)
      _emitByte(c);
  }
  void _emitFlashStr0(const __FlashStringHelper *s)
  {
    _emitFlashStr(s);
    _emitByte(0);
  }
  void _emitFlashStr0(BlaeckString s)
  {
    if (s != nullptr)
      for (size_t i = 0; s.read(i) != 0; ++i)
        _emitByte(s.read(i));
    _emitByte(0);
  }
  bool _bufSend()
  {
    if (_bufOverflow)
    {
      if (!_bufOverflowWarned && _debugStream != nullptr)
      {
        _debugStream->println(F("Buffered frame exceeds available memory; frame dropped."));
        _bufOverflowWarned = true;
      }
      return false;
    }
    _sendBuffered();
    return !_frameWriteFailed;
  }
  // One device record of the B7 device list.
  void _emitDeviceRecord(byte deviceId, byte state, BlaeckString name, BlaeckString hw, BlaeckString fw);
  // The signal count and each signal's name and type code, for one device of the list.
  void _emitDeviceSignals(byte deviceId);
  // Sends each device's pending C1 notices; stops at the first that can't be sent.
  void _writeDeviceNotices();
  // One C1 frame; false if it could not be sent.
  bool _writeDeviceNotice(byte deviceId, byte event);
  // write() of one signal: dropped, with a debug note, while its device is missing.
  void _writeSignalNow(int signalIndex, unsigned long long timestamp);
  // writeAll() for one device from addDevice().
  void _writeDeviceSignals(byte deviceId, unsigned long long timestamp);
  // The owner of a catalog entry or push: 0 for the board, 1-254 for a device from addDevice(),
  // the same DeviceID as in the B7 device list.
  void _emitDeviceId(byte deviceId) { _emitByte(deviceId); }
  // A range bound, at the width and the type of the variable it bounds.
  void _emitRangeValue(dataType type, const blaeck_detail::RangeBound &b);
  // One entity-list entry: its DeviceID and kind, the length of what fields() writes, then the
  // fields. fields() runs twice, first only counting, so it must write the same both times.
  template <typename Fields>
  void _emitEntry(byte deviceId, byte kind, Fields fields)
  {
    _emitDeviceId(deviceId);
    _emitByte(kind);
    _measuring = true;
    _measured = 0;
    fields();
    _measuring = false;
    const uint16_t length = static_cast<uint16_t>(_measured);
    _emitByte(static_cast<byte>(length & 0xFF));
    _emitByte(static_cast<byte>(length >> 8));
    fields();
  }

  static unsigned long long _microsWrapper()
  {
    return (unsigned long long)micros();
  }

  typedef blaeck_detail::CommandHandlerEntry CommandHandlerEntry;
  detail::ChunkList<CommandHandlerEntry> _commandHandlers;
  // Entries that exist, in use or free. Loops over a table stop here.
  uint16_t _commandSlots() const { return _commandHandlers.capacity(); }

  // ── Devices ───────────────────────────────────────────────────────
  // Added in order and never removed, so a device's DeviceID is its index plus one.
  typedef blaeck_detail::DeviceEntry DeviceEntry;
  detail::ChunkList<DeviceEntry> _devices;
  byte _deviceCount = 0;
  // The entry for a DeviceID, or nullptr for 0 or an ID never handed out.
  DeviceEntry *_deviceEntry(byte id) const
  {
    return (id != 0 && id <= _deviceCount) ? &_devices[id - 1] : nullptr;
  }
  bool _deviceMissing(byte id) const
  {
    const DeviceEntry *d = _deviceEntry(id);
    return d != nullptr && d->missing;
  }
  void _setDeviceMissing(byte id, bool missing);
  void _writeDeviceRestarted(byte id);

  // Sends each catalog that changed since it was last sent. Called after anything that can
  // change one, and before a state or event push. Never sends the device list: a host lays out
  // its storage by its signals, so a changed list mid-session must be sent by the sketch on
  // purpose.
  void _flushCatalogs();

  // The datatype's code in the device list and the entity list. Used by the schema hash too.
  static byte _dtypeCode(dataType t);
  // Compares a flash string with a RAM string.
  static bool _flashStringEqualsName(const __FlashStringHelper *flashName, const char *name);
#if BLAECK_ENABLE_IOT
  typedef blaeck_detail::EventChannelEntry EventChannelEntry;
  detail::ChunkList<EventChannelEntry> _eventChannels;
  uint16_t _eventChannelSlots() const { return _eventChannels.capacity(); }

  // One table of event types for all events. Each entry names its channel; a type's index
  // is its position among its channel's entries.
  typedef blaeck_detail::EventTypeEntry EventTypeEntry;
  static const byte WHOLE_STRING = blaeck_detail::WHOLE_STRING;

  // Where the entry's name starts in its stored string, and its length.
  static void _eventTypeExtent(const EventTypeEntry &e, unsigned int &start, unsigned int &len);
  // Compares the entry's name with eventType.
  static bool _eventTypeEquals(const EventTypeEntry &e, BlaeckString eventType);
  // Emits the entry's name with a terminator.
  void _emitEventType0(const EventTypeEntry &e);
  detail::ChunkList<EventTypeEntry> _eventTypes;
  uint16_t _eventTypeCount = 0;
#endif
  BlaeckAnyCommandHandler _anyCommandHandler = nullptr;
  char _parsedTokenBuffer[MAXIMUM_CHAR_COUNT] = {0};
  char _parsedCommand[MAX_PARSED_COMMAND_COUNT] = {0};
  const char *_parsedParamPtrs[MAX_COMMAND_PARAM_COUNT] = {0};
  byte _parsedParamCount = 0;
  // The command didn't arrive whole, so no handler may run on it.
  bool _parsedTruncated = false;
  // The message id from the command's '#' prefix, echoed in its ack and reply. 0 if none.
  uint16_t _parsedPrefixMsgId = 0;
  // Length of the prefix. The ack's hash covers what follows it.
  uint16_t _parsedPrefixLen = 0;

  // ── Properties ────────────────────────────────────────────────────
  // Added in order and never removed, so a property's index is its position among the
  // properties of the entity list, which 0x95 frames carry. _entityCatalogDirty covers
  // events and buttons too.
  typedef blaeck_detail::PropertyEntry PropertyEntry;
  detail::ChunkList<PropertyEntry> _properties;
  uint16_t _propertyCount = 0;
  uint16_t _rejectedPropertyCount = 0;
  bool _entityCatalogDirty = false;
  // The input a host has just set: its callback runs and its value is sent after the ack.
  int _propertyJustSet = -1;
  int _registerProperty(byte deviceId, BlaeckString name, uint8_t kind, bool writable, dataType type,
                        void *address, void (*getter)(), uint8_t getterType, uint16_t textSize,
                        BlaeckString options, BlaeckPropertyCallback onChange);
  // Why name can't be used for a property or command, printed on the debug stream; false if it
  // can.
  bool _nameRefused(BlaeckString name, bool isCommand);
  static bool _hasControlChar(BlaeckString name);
  // Removes the inputs (writable) or the sensors, closing the gaps; see clearAllControls().
  void _clearProperties(bool writable);
  void _resetProperty(blaeck_detail::PropertyEntry &p);
  void _moveProperty(blaeck_detail::PropertyEntry &to, blaeck_detail::PropertyEntry &from);
  int _findProperty(BlaeckString name) const;
  // The current value: a number or bool into out (at most 8 bytes), or the text and whether it
  // is in flash.
  void _propertyValue(const PropertyEntry &p, byte *out) const;
  const char *_propertyText(const PropertyEntry &p, bool &inFlash) const;
  // Checks a host's value for the property, stores it and returns BLAECK_ACK_OK, or returns
  // why it was refused.
  byte _receiveProperty(uint16_t index);
  // Sends one property's value as 0x95. False if the frame didn't go out whole.
  bool _writePropertyFrame(uint16_t index);
  // Emits a property's value as 0x90 and 0x95 carry it.
  void _emitPropertyValue(const PropertyEntry &p);
  // Sends each property whose value changed, as writeOnChange() allows.
  void _writeChangedProperties();
  void _writePropertyByName(byte deviceId, BlaeckString name);
  void writeEntities(unsigned long messageID);
  void writeEntitiesFrame(unsigned long messageID);

  void (*_beforeWriteCallback)() = nullptr;
  void (*_dataStartCallback)() = nullptr;
  void (*_dataStopCallback)() = nullptr;
  void (*_intervalStartCallback)(uint32_t intervalMs) = nullptr;
  void (*_intervalStopCallback)() = nullptr;

  BlaeckTimestampMode _timestampMode = BLAECK_NO_TIMESTAMP;
  unsigned long long (*_timestampCallback)() = nullptr;

  union
  {
    bool val;
    byte bval[1];
  } boolCvt;

  union
  {
    short val;
    byte bval[2];
  } shortCvt;

  union
  {
    unsigned short val;
    byte bval[2];
  } ushortCvt;

  union
  {
    int val;
    byte bval[2];
  } intCvt;

  union
  {
    unsigned int val;
    byte bval[2];
  } uintCvt;

  union
  {
    long val;
    byte bval[4];
  } lngCvt;

  union
  {
    unsigned long val;
    byte bval[4];
  } ulngCvt;

  union
  {
    unsigned long long val;
    byte bval[8];
  } ullCvt;

  union
  {
    float val;
    byte bval[4];
  } fltCvt;

  union
  {
    double val;
    byte bval[8];
  } dblCvt;

  friend class BlaeckSignalRef;
  friend bool blaeck_detail::optionsAccepted(BlaeckString, Print *,
                                             const char *, bool);
  friend class BlaeckCommandRefBase;
  friend class BlaeckEventRef;
  friend class BlaeckPropertyRefBase;
  friend class BlaeckBeginRef;
  friend class BlaeckDeviceRef;
  friend class BlaeckDeviceBase;

private:
  // Copies an F() string into out, cut to fit and always terminated; returns the length.
  static byte copyFlashName(const __FlashStringHelper *flash, char *out, byte outSize);

  struct Connection
  {
    Receiver receiver;
    // The slot holds a connection. Tracked here because a client's own bool means
    // "connected" on some cores and "has a socket" on others.
    bool open = false;
  };
  static const byte NO_HOST = 255;

  // Allocated by the first read(), so withClients() on the begin() chain can size it.
  Connection *_connections = nullptr;
  Stream *_stream = nullptr;
  bool _tcpSelected = false;
  blaeck::detail::ServerAdapter *_adapter = nullptr;
  TransportError _transportError = TransportError::NotStarted;
  bool _transportErrorReported = false;
  bool _beginCalled = false;
  byte _maxClients = 4;
  // The connection whose command is being handled.
  byte _requester = 0;
  // The one connection that receives frames. The newest to send a built-in takes over.
  byte _hostSlot = NO_HOST;

  void (*_connectedCallback)(byte clientNo) = nullptr;
  void (*_disconnectedCallback)(byte clientNo) = nullptr;

  bool _ensureConnections();
  void _sendStreamBuffered();
  void _acceptConnection();
  void _dropClosedConnections();
  // Frees a slot. _announceDisconnect() reports it once the transport state is consistent.
  void _releaseConnection(byte slot);
  void _announceDisconnect(byte slot, const __FlashStringHelper *reason);
  bool _hostConnected() const;
  void _setMaxClients(byte count);
  void _setTransportError(TransportError error);
  bool _beginOnce();
  void _reportTransportError();

  friend class BlaeckTerminal;
};

// ----- BlaeckBeginRef bodies -----
// Defined here because they use Blaeck's private members. Documented at the
// declarations. (Keep the blank line below, or this comment becomes withClients()'s hover.)

inline BlaeckBeginRef &BlaeckBeginRef::withClients(byte count)
{
  if (_owner != nullptr)
    _owner->_setMaxClients(count);
  return *this;
}

inline BlaeckBeginRef &BlaeckBeginRef::withDebugStream(Print *debugStream)
{
  if (_owner != nullptr)
  {
    _owner->_debugStream = debugStream;
    _owner->_reportTransportError();
  }
  return *this;
}

// ----- Handle method bodies -----
// Defined here because they need Blaeck to be complete.

inline blaeck_detail::CommandHandlerEntry * BlaeckCommandRefBase::_entry() const
{
  if (_owner == nullptr || _index < 0)
    return nullptr;
  return &_owner->_commandHandlers[_index];
}

inline void BlaeckCommandRefBase::_markDirty() const
{
  if (_owner != nullptr)
    _owner->_entityCatalogDirty = true;
}

inline void BlaeckSignalRef::_setInterval(BlaeckIntervalMode mode, double delta)
{
  if (_owner != nullptr)
    _owner->_setSignalInterval(_index, mode, delta);
}

inline bool BlaeckCommandRefBase::_storeString(detail::StoredString &slot, BlaeckString value)
{
  return _owner != nullptr && _owner->_storeString(slot, value);
}

inline void BlaeckSignalRef::_setOnChange(double delta, uint32_t minIntervalMs)
{
  if (_owner != nullptr)
    _owner->_setSignalOnChange(_index, delta, minIntervalMs);
}

inline void BlaeckSignalRef::_setOnChange(BlaeckIntervalMode mode)
{
  if (_owner != nullptr)
    _owner->_setSignalOnChange(_index, mode);
}

inline void BlaeckSignalRef::_setNameSuffix(uint8_t suffix)
{
  if (_owner == nullptr || _index < 0 ||
      static_cast<unsigned int>(_index) >= _owner->Signals.capacity())
    return;
  Signal &s = _owner->Signals[_index];
  s.NameSuffix = suffix;
  s.HasSuffix = 1;
  // The suffix changes the name, and the name is part of the schema hash.
  _owner->_schemaHash = _owner->_computeSchemaHash();
}

inline BlaeckEventRef BlaeckEventRef::withIcon(BlaeckString icon)
{
#if BLAECK_ENABLE_IOT
  if (_index >= 0 && _owner != nullptr)
    // Only a real change marks the catalog.
    if (_owner->_eventChannels[_index].icon != icon)
    {
      if (!_owner->_storeString(_owner->_eventChannels[_index].icon, icon))
        return *this;
      _owner->_entityCatalogDirty = true;
    }
#else
  (void)icon;
  // Clang's -Wunused-private-field would otherwise fail -Werror builds with events disabled.
  (void)_owner;
  (void)_index;
#endif
  return *this;
}

inline BlaeckEventRef BlaeckEventRef::withDisplayName(BlaeckString displayName)
{
#if BLAECK_ENABLE_IOT
  if (_index >= 0 && _owner != nullptr)
    if (_owner->_eventChannels[_index].displayName != displayName)
    {
      if (!_owner->_storeString(_owner->_eventChannels[_index].displayName, displayName))
        return *this;
      _owner->_entityCatalogDirty = true;
    }
#else
  (void)displayName;
#endif
  return *this;
}

inline void BlaeckEventRef::_setCategory(uint8_t category, bool on)
{
#if BLAECK_ENABLE_IOT
  if (_index < 0 || _owner == nullptr)
    return;
  uint8_t &current = _owner->_eventChannels[_index].category;
  // Undoing a category the event doesn't have leaves it as it is.
  const uint8_t wanted = on ? category : (current == category ? 0 : current);
  if (current != wanted)
  {
    current = wanted;
    _owner->_entityCatalogDirty = true;
  }
#else
  (void)category;
  (void)on;
#endif
}

inline BlaeckEventRef BlaeckEventRef::config(bool on)
{
  _setCategory(1, on);
  return *this;
}

inline BlaeckEventRef BlaeckEventRef::diagnostic(bool on)
{
  _setCategory(2, on);
  return *this;
}

inline BlaeckEventRef BlaeckEventRef::withDeviceClass(BlaeckString deviceClass)
{
#if BLAECK_ENABLE_IOT
  if (_index >= 0 && _owner != nullptr)
    if (_owner->_eventChannels[_index].deviceClass != deviceClass)
    {
      if (!_owner->_storeString(_owner->_eventChannels[_index].deviceClass, deviceClass))
        return *this;
      _owner->_entityCatalogDirty = true;
    }
#else
  (void)deviceClass;
#endif
  return *this;
}

inline BlaeckEventRef BlaeckEventRef::disabledByDefault(bool on)
{
#if BLAECK_ENABLE_IOT
  if (_index >= 0 && _owner != nullptr)
    if (_owner->_eventChannels[_index].disabledByDefault != on)
    {
      _owner->_eventChannels[_index].disabledByDefault = on;
      _owner->_entityCatalogDirty = true;
    }
#else
  (void)on;
#endif
  return *this;
}

} // namespace blaeck

// Keep the sketch API available without namespace qualification.
using namespace blaeck;

#endif // BLAECK_H
