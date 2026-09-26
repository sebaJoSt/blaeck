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
// Paused writes
// -------------
// BLAECK.PAUSE_WRITES,<ms> holds back every frame for that long, so a host can disconnect
// while the board is quiet. On native-USB boards such as the Giga R1, closing the port
// mid-transmission kills the USB endpoint until the board is reset.
//
// The pause ends on its own; BLAECK.RESUME_WRITES ends it early. No duration, or 0, means
// the default, and longer requests are capped at the maximum, so a host that disappears
// cannot silence a board for long.
//
// BLAECK.PAUSE_WRITES,FOREVER has no cap and lasts until RESUME_WRITES or a reset.
#ifndef BLAECK_PAUSE_WRITES_DEFAULT_MS
  #define BLAECK_PAUSE_WRITES_DEFAULT_MS 1000UL
#endif

#ifndef BLAECK_PAUSE_WRITES_MAX_MS
  #define BLAECK_PAUSE_WRITES_MAX_MS 10000UL
#endif

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

// Table sizes (signals, commands, channels) are set in the sketch, on the begin() chain:
// device.begin(...).withCommands(16).

// The four switches below each remove one feature to save SRAM and flash on small boards.
// The API stays, so a sketch compiles either way, and the matching catalog request is
// answered with an empty frame so a host does not wait for it.

// Command metadata: what onNumberCommand() and the other typed commands declare, sent in
// the 0xA0 Command List. Off, the typed commands work like onCommand().
#ifndef BLAECK_ENABLE_COMMAND_META
  #define BLAECK_ENABLE_COMMAND_META 1
#endif

// Signal metadata: withUnit(), withIcon() and the rest of addSignal()'s handle, sent in the
// 0xF0 Signal Config. Off, those calls store nothing.
#ifndef BLAECK_ENABLE_SIGNAL_META
  #define BLAECK_ENABLE_SIGNAL_META 1
#endif

// State channels: addStateChannel() and writeState(), with the 0x90 State Channel List
// and 0x95 values. A typed command's withOwnState() uses a state channel too.
#ifndef BLAECK_ENABLE_STATE_CHANNELS
  #define BLAECK_ENABLE_STATE_CHANNELS 1
#endif

// Events: addEventChannel(), addEventType() and writeEvent(), with the 0x80 Event Channel
// List and 0x85 events.
#ifndef BLAECK_ENABLE_EVENTS
  #define BLAECK_ENABLE_EVENTS 1
#endif

// The built-in commands. read() matches against these names, and the build fails if one is
// too long for the parse buffer. A new built-in has to be added to the list below as well.
#define BLAECK_BUILTIN_WRITE_SYMBOLS "BLAECK.WRITE_SYMBOLS"
#define BLAECK_BUILTIN_WRITE_SIGNAL_CONFIG "BLAECK.WRITE_SIGNAL_CONFIG"
#define BLAECK_BUILTIN_WRITE_DATA "BLAECK.WRITE_DATA"
#define BLAECK_BUILTIN_GET_DEVICES "BLAECK.GET_DEVICES"
#define BLAECK_BUILTIN_WRITE_COMMANDS "BLAECK.WRITE_COMMANDS"
#define BLAECK_BUILTIN_WRITE_STATE_CHANNELS "BLAECK.WRITE_STATE_CHANNELS"
#define BLAECK_BUILTIN_WRITE_EVENT_CHANNELS "BLAECK.WRITE_EVENT_CHANNELS"
#define BLAECK_BUILTIN_ACTIVATE "BLAECK.ACTIVATE"
#define BLAECK_BUILTIN_DEACTIVATE "BLAECK.DEACTIVATE"
#define BLAECK_BUILTIN_PAUSE_WRITES "BLAECK.PAUSE_WRITES"
#define BLAECK_BUILTIN_RESUME_WRITES "BLAECK.RESUME_WRITES"

// The PAUSE_WRITES argument that means no time limit. Matched in capitals only.
#define BLAECK_PAUSE_WRITES_FOREVER "FOREVER"

// Sent as the device name when the sketch sets none.
#define BLAECK_DEVICE_NAME_UNNAMED "Unnamed"

#define BLAECK_BUILTIN_COMMAND_LIST(X)  \
  X(BLAECK_BUILTIN_WRITE_SYMBOLS)       \
  X(BLAECK_BUILTIN_WRITE_SIGNAL_CONFIG) \
  X(BLAECK_BUILTIN_WRITE_DATA)          \
  X(BLAECK_BUILTIN_GET_DEVICES)         \
  X(BLAECK_BUILTIN_WRITE_COMMANDS)      \
  X(BLAECK_BUILTIN_WRITE_STATE_CHANNELS)\
  X(BLAECK_BUILTIN_WRITE_EVENT_CHANNELS)\
  X(BLAECK_BUILTIN_ACTIVATE)            \
  X(BLAECK_BUILTIN_DEACTIVATE)          \
  X(BLAECK_BUILTIN_PAUSE_WRITES)        \
  X(BLAECK_BUILTIN_RESUME_WRITES)

// Longest select option a state channel can report, terminator included. A stack buffer
// used while a frame is built, not storage per channel.
#ifndef BLAECK_STATE_MAX_OPTION_CHARS
  #define BLAECK_STATE_MAX_OPTION_CHARS 24
#endif

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
  Blaeck_string
} dataType;

// The enumerators are not the wire codes and nothing treats them as such: _dtypeCode() is
// the one mapping, and the schema hash, the symbol list and the state frames all go through
// it. Reorder this list or insert a type and nothing on the wire moves.

// Type tags for addStateChannel(), for a channel with no variable behind it. They are separate
// types rather than one enum because the tag picks which handle comes back, so withUnit() on a
// text channel fails to compile.
struct BlaeckTextTag
{
};
struct BlaeckBoolTag
{
};
struct BlaeckNumericTag
{
  /*!
    @brief   Which numeric type the tag names.

    @code
      device.addStateChannel(F("Temperature"), BlaeckFloat);
    @endcode
  */
  dataType t;
};

constexpr BlaeckTextTag BlaeckText{};
constexpr BlaeckBoolTag BlaeckBool{};
constexpr BlaeckNumericTag BlaeckByte{Blaeck_byte};
constexpr BlaeckNumericTag BlaeckShort{Blaeck_short};
constexpr BlaeckNumericTag BlaeckUShort{Blaeck_ushort};
constexpr BlaeckNumericTag BlaeckInt{Blaeck_int};
constexpr BlaeckNumericTag BlaeckUInt{Blaeck_uint};
constexpr BlaeckNumericTag BlaeckLong{Blaeck_long};
constexpr BlaeckNumericTag BlaeckULong{Blaeck_ulong};
constexpr BlaeckNumericTag BlaeckFloat{Blaeck_float};
constexpr BlaeckNumericTag BlaeckDouble{Blaeck_double};

// How a signal's value behaves over time, so a host knows whether to keep statistics on it.
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

// The 0xF0 flag word, stored as it is sent. A word of zero means the signal declares nothing.
enum BlaeckSignalMetaFlag
{
  BLAECK_SIG_HAS_UNIT = 0x0001,
  BLAECK_SIG_HAS_DEVICE_CLASS = 0x0002,
  BLAECK_SIG_HAS_ICON = 0x0004,
  BLAECK_SIG_STATE_CLASS_MASK = 0x0038, // bits 3-5
  BLAECK_SIG_DIAGNOSTIC = 0x0040,
  BLAECK_SIG_DISABLED_BY_DEFAULT = 0x0080,
  BLAECK_SIG_FORCE_UPDATE = 0x0100,
  BLAECK_SIG_HAS_DISPLAY_PRECISION = 0x0200,
  BLAECK_SIG_HAS_OPTIONS = 0x0400,
  BLAECK_SIG_HAS_DISPLAY_NAME = 0x0800
};
static const byte BLAECK_SIG_STATE_CLASS_SHIFT = 3;

// The 0x90 flag word. Laid out differently from the signal flags above, so the names are
// separate. A text channel never sets unit, state class or display precision.
enum BlaeckStateChannelFlag
{
  BLAECK_SCH_HAS_ICON = 0x0001,
  BLAECK_SCH_DIAGNOSTIC = 0x0002,
  BLAECK_SCH_HAS_STATE_VALUE = 0x0004,
  BLAECK_SCH_HAS_DEVICE_CLASS = 0x0008,
  BLAECK_SCH_DISABLED_BY_DEFAULT = 0x0010,
  BLAECK_SCH_FORCE_UPDATE = 0x0020,
  BLAECK_SCH_HAS_OPTIONS = 0x0040,
  BLAECK_SCH_HAS_UNIT = 0x0080,
  BLAECK_SCH_STATE_CLASS_MASK = 0x0700, // bits 8-10
  BLAECK_SCH_HAS_DISPLAY_PRECISION = 0x0800
};
static const byte BLAECK_SCH_STATE_CLASS_SHIFT = 8;

#if BLAECK_ENABLE_SIGNAL_META
// What a signal declares beyond its name and type. Allocated only for signals that declare
// something, since most don't and this is larger than the signal entry itself.
struct SignalMeta
{
  detail::StoredString Unit;
  detail::StoredString DeviceClass;
  detail::StoredString Icon;
  detail::StoredString Options;
  detail::StoredString DisplayName;
  uint16_t MetaFlags = 0;
  uint8_t DisplayPrecision = 0;
};
#endif

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

struct SignalReporting
{
  byte value[sizeof(double) > sizeof(unsigned long) ? sizeof(double) : sizeof(unsigned long)] = {};
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
  ~SignalReporting() { delete[] text; }
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
  SignalReporting *Reporting = nullptr;
#if BLAECK_ENABLE_SIGNAL_META
  // Null until the sketch describes the signal. Owned by the entry.
  SignalMeta *Meta = nullptr;
#endif
};

enum BlaeckTimestampMode
{
  BLAECK_NO_TIMESTAMP = 0,
  BLAECK_MICROS = 1,
  BLAECK_UNIX = 2,
  BLAECK_RTC = BLAECK_UNIX // Deprecated alias
};

// paramCount is 0 only for a plain command or a button. A typed command that arrives without
// its value is rejected before any handler runs.
typedef void (*BlaeckCommandHandler)(const char *command, const char *const *params, byte paramCount);
typedef void (*BlaeckAnyCommandHandler)(const char *command, const char *const *params, byte paramCount);

// Returns a text state channel's current value when it is sent. It runs while a frame is
// being built, so it should return quickly. The text is copied at once, so a static local
// buffer is fine. nullptr means no value right now.
typedef const char *(*BlaeckStateTextGetter)();

// The same for numeric channels, one type per getter.
typedef bool (*BlaeckStateBoolGetter)();
typedef byte (*BlaeckStateByteGetter)();
typedef short (*BlaeckStateShortGetter)();
typedef unsigned short (*BlaeckStateUShortGetter)();
typedef int (*BlaeckStateIntGetter)();
typedef unsigned int (*BlaeckStateUIntGetter)();
typedef long (*BlaeckStateLongGetter)();
typedef unsigned long (*BlaeckStateULongGetter)();
typedef float (*BlaeckStateFloatGetter)();
typedef double (*BlaeckStateDoubleGetter)();

// What kind of control a command is, as listed in the command catalog.
enum BlaeckCommandKind
{
  BLAECK_CMD_PLAIN = 0,  // onCommand(): listed, but not offered as a control
  BLAECK_CMD_NUMBER = 1, // a value in [min, max]
  BLAECK_CMD_SWITCH = 2, // 0 or 1
  BLAECK_CMD_SELECT = 3, // one of a list of options
  BLAECK_CMD_BUTTON = 4, // no value
  BLAECK_CMD_TEXT = 5    // free text, percent-encoded in transit
};

// Where a host files a command's control. CONFIG is a device setting, DIAGNOSTIC fits only a
// button such as identify or self-test. Either keeps the control off a host's default views.
enum BlaeckEntityCategory
{
  BLAECK_CAT_NONE = 0,      // a main control (default)
  BLAECK_CAT_CONFIG = 1,
  BLAECK_CAT_DIAGNOSTIC = 2
};

// How a host should show a number command's input. Only a hint; the range still bounds it.
enum BlaeckNumberMode
{
  BLAECK_NUMBER_MODE_AUTO = 0,   // the host decides (default)
  BLAECK_NUMBER_MODE_BOX = 1,    // a typed field
  BLAECK_NUMBER_MODE_SLIDER = 2
};

// How a host should show a text command's input. PASSWORD only masks the field on screen; the
// value still travels as plain text.
enum BlaeckTextMode
{
  BLAECK_TEXT_MODE_PLAIN = 0,   // default
  BLAECK_TEXT_MODE_PASSWORD = 1
};

// The longest text value a host is expected to accept. withMaxLength() warns above it, because
// a host may reject the whole control rather than shorten it.
#define BLAECK_TEXT_MAX_LENGTH 255

// What a typed command's state name refers to. Set by the library from how the command was
// declared.
enum BlaeckStateSource
{
  BLAECK_STATE_SIGNAL = 0, // an addSignal() signal
  BLAECK_STATE_CHANNEL = 1 // a state channel the command owns (see withOwnState())
};

// Why a command was accepted or rejected, sent back to the host after each command.
enum BlaeckCommandAckReason
{
  BLAECK_ACK_OK = 0,            // accepted
  BLAECK_ACK_UNKNOWN = 1,       // no handler for it
  BLAECK_ACK_OUT_OF_RANGE = 2,  // number outside [min, max]
  BLAECK_ACK_BAD_SWITCH = 3,    // switch value not 0 or 1
  BLAECK_ACK_BAD_SELECT = 4,    // not one of the select's options
  BLAECK_ACK_TOO_LONG = 5,      // text longer than its declared maximum
  BLAECK_ACK_MISSING_VALUE = 6, // a typed command without its value
  BLAECK_ACK_TRUNCATED = 7,     // too long or too many parameters to receive whole
  BLAECK_ACK_DEVICE_NOT_RESPONDING = 8 // its device from addDevice() is marked missing
};

// Warns when a call's return value is ignored. onNumberCommand() and onSelectCommand() use it,
// because dropping their handle skips the required withRange() or withOptions().
#if defined(__GNUC__)
#define BLAECK_NODISCARD __attribute__((warn_unused_result))
#else
#define BLAECK_NODISCARD
#endif

class BlaeckSignalRefBase;
class BlaeckNumericSignalRef;
class BlaeckTextSignalRef;
class BlaeckBoolSignalRef;
class BlaeckCommandRefBase;
class BlaeckNumberCommandRef;
class BlaeckNumberCommandNeedsRange;
class BlaeckSwitchCommandRef;
class BlaeckSelectCommandRef;
class BlaeckSelectCommandNeedsOptions;
class BlaeckButtonCommandRef;
class BlaeckTextCommandRef;
class BlaeckStateRefBase;
class BlaeckNumericStateRef;
class BlaeckTextStateRef;
class BlaeckBoolStateRef;
class BlaeckEventChannelRef;
class BlaeckDeviceRef;
class Blaeck;

// Returned by begin() to set table sizes and connection limits, e.g.
//
//   device.begin(...).withSignals(50).withStateChannels(12);
//
// Each table starts from a default that suits the board, and is allocated when its first
// entry is added, so an unused table costs nothing. Changing a size after that table exists
// is refused. The state and event calls still compile when those features are switched off.
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
    limit is closed at once. Set it before the first read(); later it is refused.

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
    @brief   Sets how many signals fit in the signal table.

    Each signal takes 12 bytes of SRAM on AVR, and a signal with a unit, icon or
    other description 18 more.

    @param   count  Up to 32767. A larger literal fails the build.
    @return  The same handle, for chaining.

    @code
      device.begin(Serial).withSignals(50);
    @endcode
  */
  BlaeckBeginRef &withSignals(unsigned int count);

  /*!
    @brief   Sets how many state channels fit in the state channel table.

    Count the channels from addStateChannel() plus one for each command that uses
    withOwnState(). Each channel takes 35 bytes of SRAM on AVR, plus any RAM strings
    it copies.

    @param   count  Up to 32767. A larger literal fails the build.
    @return  The same handle, for chaining.

    @code
      device.begin(Serial).withStateChannels(12);
    @endcode
  */
  BlaeckBeginRef &withStateChannels(unsigned int count);

  /*!
    @brief   Sets how many event channels fit in the event channel table.

    Each channel takes 13 bytes of SRAM on AVR, plus any RAM strings it copies.

    @param   count  Up to 32767. A larger literal fails the build.
    @return  The same handle, for chaining.

    @code
      device.begin(Serial).withEventChannels(4);
    @endcode
  */
  BlaeckBeginRef &withEventChannels(unsigned int count);

  /*!
    @brief   Sets how many event types fit, counted across all channels.

    All channels share one table of types, so give the total: four channels with
    five types each need 20. Each type takes 6 bytes of SRAM on AVR, plus the copy
    of a type list given as a RAM string.

    @param   count  Up to 32767. A larger literal fails the build.
    @return  The same handle, for chaining.

    @code
      device.begin(Serial).withEventChannels(4).withEventTypes(20);
    @endcode
  */
  BlaeckBeginRef &withEventTypes(unsigned int count);

  /*!
    @brief   Sets how many commands fit in the command table.

    onCommand() and all the typed commands share this table. Each command takes 66
    bytes of SRAM on AVR, plus any RAM strings it copies. A command using
    withOwnState() also needs a state channel, so raise withStateChannels() to match.

    @param   count  Up to 32767. A larger literal fails the build.
    @return  The same handle, for chaining.

    @code
      device.begin(Serial).withCommands(8);
    @endcode
  */
  BlaeckBeginRef &withCommands(unsigned int count);

  /*!
    @brief   Sets how many devices addDevice() can add.

    Each device takes 13 bytes of SRAM on AVR, plus any RAM strings it copies.

    @param   count  Up to 254.
    @return  The same handle, for chaining.

    @code
      device.begin(Serial).withDevices(2);
    @endcode
  */
  BlaeckBeginRef &withDevices(unsigned int count);

  /*!
    @brief   Sets a stream where the library reports what it rejected and why.

    Without one, problems such as a full table show only in hasRejections().

    @param   debugStream  Where to print: a serial port, or anything else that can print,
                          such as a display.
    @return  The same handle, for chaining.

    @code
      device.begin(Serial).withSignals(50).withDebugStream(&Serial);
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
// A literal index 0 must not compete with null-name forwarding overloads.
template<class T> struct NullName {};
template<> struct NullName<decltype(nullptr)> { using type = void; };

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

// Checks an options list for withOptions(): it must have at least one entry and no blank
// ones. Prints why on debug when it refuses. `name` is the signal, channel or command named
// in that message.
bool optionsAccepted(BlaeckString optionsCsv, Print *debug,
                     const char *name, bool nameInFlash);

// Checks that a channel can take a getter: it must not already read a variable, and the
// getter must return the channel's type.
bool stateGetterAccepted(const void *stateValue, dataType want, dataType have,
                         const __FlashStringHelper *method, Print *debug,
                         const char *name, bool nameInFlash);

// Turns what a switch's getter returned into "1" or "0". Accepts 1/on/true/yes and
// 0/off/false/no in any case, and returns nullptr for anything else.
inline const char *switchStateText(const char *value)
{
  if (value == nullptr)
    return nullptr;

  // Lowercase into a buffer that fits the longest word; anything longer can't match.
  // strcasecmp is missing on some cores.
  char w[6];
  byte n = 0;
  while (value[n] != '\0')
  {
    if (n >= sizeof(w) - 1)
      return nullptr;
    char c = value[n];
    w[n] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    n++;
  }
  w[n] = '\0';

  // Static, so the returned pointer stays valid after the call.
  static const char kOn[] = "1";
  static const char kOff[] = "0";

  switch (n)
  {
  case 1:
    if (w[0] == '1') return kOn;
    if (w[0] == '0') return kOff;
    break;
  case 2:
    if (w[0] == 'o' && w[1] == 'n') return kOn;
    if (w[0] == 'n' && w[1] == 'o') return kOff;
    break;
  case 3:
    if (w[0] == 'y' && w[1] == 'e' && w[2] == 's') return kOn;
    if (w[0] == 'o' && w[1] == 'f' && w[2] == 'f') return kOff;
    break;
  case 4:
    if (w[0] == 't' && w[1] == 'r' && w[2] == 'u' && w[3] == 'e') return kOn;
    break;
  case 5:
    if (w[0] == 'f' && w[1] == 'a' && w[2] == 'l' && w[3] == 's' && w[4] == 'e') return kOff;
    break;
  }
  return nullptr;
}

struct CommandHandlerEntry
{
  char command[MAX_COMMAND_NAME_COUNT];
  BlaeckCommandHandler handler = nullptr;
  bool inUse = false;
  // The device from addDevice() the command belongs to, 0 for the board itself.
  uint8_t deviceId = 0;
#if BLAECK_ENABLE_COMMAND_META
  uint8_t kind = BLAECK_CMD_PLAIN;
  float meta_min = 0.0f;
  float meta_max = 0.0f;
  float meta_step = 0.0f;
  detail::StoredString unit;
  detail::StoredString deviceClass;
  detail::StoredString icon;
  detail::StoredString displayName;
  detail::StoredString options;
  detail::StoredString stateSignal;
  // Buttons only: the arguments a press sends, or nullptr for none.
  detail::StoredString pressPayload;
  uint8_t stateSource = BLAECK_STATE_SIGNAL;
  uint8_t category = BLAECK_CAT_NONE;
  bool disabledByDefault = false;
  // BlaeckNumberMode on a number command, BlaeckTextMode on a text command.
  uint8_t mode = 0;
#endif
};

struct StateChannelEntry
{
  // A heap copy, or a flash pointer when nameInFlash. Read it through the helpers only.
  const char *name = nullptr;
  bool nameInFlash = false;
  detail::StoredString icon;
  // A getter asked for the value each time it is sent. valueType says which member is in
  // use: getStateText for text, getNumber (cast to the right type) for everything else.
  union
  {
    BlaeckStateTextGetter getStateText = nullptr;
    void (*getNumber)();
  };
  detail::StoredString deviceClass;
  detail::StoredString options;
  detail::StoredString unit;
  // The variable the channel reports, when it has one instead of a getter.
  const void *stateValue = nullptr;
  dataType valueType = Blaeck_string;
  // State class and the has-display-precision bit, which no other member can hold. The rest
  // of the flag word is built from the members when the catalog is written.
  uint16_t metaFlags = 0;
  uint8_t displayPrecision = 0;
  // The channel belongs to a command's withOwnState(). addStateChannel() and writeState()
  // refuse its name.
  bool ownedByCommand = false;
  bool diagnostic = false;
  bool disabledByDefault = false;
  bool forceUpdate = false;
  // stateValue points at a byte holding a select's option index, and the channel reports
  // that option's name.
  bool stateIsSelectIndex = false;
  // The channel reports an option name directly; it is checked against the options list.
  bool stateIsSelectName = false;
  // The getter belongs to a switch, so its text is turned into "1" or "0".
  bool stateIsSwitchBool = false;
  // Set after the first warning about a value that can't be reported, so it prints once.
  // Mutable because the entry is read through a const reference when the value is fetched.
  mutable bool stateWarned = false;
  // Set after the first warning about text cut to 255 bytes.
  bool truncationWarned = false;
  bool textInFlash = false;
  bool inUse = false;
  // The device from addDevice() the channel belongs to, 0 for the board itself.
  uint8_t deviceId = 0;
};

struct EventChannelEntry
{
  // A heap copy, or a flash pointer when nameInFlash. Read it through the helpers only.
  const char *name = nullptr;
  bool nameInFlash = false;
  detail::StoredString icon;
  detail::StoredString deviceClass;
  bool diagnostic = false;
  bool disabledByDefault = false;
  bool inUse = false;
  // The device from addDevice() the channel belongs to, 0 for the board itself.
  uint8_t deviceId = 0;
};

// A device added with addDevice(). Its slave ID in the protocol is its index plus one.
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

// The shared part of the handles returned by the typed command registrations. Each kind's
// handle exposes only the modifiers that make sense for it, so a range on a text command is
// a compile error. A rejected registration returns a handle that ignores every call.
// With BLAECK_ENABLE_COMMAND_META=0 the modifiers store nothing.
class BlaeckCommandRefBase
{
protected:
  BlaeckCommandRefBase(Blaeck *owner, int16_t index) : _owner(owner), _index(index) {}

  // The entry this handle names, or nullptr when registration was rejected.
  blaeck_detail::CommandHandlerEntry * _entry() const;

  // Marks the command catalog as changed, so it is sent again.
  void _markDirty() const;

  void _setStateSignal(BlaeckString signalName)
  {
#if BLAECK_ENABLE_COMMAND_META
    if (auto *e = _entry())
    {
      // Only a real change marks the catalog. The modifiers here and below may be called
      // on every loop() pass, and each mark resends the catalog.
      if (e->stateSignal != signalName || e->stateSource != BLAECK_STATE_SIGNAL)
      {
        if (!_storeString(e->stateSignal, signalName))
          return;
        e->stateSource = BLAECK_STATE_SIGNAL;
        _markDirty();
      }
    }
#else
    (void)signalName;
#endif
  }

  // Gives the command a state channel of its own, reading a variable or a getter.
  void _setOwnState(BlaeckString channelName, dataType valueType, const void *value,
                    bool selectIndex = false);

  void _setOwnState(BlaeckString channelName, BlaeckStateTextGetter getStateText);

  // A max not above min means no range, so nothing would be checked. Say so.
  void _warnRangeIgnored(float mn, float mx) const;

  // A step that isn't positive (or is NaN) means no step. Say so.
  void _warnStepIgnored(float st) const;

  // A step below 0.001 is sent, but a host may reject the whole control for it.
  void _warnStepTooFine(float st) const;

  // A maximum length above BLAECK_TEXT_MAX_LENGTH is refused, not clamped.
  void _warnMaxLengthTooLong(unsigned int maxLength) const;

  // Refuses an options list with no entries or a blank one.
  bool _optionsAccepted(BlaeckString optionsCsv) const;

  void _setRange(float mn, float mx, float st)
  {
#if BLAECK_ENABLE_COMMAND_META
    if (auto *e = _entry())
    {
      if (e->meta_min != mn || e->meta_max != mx || e->meta_step != st)
      {
        e->meta_min = mn;
        e->meta_max = mx;
        e->meta_step = st;
        _markDirty();
      }
      if (!(mx > mn))
        _warnRangeIgnored(mn, mx);
      // A step of 0 means none, on purpose.
      if (st != 0.0f && !(st > 0.0f))
        _warnStepIgnored(st);
      else if (st > 0.0f && st < 0.001f)
        _warnStepTooFine(st);
    }
#else
    (void)mn; (void)mx; (void)st;
#endif
  }

  void _setUnit(BlaeckString unit)
  {
#if BLAECK_ENABLE_COMMAND_META
    if (blaeck_detail::flashStrEmpty(unit))
      unit = nullptr;
    if (auto *e = _entry())
    {
      if (e->unit != unit)
      {
        if (!_storeString(e->unit, unit))
          return;
        _markDirty();
      }
    }
#else
    (void)unit;
#endif
  }

  // An empty device class is treated as none; a host may reject a blank one.
  void _setDeviceClass(BlaeckString deviceClass)
  {
#if BLAECK_ENABLE_COMMAND_META
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
#if BLAECK_ENABLE_COMMAND_META
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

  void _setPressPayload(BlaeckString pressPayload)
  {
#if BLAECK_ENABLE_COMMAND_META
    if (blaeck_detail::flashStrEmpty(pressPayload))
      pressPayload = nullptr;
    if (auto *e = _entry())
    {
      if (e->pressPayload != pressPayload)
      {
        if (!_storeString(e->pressPayload, pressPayload))
          return;
        _markDirty();
      }
    }
#else
    (void)pressPayload;
#endif
  }

  void _setDisplayName(BlaeckString displayName)
  {
#if BLAECK_ENABLE_COMMAND_META
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

  void _setOptions(BlaeckString optionsCsv)
  {
#if BLAECK_ENABLE_COMMAND_META
    if (auto *e = _entry())
    {
      // A refused list leaves whatever the entry had before.
      if (!_optionsAccepted(optionsCsv))
        return;
      if (e->options != optionsCsv)
      {
        if (!_storeString(e->options, optionsCsv))
          return;
        _markDirty();
      }
    }
#else
    (void)optionsCsv;
#endif
  }

  // Stored in meta_max, which a text command uses for nothing else.
  void _setMaxLength(unsigned int maxLength)
  {
#if BLAECK_ENABLE_COMMAND_META
    if (auto *e = _entry())
    {
      // The entry keeps the length it was registered with.
      if (maxLength > BLAECK_TEXT_MAX_LENGTH)
      {
        _warnMaxLengthTooLong(maxLength);
        return;
      }
      if (e->meta_max != (float)maxLength)
      {
        e->meta_max = (float)maxLength;
        _markDirty();
      }
    }
#else
    (void)maxLength;
#endif
  }

  void _setCategory(uint8_t category)
  {
#if BLAECK_ENABLE_COMMAND_META
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
#if BLAECK_ENABLE_COMMAND_META
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

  // Only number and text handles offer withMode(), so the kind needs no check here.
  void _setMode(uint8_t mode)
  {
#if BLAECK_ENABLE_COMMAND_META
    if (auto *e = _entry())
    {
      if (e->mode != mode)
      {
        e->mode = mode;
        _markDirty();
      }
    }
#else
    (void)mode;
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

    Only the label changes. When the control is used, the host still sends the
    command name, such as SET_FREQ, so adding a label later breaks nothing.

    @param   displayName  The label.
    @return  The same handle, for chaining.

    @code
      device.onNumberCommand("SET_FREQ", onSetFreq)
          .withRange(0.0f, 2.0f, 0.01f)
          .withDisplayName(F("Frequency"));
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
      device.onButtonCommand("CALIBRATE", onCalibrate).withIcon(F("mdi:tune"));
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
      device.onTextCommand("SET_LABEL", onSetLabel).withMaxLength(32).config();
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
      device.onButtonCommand("REBOOT", onReboot).diagnostic();
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
      device.onButtonCommand("CALIBRATE", onCalibrate).disabledByDefault();
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

// The state modifiers, for every kind except a button, which has no state to report.
template <class TYPE>
class BlaeckCommandRefStateful : public BlaeckCommandRefShared<TYPE>
{
public:
  /*!
    @brief   Reports the command's current value through an existing signal.

    A host then shows what the device actually holds, not just what was last sent.
    Use this when the value should be logged with the data; use withOwnState() when
    it shouldn't.

    @param   signalName  A signal added with addSignal().
    @return  The same handle, for chaining.

    @code
      device.addSignal(F("LED_State"), &ledState);
      device.onSwitchCommand("LED", onLED).withStateFromSignal(F("LED_State"));
    @endcode
  */
  TYPE &withStateFromSignal(BlaeckString signalName)
  {
    this->_setStateSignal(signalName);
    return this->_self();
  }

  /*!
    @brief   Reports the command's current value on a state channel of its own, read
             from a getter.

    For a value that shouldn't be logged. The channel belongs to the command, so
    addStateChannel() and writeState() refuse its name. Call writeCommandState()
    after a change; otherwise the value is sent only when a host asks.

    @param   channelName   Name of the channel. It takes a slot in withStateChannels().
    @param   getStateText  Returns the current value as text.
    @return  The same handle, for chaining.

    @warning The getter runs while a frame is being built, so it must not write a
             frame itself, for example by calling writeState().

    @code
      device.onNumberCommand("SET_OFFSET", onSetOffset)
          .withRange(-100.0f, 100.0f, 0.1f)
          .withOwnState(F("Offset"), offsetText);
    @endcode
  */
  TYPE &withOwnState(BlaeckString channelName, BlaeckStateTextGetter getStateText)
  {
    this->_setOwnState(channelName, getStateText);
    return this->_self();
  }

protected:
  BlaeckCommandRefStateful(Blaeck *owner, int16_t index) : BlaeckCommandRefShared<TYPE>(owner, index) {}
};

class BlaeckNumberCommandRef : public BlaeckCommandRefStateful<BlaeckNumberCommandRef>
{
public:
  BlaeckNumberCommandRef(Blaeck *owner, int16_t index) : BlaeckCommandRefStateful<BlaeckNumberCommandRef>(owner, index) {}

  // Otherwise the withOwnState() below would hide the inherited getter form.
  using BlaeckCommandRefStateful<BlaeckNumberCommandRef>::withOwnState;

  /*!
    @brief   Sets the unit a host shows next to the input.

    Only a label; the handler gets the number as sent.

    @param   unit  The unit. Non-ASCII characters must be UTF-8:
                   F("\xC2\xB0" "C") is degrees Celsius.
    @return  The same handle, for chaining.

    @code
      device.onNumberCommand("SET_FREQ", onSetFreq).withRange(0.0f, 2.0f, 0.01f).withUnit(F("Hz"));
    @endcode
  */
  BlaeckNumberCommandRef &withUnit(BlaeckString unit)
  {
    _setUnit(unit);
    return *this;
  }

  /*!
    @brief   Asks a host to show the input as a typed box or a slider.

    Only a hint. Without it the host chooses, which suits most controls.

    @param   mode  BLAECK_NUMBER_MODE_BOX or BLAECK_NUMBER_MODE_SLIDER.
                   BLAECK_NUMBER_MODE_AUTO is the default.
    @return  The same handle, for chaining.

    @note    A slider can send values like 21.200000000000003 for a step of 0.1.
             The library doesn't round them, so round in the handler if it matters.

    @code
      device.onNumberCommand("SET_FREQ", onSetFreq)
          .withRange(0.0f, 2.0f, 0.01f)
          .withMode(BLAECK_NUMBER_MODE_BOX);
    @endcode
  */
  BlaeckNumberCommandRef &withMode(BlaeckNumberMode mode)
  {
    _setMode((uint8_t)mode);
    return *this;
  }

  /*!
    @brief   Sets what kind of quantity the control sets, such as "temperature".

    A host uses it for the icon, and may show the value in the user's own units.
    Any conversion happens in the host, so values always reach the device in the
    unit declared with withUnit(), and the range stays in that unit too.

    @param   deviceClass  A number device class in lower case:
                          "temperature", "pressure", "power", "voltage" and so on.
                          Numbers don't take "enum", "timestamp" or "date".
    @return  The same handle, for chaining.

    @warning A class the host doesn't know makes it drop the control, so leave it
             out rather than guess. Declare the unit too; a converting class
             without one gives wrong values.

    @code
      device.onNumberCommand("SET_TEMP", onSetTemp)
          .withRange(5.0f, 30.0f, 0.5f)
          .withUnit(F("\xC2\xB0" "C"))
          .withDeviceClass(F("temperature"));
    @endcode
  */
  BlaeckNumberCommandRef &withDeviceClass(BlaeckString deviceClass)
  {
    _setDeviceClass(deviceClass);
    return *this;
  }

  /*!
    @brief   Reports the command's current value on a state channel of its own, read
             from a numeric variable.

    There is an overload for each numeric type.

    @param   channelName  Name of the channel. It takes a slot in withStateChannels().
    @param   value        The variable to read. It must outlive the sketch, so use a
                          global.
    @return  The same handle, for chaining.

    @code
      device.onNumberCommand("SET_AMP", onSetAmp)
          .withRange(0.0f, 100.0f, 0.1f)
          .withOwnState(F("Amplitude"), &Amplitude);
    @endcode
  */
  BlaeckNumberCommandRef &withOwnState(BlaeckString channelName, byte *value)
  {
    _setOwnState(channelName, Blaeck_byte, value);
    return *this;
  }

  // Reports a short variable as the command's state.
  BlaeckNumberCommandRef &withOwnState(BlaeckString channelName, short *value)
  {
    _setOwnState(channelName, Blaeck_short, value);
    return *this;
  }

  // Reports an unsigned short variable as the command's state.
  BlaeckNumberCommandRef &withOwnState(BlaeckString channelName, unsigned short *value)
  {
    _setOwnState(channelName, Blaeck_ushort, value);
    return *this;
  }

  // Reports an int variable as the command's state. An int is 16-bit on AVR, 32-bit elsewhere.
  BlaeckNumberCommandRef &withOwnState(BlaeckString channelName, int *value)
  {
#ifdef __AVR__
    _setOwnState(channelName, Blaeck_int, value);
#else
    _setOwnState(channelName, Blaeck_long, value);
#endif
    return *this;
  }

  // Reports an unsigned int variable as the command's state. 16-bit on AVR, 32-bit elsewhere.
  BlaeckNumberCommandRef &withOwnState(BlaeckString channelName, unsigned int *value)
  {
#ifdef __AVR__
    _setOwnState(channelName, Blaeck_uint, value);
#else
    _setOwnState(channelName, Blaeck_ulong, value);
#endif
    return *this;
  }

  // Reports a long variable as the command's state.
  BlaeckNumberCommandRef &withOwnState(BlaeckString channelName, long *value)
  {
    _setOwnState(channelName, Blaeck_long, value);
    return *this;
  }

  // Reports an unsigned long variable as the command's state.
  BlaeckNumberCommandRef &withOwnState(BlaeckString channelName, unsigned long *value)
  {
    _setOwnState(channelName, Blaeck_ulong, value);
    return *this;
  }

  // Reports a float variable as the command's state.
  BlaeckNumberCommandRef &withOwnState(BlaeckString channelName, float *value)
  {
    _setOwnState(channelName, Blaeck_float, value);
    return *this;
  }

  // Reports a double variable as the command's state. On AVR a double is a 4-byte float and
  // is sent as one.
  BlaeckNumberCommandRef &withOwnState(BlaeckString channelName, double *value)
  {
#ifdef __AVR__
    _setOwnState(channelName, Blaeck_float, value);
#else
    _setOwnState(channelName, Blaeck_double, value);
#endif
    return *this;
  }
};

class BlaeckSwitchCommandRef : public BlaeckCommandRefStateful<BlaeckSwitchCommandRef>
{
public:
  BlaeckSwitchCommandRef(Blaeck *owner, int16_t index) : BlaeckCommandRefStateful<BlaeckSwitchCommandRef>(owner, index) {}

  // Otherwise the withOwnState() below would hide the inherited getter form. On a switch, the
  // getter's text is read as on (1/on/true/yes) or off (0/off/false/no) in any case, and
  // anything else reports no value.
  using BlaeckCommandRefStateful<BlaeckSwitchCommandRef>::withOwnState;

  /*!
    @brief   Says whether the switch controls a mains socket or something else.

    Changes only the icon and wording a host uses. Most switches don't need it.

    @param   deviceClass  "outlet" for a socket, "switch" for anything else. No other
                          values are valid for a switch.
    @return  The same handle, for chaining.

    @code
      device.onSwitchCommand("SET_RELAY", onSetRelay).withDeviceClass(F("outlet"));
    @endcode
  */
  BlaeckSwitchCommandRef &withDeviceClass(BlaeckString deviceClass)
  {
    _setDeviceClass(deviceClass);
    return *this;
  }

  /*!
    @brief   Reports the switch's current position on a state channel of its own,
             read from a bool.

    @param   channelName  Name of the channel. It takes a slot in withStateChannels().
    @param   value        The bool to read. Use a global.
    @return  The same handle, for chaining.

    @code
      device.onSwitchCommand("SET_ENABLE", onSetEnable)
          .withOwnState(F("Enabled"), &Enabled);
    @endcode
  */
  BlaeckSwitchCommandRef &withOwnState(BlaeckString channelName, bool *value)
  {
    _setOwnState(channelName, Blaeck_bool, value);
    return *this;
  }
};

class BlaeckSelectCommandRef : public BlaeckCommandRefStateful<BlaeckSelectCommandRef>
{
public:
  BlaeckSelectCommandRef(Blaeck *owner, int16_t index) : BlaeckCommandRefStateful<BlaeckSelectCommandRef>(owner, index) {}

  // Otherwise the withOwnState() below would hide the inherited getter form.
  using BlaeckCommandRefStateful<BlaeckSelectCommandRef>::withOwnState;

  /*!
    @brief   Reports the selected option on a state channel of its own, read from an
             index variable.

    The library looks the index up in the options list and sends the option's name.
    This is usually the same variable the handler sets.

    @param   channelName  Name of the channel. It takes a slot in withStateChannels().
    @param   index        The index variable, counting from 0. Use a global.
    @return  The same handle, for chaining.

    @code
      device.onSelectCommand("SET_WAVE", onSetWave)
          .withOptions(F("Sine,Square,Triangle,Sawtooth"))
          .withOwnState(F("Wave"), &waveIndex);
    @endcode
  */
  BlaeckSelectCommandRef &withOwnState(BlaeckString channelName, byte *index)
  {
    _setOwnState(channelName, Blaeck_string, index, true);
    return *this;
  }

  // Reports the selected option from a buffer holding its name. The sketch keeps the buffer
  // up to date; the name is checked against the options list.
  BlaeckSelectCommandRef &withOwnState(BlaeckString channelName, const char *value)
  {
    _setOwnState(channelName, Blaeck_string, value);
    return *this;
  }

};

class BlaeckButtonCommandRef : public BlaeckCommandRefShared<BlaeckButtonCommandRef>
{
public:
  BlaeckButtonCommandRef(Blaeck *owner, int16_t index) : BlaeckCommandRefShared<BlaeckButtonCommandRef>(owner, index) {}

  /*!
    @brief   Says what pressing the button does: restart, identify or update.

    Changes only the icon and wording a host uses.

    @param   deviceClass  "restart", "identify" or "update". No other values are
                          valid for a button.
    @return  The same handle, for chaining.

    @note    These buttons usually belong under diagnostic() as well.

    @code
      device.onButtonCommand("REBOOT", onReboot).withDeviceClass(F("restart")).diagnostic();
    @endcode
  */
  BlaeckButtonCommandRef &withDeviceClass(BlaeckString deviceClass)
  {
    _setDeviceClass(deviceClass);
    return *this;
  }

  /*!
    @brief   Sets fixed arguments that a press sends.

    Without this a press sends none. With it, the handler gets these arguments in
    params, as if they had been typed after the command name.

    @param   pressPayload  Comma-separated arguments.
    @return  The same handle, for chaining.

    @warning Nothing checks the payload. A typo reaches the handler as written.

    @note    One button has one payload. For another preset, register a second
             command with the same handler.

    @code
      device.onButtonCommand("DUT_ACTIVATE_ALL", onDutActivate)
          .withPressPayload(F("1,40"))
          .withDisplayName(F("Activate all DUTs"));
    @endcode
  */
  BlaeckButtonCommandRef &withPressPayload(BlaeckString pressPayload)
  {
    _setPressPayload(pressPayload);
    return *this;
  }
};

class BlaeckTextCommandRef : public BlaeckCommandRefStateful<BlaeckTextCommandRef>
{
public:
  BlaeckTextCommandRef(Blaeck *owner, int16_t index) : BlaeckCommandRefStateful<BlaeckTextCommandRef>(owner, index) {}

  // Otherwise the withOwnState() below would hide the inherited getter form.
  using BlaeckCommandRefStateful<BlaeckTextCommandRef>::withOwnState;

  /*!
    @brief   Reports the control's current text on a state channel of its own, read
             from a buffer.

    @param   channelName  Name of the channel. It takes a slot in withStateChannels().
    @param   value        The buffer holding the text. Use a global.
    @return  The same handle, for chaining.

    @code
      device.onTextCommand("SET_LABEL", onSetLabel)
          .withMaxLength(sizeof(DeviceLabel) - 1)
          .withOwnState(F("DeviceLabel"), DeviceLabel);
    @endcode
  */
  BlaeckTextCommandRef &withOwnState(BlaeckString channelName, const char *value)
  {
    _setOwnState(channelName, Blaeck_string, value);
    return *this;
  }

  /*!
    @brief   Sets the longest text the control accepts.

    Longer text is rejected before the handler runs, so the handler can copy what it
    gets without checking.

    @param   maxLength  In bytes after decoding, at most 255, which is also the
                        default. sizeof(buffer) - 1 is usually right.
    @return  The same handle, for chaining.

    @note    A value above 255 is ignored, with a warning on the debug stream, because
             a host may reject a text control that declares more.

    @code
      device.onTextCommand("SET_LABEL", onSetLabel)
          .withMaxLength(sizeof(DeviceLabel) - 1);
    @endcode
  */
  BlaeckTextCommandRef &withMaxLength(unsigned int maxLength)
  {
    _setMaxLength(maxLength);
    return *this;
  }

  /*!
    @brief   Asks a host to mask the field while it is typed.

    @param   mode  BLAECK_TEXT_MODE_PASSWORD to mask it. BLAECK_TEXT_MODE_PLAIN is the
                   default.
    @return  The same handle, for chaining.

    @warning This only hides the text on screen. It still travels as plain text.

    @code
      device.onTextCommand("SET_API_KEY", onSetApiKey)
          .withMaxLength(sizeof(ApiKey) - 1)
          .withMode(BLAECK_TEXT_MODE_PASSWORD);
    @endcode
  */
  BlaeckTextCommandRef &withMode(BlaeckTextMode mode)
  {
    _setMode((uint8_t)mode);
    return *this;
  }
};

// A number command needs a range and a select needs its options; without them a host guesses.
// So onNumberCommand() and onSelectCommand() return one of these two handles, whose only method
// is the required one, and which returns the full handle.
class BlaeckNumberCommandNeedsRange : public BlaeckCommandRefBase
{
public:
  BlaeckNumberCommandNeedsRange(Blaeck *owner, int16_t index) : BlaeckCommandRefBase(owner, index) {}

  /*!
    @brief   Sets the range of values the command accepts.

    A value outside it is rejected before the handler runs. The other modifiers are
    available on the handle this returns.

    @param   min   Lowest value accepted.
    @param   max   Highest value accepted. Must be above min, or the command gets no
                   range and accepts anything.
    @param   step  How finely a host's control moves. It isn't enforced. Pass 0 to let
                   the host choose, which is usually a step of 1.
    @return  The command's full handle, for chaining.

    @note    A host may reject a step below 0.001, and the control with it.

    @code
      device.onNumberCommand("SET_FREQ", onSetFreq).withRange(0.0f, 2.0f, 0.01f);
    @endcode
  */
  BlaeckNumberCommandRef withRange(float min, float max, float step)
  {
    _setRange(min, max, step);
    return BlaeckNumberCommandRef(_owner, _index);
  }
};

class BlaeckSelectCommandNeedsOptions : public BlaeckCommandRefBase
{
public:
  BlaeckSelectCommandNeedsOptions(Blaeck *owner, int16_t index) : BlaeckCommandRefBase(owner, index) {}

  /*!
    @brief   Sets the options the select offers.

    A value that is neither an option's name nor a valid index is rejected before
    the handler runs. The handler always gets the index as text, so
    atoi(params[0]) is enough; getSelectOptionNameAt() gives the name. The other
    modifiers are available on the handle this returns.

    @param   optionsCsv  Comma-separated option names. The first
                         has index 0.
    @return  The command's full handle, for chaining.

    @note    Don't call an option "none": a host may take it to mean nothing is
             selected.

    @warning A list that is empty or has a blank entry is refused, with a warning
             on the debug stream.

    @code
      device.onSelectCommand("SET_WAVE", onSetWave)
          .withOptions(F("Sine,Square,Triangle,Sawtooth"));
    @endcode
  */
  BlaeckSelectCommandRef withOptions(BlaeckString optionsCsv)
  {
    _setOptions(optionsCsv);
    return BlaeckSelectCommandRef(_owner, _index);
  }
};

// The shared part of the handles addSignal() returns, which describe how a signal is shown:
//
//   device.addSignal("FreeMemory", &FreeMemory)
//       .withUnit(F("B"))
//       .diagnostic();
//
// Numeric, text and bool signals each get their own handle, so a modifier that makes no sense
// for the type (a unit on a bool, say) fails to compile. A rejected handle ignores every call.
// Reporting policies remain available when signal metadata is off.
class BlaeckSignalRefBase
{
protected:
  BlaeckSignalRefBase(Blaeck *owner, int16_t index) : _owner(owner), _index(index) {}

  // A handle that names no signal and ignores every call, like a rejected one.
  BlaeckSignalRefBase() : _owner(nullptr), _index(-1) {}

  void _setFlash(BlaeckString value, uint16_t bit);

  void _setBit(uint16_t bit, bool on);

  void _setStateClass(BlaeckStateClass stateClass);

  void _setOptions(BlaeckString optionsCsv);

  void _setDisplayPrecision(uint8_t decimals);

  void _setNameSuffix(uint8_t suffix);
  void _setInterval(BlaeckIntervalMode mode, double delta);
  void _setOnChange(double delta, uint32_t minIntervalMs);
  void _setOnChange(BlaeckIntervalMode mode);
  Blaeck *_owner;
  int16_t _index;
};

// Modifiers every signal handle has. TYPE is the deriving handle, as in BlaeckCommandRefShared.
template <class TYPE>
class BlaeckSignalRefShared : public BlaeckSignalRefBase
{
public:
  /*!
    @brief   Selects how this signal participates in host-interval reports.

    BLAECK_ALWAYS is the default. BLAECK_ON_CHANGE compares against the last sent
    value when the interval is due. BLAECK_OFF excludes this signal from interval
    reports. This replaces the interval policy, independently of writeOnChange().
    Every ACTIVATE makes an initial interval report due: all interval-enabled
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
  TYPE &writeAtInterval(BlaeckIntervalMode mode, double delta = BLAECK_ANY_CHANGE)
  {
    _setInterval(mode, delta);
    return _self();
  }

  /*!
    @brief   Enables prompt reporting of changes, independently of host activation.

    Checked by tick() or writeIfDue(). Interval reporting remains separately
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
  TYPE &writeOnChange(double delta, uint32_t minIntervalMs = 100)
  {
    _setOnChange(delta, minIntervalMs);
    return _self();
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
  TYPE &writeOnChange(BlaeckIntervalMode mode)
  {
    _setOnChange(mode);
    return _self();
  }

  // A mode with a rate limit must not fall through to the numeric-threshold overload.
  TYPE &writeOnChange(BlaeckIntervalMode mode, uint32_t minIntervalMs) = delete;

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
  TYPE &withNameSuffix(uint8_t suffix)
  {
    _setNameSuffix(suffix);
    return _self();
  }

  /*!
    @brief   Sets what the value measures, such as "temperature" or "duration".

    Sent as written; the library doesn't check it against a list.

    @param   deviceClass  The device class.
    @return  The same handle, for chaining.

    @code
      device.addSignal(F("Uptime"), &Uptime).withDeviceClass(F("duration"));
    @endcode
  */
  TYPE &withDeviceClass(BlaeckString deviceClass)
  {
    _setFlash(deviceClass, BLAECK_SIG_HAS_DEVICE_CLASS);
    return _self();
  }

  /*!
    @brief   Sets the icon a host shows next to the value.

    @param   icon  A Material Design Icons name.
    @return  The same handle, for chaining.

    @code
      device.addSignal(F("Output"), &Output).withIcon(F("mdi:sine-wave"));
    @endcode
  */
  TYPE &withIcon(BlaeckString icon)
  {
    _setFlash(icon, BLAECK_SIG_HAS_ICON);
    return _self();
  }

  /*!
    @brief   Sets the label a host shows instead of the signal name.

    Useful when the name carries extra detail for logging, like "Output [V]". The
    signal is still identified by its name, so adding a label later moves nothing.

    @param   displayName  The label.
    @return  The same handle, for chaining.

    @code
      device.addSignal(F("Output [V]"), &Output).withUnit(F("V")).withDisplayName(F("Output"));
    @endcode
  */
  TYPE &withDisplayName(BlaeckString displayName)
  {
    _setFlash(displayName, BLAECK_SIG_HAS_DISPLAY_NAME);
    return _self();
  }

  /*!
    @brief   Marks the signal as diagnostic, such as free memory or uptime.

    A host usually keeps these off its default dashboard.

    @param   on  false undoes it.
    @return  The same handle, for chaining.

    @code
      device.addSignal(F("Uptime"), &Uptime).withUnit(F("s")).diagnostic();
    @endcode
  */
  TYPE &diagnostic(bool on = true)
  {
    _setBit(BLAECK_SIG_DIAGNOSTIC, on);
    return _self();
  }

  /*!
    @brief   Asks a host to create the signal disabled, until someone enables it.

    The device sends the value either way.

    @param   on  false undoes it.
    @return  The same handle, for chaining.

    @code
      device.addSignal(F("RawADC"), &rawAdc).disabledByDefault();
    @endcode
  */
  TYPE &disabledByDefault(bool on = true)
  {
    _setBit(BLAECK_SIG_DISABLED_BY_DEFAULT, on);
    return _self();
  }

  /*!
    @brief   Asks a host to record every reading, even one equal to the last.

    Otherwise a host may keep only changes, and a steady value looks the same as a
    sensor that stopped.

    @param   on  false undoes it.
    @return  The same handle, for chaining.

    @code
      device.addSignal(F("Temperature"), &Temperature).forceUpdate();
    @endcode
  */
  TYPE &forceUpdate(bool on = true)
  {
    _setBit(BLAECK_SIG_FORCE_UPDATE, on);
    return _self();
  }

protected:
  BlaeckSignalRefShared(Blaeck *owner, int16_t index) : BlaeckSignalRefBase(owner, index) {}
  BlaeckSignalRefShared() : BlaeckSignalRefBase() {}

private:
  TYPE &_self() { return *static_cast<TYPE *>(this); }
};

// The handle for a numeric signal, the only kind with a unit, state class or display precision.
class BlaeckNumericSignalRef : public BlaeckSignalRefShared<BlaeckNumericSignalRef>
{
public:
  /*!
    @brief   Creates an empty handle, to keep a signal's handle in a global.

    Assign what addSignal() returns, and the sketch can change how the signal is
    shown later. Until then, calls on it do nothing.

    @code
      BlaeckNumericSignalRef OutputSignal;  // file scope

      void setup() { OutputSignal = device.addSignal(F("Output"), &Output); }
      void loop()  { OutputSignal.withIcon(F("mdi:sine-wave")); }
    @endcode
  */
  BlaeckNumericSignalRef() : BlaeckSignalRefShared<BlaeckNumericSignalRef>() {}

  /*!
    @brief   Sets the unit a host shows after the value.

    A host that logs may keep only the signal name, so put the unit in the name as
    well if the log should show it.

    @param   unit  The unit. Non-ASCII characters must be UTF-8.
    @return  The same handle, for chaining.

    @code
      device.addSignal(F("Frequency"), &Frequency).withUnit(F("Hz"));
    @endcode
  */
  BlaeckNumericSignalRef &withUnit(BlaeckString unit)
  {
    _setFlash(unit, BLAECK_SIG_HAS_UNIT);
    return *this;
  }

  /*!
    @brief   Sets how the value behaves over time, so a host can keep statistics.

    Without it, a host keeps no long-term statistics for the signal.

    @param   stateClass  One of the BlaeckStateClass values.
    @return  The same handle, for chaining.

    @code
      device.addSignal(F("Output"), &Output)
          .withStateClass(BLAECK_STATE_CLASS_MEASUREMENT);
    @endcode
  */
  BlaeckNumericSignalRef &withStateClass(BlaeckStateClass stateClass)
  {
    _setStateClass(stateClass);
    return *this;
  }

  /*!
    @brief   Sets how many decimal places a host shows.

    The value sent is not rounded.

    @param   decimals  0 shows a whole number.
    @return  The same handle, for chaining.

    @code
      device.addSignal(F("Output"), &Output).withDisplayPrecision(3);
    @endcode
  */
  BlaeckNumericSignalRef &withDisplayPrecision(uint8_t decimals)
  {
    _setDisplayPrecision(decimals);
    return *this;
  }

private:
  // Private, so only addSignal() can make a handle that names a signal.
  BlaeckNumericSignalRef(Blaeck *owner, int16_t index) : BlaeckSignalRefShared<BlaeckNumericSignalRef>(owner, index) {}
  friend class Blaeck;
  friend class BlaeckDeviceBase;
};

// The handle for a text signal. It has no unit, state class or display precision, because a
// host would then treat the value as a number and reject the text. Keep it in step with
// BlaeckTextStateRef, which becomes the same kind of entity on a host.
class BlaeckTextSignalRef : public BlaeckSignalRefShared<BlaeckTextSignalRef>
{
public:
  /*!
    @brief   Creates an empty handle, to keep a signal's handle in a global.

    Assign what addSignal() returns. Until then, calls on it do nothing.
  */
  BlaeckTextSignalRef() : BlaeckSignalRefShared<BlaeckTextSignalRef>() {}

  /*!
    @brief   Sets the fixed list of values the signal can report.

    @param   optionsCsv  Comma-separated values.
    @return  The same handle, for chaining.

    @note    A host may also need withDeviceClass(F("enum")), and may reject a value
             that isn't in the list.

    @warning A list that is empty or has a blank entry is refused, with a warning on
             the debug stream.

    @code
      device.addSignal(F("Mode"), modeText)
          .withDeviceClass(F("enum"))
          .withOptions(F("idle,running,fault"));
    @endcode
  */
  BlaeckTextSignalRef &withOptions(BlaeckString optionsCsv)
  {
    _setOptions(optionsCsv);
    return *this;
  }

private:
  BlaeckTextSignalRef(Blaeck *owner, int16_t index) : BlaeckSignalRefShared<BlaeckTextSignalRef>(owner, index) {}
  friend class Blaeck;
  friend class BlaeckDeviceBase;
};

// The handle for a bool signal, which a host shows as an on/off sensor. Its device classes come
// from a different list: "door", "motion", "window" and so on, not "temperature". A class from
// the wrong list can make a host drop the signal.
class BlaeckBoolSignalRef : public BlaeckSignalRefShared<BlaeckBoolSignalRef>
{
public:
  /*!
    @brief   Creates an empty handle, to keep a signal's handle in a global.

    Assign what addSignal() returns. Until then, calls on it do nothing.
  */
  BlaeckBoolSignalRef() : BlaeckSignalRefShared<BlaeckBoolSignalRef>() {}

private:
  BlaeckBoolSignalRef(Blaeck *owner, int16_t index) : BlaeckSignalRefShared<BlaeckBoolSignalRef>(owner, index) {}
  friend class Blaeck;
  friend class BlaeckDeviceBase;
};

// The shared part of the handles addStateChannel() returns. As with signals there is one per
// value type, so a unit on a text channel fails to compile. A rejected channel's handle, or any
// handle with BLAECK_ENABLE_STATE_CHANNELS=0, ignores every call.
class BlaeckStateRefBase
{
protected:
  BlaeckStateRefBase(Blaeck *owner, int16_t index) : _owner(owner), _index(index) {}

  // The entry this handle names, or nullptr when registration was rejected. Defined out of
  // line, like the two below, because Blaeck is incomplete here.
  blaeck_detail::StateChannelEntry * _entry() const;

  // Marks the state catalog as changed, so it is sent again.
  void _markDirty() const;

  // The debug stream, or nullptr.
  Print *_debugStream() const;

  void _setStateClass(BlaeckStateClass stateClass)
  {
#if BLAECK_ENABLE_STATE_CHANNELS
    if (auto *e = _entry())
    {
      // Only a real change marks the catalog, here and in every modifier on these handles.
      const uint16_t flags =
          (uint16_t)((e->metaFlags & ~BLAECK_SCH_STATE_CLASS_MASK) |
                     (((uint16_t)stateClass << BLAECK_SCH_STATE_CLASS_SHIFT) &
                      BLAECK_SCH_STATE_CLASS_MASK));
      if (e->metaFlags != flags)
      {
        e->metaFlags = flags;
        _markDirty();
      }
    }
#else
    (void)stateClass;
#endif
  }

  void _setDisplayPrecision(uint8_t decimals)
  {
#if BLAECK_ENABLE_STATE_CHANNELS
    if (auto *e = _entry())
    {
      const uint16_t flags = (uint16_t)(e->metaFlags | BLAECK_SCH_HAS_DISPLAY_PRECISION);
      if (e->displayPrecision != decimals || e->metaFlags != flags)
      {
        e->displayPrecision = decimals;
        e->metaFlags = flags;
        _markDirty();
      }
    }
#else
    (void)decimals;
#endif
  }

  bool _storeString(detail::StoredString &slot, BlaeckString value);

  Blaeck *_owner;
  int16_t _index;
};

// Modifiers every state channel handle has. TYPE is the deriving handle.
template <class TYPE>
class BlaeckStateRefShared : public BlaeckStateRefBase
{
public:
  /*!
    @brief   Sets the icon a host shows next to the channel.

    @param   icon  A Material Design Icons name.
    @return  The same handle, for chaining.

    @code
      device.addStateChannel(F("Status"), BlaeckText).withIcon(F("mdi:pulse"));
    @endcode
  */
  TYPE &withIcon(BlaeckString icon)
  {
    if (blaeck_detail::flashStrEmpty(icon))
      icon = nullptr;
    if (auto *e = _entry())
    {
      if (e->icon != icon)
      {
        if (!_storeString(e->icon, icon))
          return _self();
        _markDirty();
      }
    }
    return _self();
  }

  /*!
    @brief   Marks the channel as diagnostic, such as a status line.

    A host usually keeps these off its default dashboard.

    @param   on  false undoes it.
    @return  The same handle, for chaining.

    @code
      device.addStateChannel(F("Status"), BlaeckText).withIcon(F("mdi:pulse")).diagnostic();
    @endcode
  */
  TYPE &diagnostic(bool on = true)
  {
    if (auto *e = _entry())
    {
      if (e->diagnostic != on)
      {
        e->diagnostic = on;
        _markDirty();
      }
    }
    return _self();
  }

  /*!
    @brief   Sets what kind of value the channel carries, such as "timestamp".

    @param   deviceClass  The device class.
    @return  The same handle, for chaining.

    @warning Use a class that fits the channel's type: "timestamp" or "date" for
             text, "voltage" and the like for a number. A wrong one can make a host
             drop the channel.

    @code
      device.addStateChannel(F("LastSeen"), BlaeckText).withDeviceClass(F("timestamp"));
    @endcode
  */
  TYPE &withDeviceClass(BlaeckString deviceClass)
  {
    if (blaeck_detail::flashStrEmpty(deviceClass))
      deviceClass = nullptr;
    if (auto *e = _entry())
    {
      if (e->deviceClass != deviceClass)
      {
        if (!_storeString(e->deviceClass, deviceClass))
          return _self();
        _markDirty();
      }
    }
    return _self();
  }

  /*!
    @brief   Asks a host to create the channel disabled, until someone enables it.

    The device sends values either way.

    @param   on  false undoes it.
    @return  The same handle, for chaining.

    @code
      device.addStateChannel(F("BuildInfo"), BlaeckText).disabledByDefault();
    @endcode
  */
  TYPE &disabledByDefault(bool on = true)
  {
    if (auto *e = _entry())
    {
      if (e->disabledByDefault != on)
      {
        e->disabledByDefault = on;
        _markDirty();
      }
    }
    return _self();
  }

  /*!
    @brief   Asks a host to record every value, even one equal to the last.

    Otherwise a host may ignore repeats, and a device that stopped looks the same as
    one reporting a steady value.

    @param   on  false undoes it.
    @return  The same handle, for chaining.

    @code
      device.addStateChannel(F("Heartbeat"), BlaeckText).forceUpdate();
    @endcode
  */
  TYPE &forceUpdate(bool on = true)
  {
    if (auto *e = _entry())
    {
      if (e->forceUpdate != on)
      {
        e->forceUpdate = on;
        _markDirty();
      }
    }
    return _self();
  }

protected:
  BlaeckStateRefShared(Blaeck *owner, int16_t index) : BlaeckStateRefBase(owner, index) {}

private:
  TYPE &_self() { return *static_cast<TYPE *>(this); }
};

// The handle for a numeric state channel. Keep it in step with BlaeckNumericSignalRef, which
// becomes the same kind of entity on a host.
class BlaeckNumericStateRef : public BlaeckStateRefShared<BlaeckNumericStateRef>
{
public:
  BlaeckNumericStateRef(Blaeck *owner, int16_t index) : BlaeckStateRefShared<BlaeckNumericStateRef>(owner, index) {}

  /*!
    @brief   Sets the unit a host shows after the value.

    @param   unit  The unit. Non-ASCII characters must be UTF-8.
    @return  The same handle, for chaining.

    @code
      device.addStateChannel(F("Amplitude"), &Amplitude).withUnit(F("V"));
    @endcode
  */
  BlaeckNumericStateRef &withUnit(BlaeckString unit)
  {
    if (blaeck_detail::flashStrEmpty(unit))
      unit = nullptr;
    if (auto *e = _entry())
    {
      // Keep the flag in step with the pointer, including when the unit is removed.
      const uint16_t flags = (unit != nullptr) ? (uint16_t)(e->metaFlags | BLAECK_SCH_HAS_UNIT)
                                               : (uint16_t)(e->metaFlags & ~BLAECK_SCH_HAS_UNIT);
      if (e->unit != unit || e->metaFlags != flags)
      {
        if (!_storeString(e->unit, unit))
          return *this;
        e->metaFlags = flags;
        _markDirty();
      }
    }
    return *this;
  }

  /*!
    @brief   Sets how the value behaves over time, so a host can keep statistics.

    State channel values aren't logged as data, so this is the only way a host keeps
    their history.

    @param   stateClass  One of the BlaeckStateClass values.
    @return  The same handle, for chaining.

    @code
      device.addStateChannel(F("Amplitude"), &Amplitude)
          .withStateClass(BLAECK_STATE_CLASS_MEASUREMENT);
    @endcode
  */
  BlaeckNumericStateRef &withStateClass(BlaeckStateClass stateClass)
  {
    _setStateClass(stateClass);
    return *this;
  }

  /*!
    @brief   Sets how many decimal places a host shows.

    The value sent is not rounded.

    @param   decimals  0 shows a whole number.
    @return  The same handle, for chaining.

    @code
      device.addStateChannel(F("Amplitude"), &Amplitude).withDisplayPrecision(2);
    @endcode
  */
  BlaeckNumericStateRef &withDisplayPrecision(uint8_t decimals)
  {
    _setDisplayPrecision(decimals);
    return *this;
  }

  /*!
    @brief   Reads the channel's value from a getter each time it is sent.

    For a value calculated from other variables, so it is never out of date. The
    catalog is sent at startup, when channels change, and whenever a host asks.

    @param   getStateValue  Returns the value. Its type must match the channel's; a
                            getter of another type is refused, with a warning on the
                            debug stream.
    @return  The same handle, for chaining.

    @warning The getter runs while a frame is being built, so it must not write a
             frame itself. Keep it to reading variables and calculating.

    @code
      device.addStateChannel(F("Efficiency"), BlaeckFloat).withStateValue(efficiency);
    @endcode
  */
  BlaeckNumericStateRef &withStateValue(BlaeckStateByteGetter getStateValue)
  {
    if (auto *e = _entry())
    {
    const dataType want = Blaeck_byte;
      if (!blaeck_detail::stateGetterAccepted(e->stateValue, want, e->valueType,
                                              F("withStateValue"), _debugStream(), e->name, e->nameInFlash))
        return *this;
      if (e->getNumber != (void (*)())getStateValue)
      {
        e->getNumber = (void (*)())getStateValue;
        _markDirty();
      }
    }
    return *this;
  }

  BlaeckNumericStateRef &withStateValue(BlaeckStateShortGetter getStateValue)
  {
    if (auto *e = _entry())
    {
    const dataType want = Blaeck_short;
      if (!blaeck_detail::stateGetterAccepted(e->stateValue, want, e->valueType,
                                              F("withStateValue"), _debugStream(), e->name, e->nameInFlash))
        return *this;
      if (e->getNumber != (void (*)())getStateValue)
      {
        e->getNumber = (void (*)())getStateValue;
        _markDirty();
      }
    }
    return *this;
  }

  BlaeckNumericStateRef &withStateValue(BlaeckStateUShortGetter getStateValue)
  {
    if (auto *e = _entry())
    {
    const dataType want = Blaeck_ushort;
      if (!blaeck_detail::stateGetterAccepted(e->stateValue, want, e->valueType,
                                              F("withStateValue"), _debugStream(), e->name, e->nameInFlash))
        return *this;
      if (e->getNumber != (void (*)())getStateValue)
      {
        e->getNumber = (void (*)())getStateValue;
        _markDirty();
      }
    }
    return *this;
  }

  BlaeckNumericStateRef &withStateValue(BlaeckStateIntGetter getStateValue)
  {
    if (auto *e = _entry())
    {
#ifdef __AVR__
    const dataType want = Blaeck_int;
#else
    const dataType want = Blaeck_long;
#endif
      if (!blaeck_detail::stateGetterAccepted(e->stateValue, want, e->valueType,
                                              F("withStateValue"), _debugStream(), e->name, e->nameInFlash))
        return *this;
      if (e->getNumber != (void (*)())getStateValue)
      {
        e->getNumber = (void (*)())getStateValue;
        _markDirty();
      }
    }
    return *this;
  }

  BlaeckNumericStateRef &withStateValue(BlaeckStateUIntGetter getStateValue)
  {
    if (auto *e = _entry())
    {
#ifdef __AVR__
    const dataType want = Blaeck_uint;
#else
    const dataType want = Blaeck_ulong;
#endif
      if (!blaeck_detail::stateGetterAccepted(e->stateValue, want, e->valueType,
                                              F("withStateValue"), _debugStream(), e->name, e->nameInFlash))
        return *this;
      if (e->getNumber != (void (*)())getStateValue)
      {
        e->getNumber = (void (*)())getStateValue;
        _markDirty();
      }
    }
    return *this;
  }

  BlaeckNumericStateRef &withStateValue(BlaeckStateLongGetter getStateValue)
  {
    if (auto *e = _entry())
    {
    const dataType want = Blaeck_long;
      if (!blaeck_detail::stateGetterAccepted(e->stateValue, want, e->valueType,
                                              F("withStateValue"), _debugStream(), e->name, e->nameInFlash))
        return *this;
      if (e->getNumber != (void (*)())getStateValue)
      {
        e->getNumber = (void (*)())getStateValue;
        _markDirty();
      }
    }
    return *this;
  }

  BlaeckNumericStateRef &withStateValue(BlaeckStateULongGetter getStateValue)
  {
    if (auto *e = _entry())
    {
    const dataType want = Blaeck_ulong;
      if (!blaeck_detail::stateGetterAccepted(e->stateValue, want, e->valueType,
                                              F("withStateValue"), _debugStream(), e->name, e->nameInFlash))
        return *this;
      if (e->getNumber != (void (*)())getStateValue)
      {
        e->getNumber = (void (*)())getStateValue;
        _markDirty();
      }
    }
    return *this;
  }

  BlaeckNumericStateRef &withStateValue(BlaeckStateFloatGetter getStateValue)
  {
    if (auto *e = _entry())
    {
    const dataType want = Blaeck_float;
      if (!blaeck_detail::stateGetterAccepted(e->stateValue, want, e->valueType,
                                              F("withStateValue"), _debugStream(), e->name, e->nameInFlash))
        return *this;
      if (e->getNumber != (void (*)())getStateValue)
      {
        e->getNumber = (void (*)())getStateValue;
        _markDirty();
      }
    }
    return *this;
  }

  BlaeckNumericStateRef &withStateValue(BlaeckStateDoubleGetter getStateValue)
  {
    if (auto *e = _entry())
    {
#ifdef __AVR__
    const dataType want = Blaeck_float;
#else
    const dataType want = Blaeck_double;
#endif
      if (!blaeck_detail::stateGetterAccepted(e->stateValue, want, e->valueType,
                                              F("withStateValue"), _debugStream(), e->name, e->nameInFlash))
        return *this;
      if (e->getNumber != (void (*)())getStateValue)
      {
        e->getNumber = (void (*)())getStateValue;
        _markDirty();
      }
    }
    return *this;
  }

};

// The handle for a text state channel. Keep it in step with BlaeckTextSignalRef.
class BlaeckTextStateRef : public BlaeckStateRefShared<BlaeckTextStateRef>
{
public:
  BlaeckTextStateRef(Blaeck *owner, int16_t index) : BlaeckStateRefShared<BlaeckTextStateRef>(owner, index) {}

  /*!
    @brief   Reads the channel's text from a getter each time it is sent.

    Without a getter, the channel has no value until writeState() sends one.

    @param   getStateText  Returns the text. A static local buffer is fine.
    @return  The same handle, for chaining.

    @warning The getter runs while a frame is being built, so it must not write a
             frame itself: calling writeState() or writeEvent() there corrupts the
             frame being built.

    @code
      device.addStateChannel(F("Offset"), BlaeckText).withStateText(offsetText);
    @endcode
  */
  BlaeckTextStateRef &withStateText(BlaeckStateTextGetter getStateText)
  {
    if (auto *e = _entry())
    {
      if (!blaeck_detail::stateGetterAccepted(e->stateValue, Blaeck_string, e->valueType,
                                              F("withStateText"), _debugStream(), e->name, e->nameInFlash))
        return *this;
      if (e->getStateText != getStateText)
      {
        e->getStateText = getStateText;
        _markDirty();
      }
    }
    return *this;
  }

  /*!
    @brief   Sets the fixed list of values the channel can report.

    @param   optionsCsv  Comma-separated values.
    @return  The same handle, for chaining.

    @note    A host may also need withDeviceClass(F("enum")), and may reject a value
             that isn't in the list.

    @warning A list that is empty or has a blank entry is refused, with a warning on
             the debug stream.

    @code
      device.addStateChannel(F("Mode"), BlaeckText)
          .withDeviceClass(F("enum"))
          .withOptions(F("idle,running,fault"));
    @endcode
  */
  BlaeckTextStateRef &withOptions(BlaeckString optionsCsv)
  {
    if (auto *e = _entry())
    {
      if (!blaeck_detail::optionsAccepted(optionsCsv, _debugStream(), e->name, e->nameInFlash))
        return *this;
      if (e->options != optionsCsv)
      {
        if (!_storeString(e->options, optionsCsv))
          return *this;
        _markDirty();
      }
    }
    return *this;
  }
};

// The handle for a bool state channel, shown by a host as an on/off sensor. Its device classes
// come from the same list as a bool signal's.
class BlaeckBoolStateRef : public BlaeckStateRefShared<BlaeckBoolStateRef>
{
public:
  BlaeckBoolStateRef(Blaeck *owner, int16_t index) : BlaeckStateRefShared<BlaeckBoolStateRef>(owner, index) {}

  /*!
    @brief   Reads the channel's value from a getter each time it is sent.

    @param   getStateValue  Returns the value.
    @return  The same handle, for chaining.

    @warning The getter runs while a frame is being built, so it must not write a
             frame itself.

    @code
      device.addStateChannel(F("Running"), BlaeckBool).withStateValue(isRunning);
    @endcode
  */
  BlaeckBoolStateRef &withStateValue(BlaeckStateBoolGetter getStateValue)
  {
    if (auto *e = _entry())
    {
    const dataType want = Blaeck_bool;
      if (!blaeck_detail::stateGetterAccepted(e->stateValue, want, e->valueType,
                                              F("withStateValue"), _debugStream(), e->name, e->nameInFlash))
        return *this;
      if (e->getNumber != (void (*)())getStateValue)
      {
        e->getNumber = (void (*)())getStateValue;
        _markDirty();
      }
    }
    return *this;
  }
};

class BlaeckEventChannelRef
{
public:
  BlaeckEventChannelRef(Blaeck *owner, int16_t index) : _owner(owner), _index(index) {}

  /*!
    @brief   Sets the icon a host shows next to the channel.

    @param   icon  A Material Design Icons name.
    @return  The same handle, for chaining.

    @code
      device.addEventChannel(F("Activity"), F("idle,resumed")).withIcon(F("mdi:pulse"));
    @endcode
  */
  BlaeckEventChannelRef withIcon(BlaeckString icon);

  /*!
    @brief   Marks the channel as diagnostic.

    @param   on  false undoes it.
    @return  The same handle, for chaining.

    @code
      device.addEventChannel(F("Faults"), F("brownout,watchdog")).diagnostic();
    @endcode
  */
  BlaeckEventChannelRef diagnostic(bool on = true);

  /*!
    @brief   Sets what kind of events the channel reports: button, doorbell or motion.

    @param   deviceClass  F("button"), F("doorbell") or F("motion"). Anything else can
                          make a host drop the channel.
    @return  The same handle, for chaining.

    @note    A doorbell channel should have a "ring" event type. A button channel may
             use the standard names press_start, press_end, long_press_start,
             long_press_end, multi_press_ongoing and multi_press_end, but needn't.

    @code
      device.addEventChannel(F("Doorbell"), F("ring")).withDeviceClass(F("doorbell"));
    @endcode
  */
  BlaeckEventChannelRef withDeviceClass(BlaeckString deviceClass);

  /*!
    @brief   Asks a host to create the channel disabled, until someone enables it.

    The device sends events either way. Events aren't kept, so enabling the channel
    later doesn't show earlier ones.

    @param   on  false undoes it.
    @return  The same handle, for chaining.

    @code
      device.addEventChannel(F("Debug"), F("trace")).disabledByDefault();
    @endcode
  */
  BlaeckEventChannelRef disabledByDefault(bool on = true);

private:
  Blaeck *_owner;
  int16_t _index;
};

// What the board and each device from addDevice() declare and report through: signals,
// commands, state channels and events, and the writes that find them by name. Blaeck
// inherits it for the board, BlaeckDeviceRef for a device. Names are looked up within the
// device the call is made on, so `pump.write("Flow", v)` finds only the pump's signal.
class BlaeckDeviceBase
{
public:
  // ----- Signals -----

  /*!
    @brief   Adds a variable to be sampled and logged over time.

    A signal is a reading that is sent on every interval and kept as history. For
    a setting or a status that shouldn't be logged, use addStateChannel().

    The variable is read each time data is sent, so it must be a global.

    @param   signalName  The name a host shows and logs the signal under. It is
                         copied.
    @param   value       The variable. There is an overload for each type.
    @return  A handle for describing how a host shows the signal. It can be
             ignored, or kept in a global to change the signal later.
    @note    If the table is full, the signal is dropped and the handle ignores
             every call; hasRejectedSignals() reports it. Describing a signal
             allocates memory, and if that fails the signal is still sent, just
             without the description.

    @code
      device.addSignal("Temperature", &Temperature)
          .withUnit(F("\xC2\xB0" "C"))
          .withDeviceClass(F("temperature"))
          .withStateClass(BLAECK_STATE_CLASS_MEASUREMENT)
          .withDisplayPrecision(1);
    @endcode
  */
  BlaeckBoolSignalRef addSignal(const char *signalName, bool *value);
  BlaeckNumericSignalRef addSignal(const char *signalName, byte *value);
  BlaeckNumericSignalRef addSignal(const char *signalName, short *value);
  BlaeckNumericSignalRef addSignal(const char *signalName, unsigned short *value);
  BlaeckNumericSignalRef addSignal(const char *signalName, int *value);
  BlaeckNumericSignalRef addSignal(const char *signalName, unsigned int *value);
  BlaeckNumericSignalRef addSignal(const char *signalName, long *value);
  BlaeckNumericSignalRef addSignal(const char *signalName, unsigned long *value);
  BlaeckNumericSignalRef addSignal(const char *signalName, float *value);
  BlaeckNumericSignalRef addSignal(const char *signalName, double *value);
  BlaeckTextSignalRef addSignal(const char *signalName, const char *value);
  // The text stays in flash and is retained, like a RAM text pointer.
  BlaeckTextSignalRef addSignal(const char *signalName, const __FlashStringHelper *value);

  /*!
    @brief   Adds a signal whose name is an F() literal.

    The name stays in flash instead of being copied to SRAM.

    @param   signalName  The name, as an F() literal.
    @param   value       The variable to read.
    @return  A handle for describing how a host shows the signal.

    @code
      device.addSignal(F("Temperature"), &Temperature);
    @endcode
  */
  BlaeckBoolSignalRef addSignal(const __FlashStringHelper *signalName, bool *value);
  BlaeckNumericSignalRef addSignal(const __FlashStringHelper *signalName, byte *value);
  BlaeckNumericSignalRef addSignal(const __FlashStringHelper *signalName, short *value);
  BlaeckNumericSignalRef addSignal(const __FlashStringHelper *signalName, unsigned short *value);
  BlaeckNumericSignalRef addSignal(const __FlashStringHelper *signalName, int *value);
  BlaeckNumericSignalRef addSignal(const __FlashStringHelper *signalName, unsigned int *value);
  BlaeckNumericSignalRef addSignal(const __FlashStringHelper *signalName, long *value);
  BlaeckNumericSignalRef addSignal(const __FlashStringHelper *signalName, unsigned long *value);
  BlaeckNumericSignalRef addSignal(const __FlashStringHelper *signalName, float *value);
  BlaeckNumericSignalRef addSignal(const __FlashStringHelper *signalName, double *value);
  BlaeckTextSignalRef addSignal(const __FlashStringHelper *signalName, const char *value);
  /*!
    @brief   Registers a text signal with its name and initial value in flash.

    The value is retained without copying it into RAM. Later write() calls may
    replace it with either RAM or flash text.

    @code
      device.addSignal(F("Status"), F("Idle"));
    @endcode
  */
  BlaeckTextSignalRef addSignal(const __FlashStringHelper *signalName, const __FlashStringHelper *value);

  // ----- State channels -----
  // With BLAECK_ENABLE_STATE_CHANNELS=0 these compile but do nothing.

  /*!
    @brief   Adds a state channel, for a current value that is shown but not logged.

    Use it for a status or a setting. Unlike a signal, it is sent when it changes,
    not on every interval.

    The second argument sets the type. Pass a variable and the channel reads it when
    it is sent. Pass a tag such as BlaeckText or BlaeckFloat and the channel only
    carries what writeState() sends.

    @param   channelName  The name a host shows. It is copied. At most 15 characters
                          on AVR and 31 elsewhere; a longer name is refused.
    @return  A handle for describing how a host shows the channel.

    @code
      device.addStateChannel(F("Status"), BlaeckText).withIcon(F("mdi:pulse")).diagnostic();
      device.addStateChannel(F("Amplitude"), &Amplitude).withUnit(F("V"));
    @endcode
  */
  BlaeckTextStateRef addStateChannel(const char *channelName, BlaeckTextTag);
  BlaeckBoolStateRef addStateChannel(const char *channelName, BlaeckBoolTag);
  BlaeckNumericStateRef addStateChannel(const char *channelName, BlaeckNumericTag type);
  BlaeckTextStateRef addStateChannel(const char *channelName, const char *value);
  // A fixed flash value; writeState(channelName) reads it without copying it into RAM.
  BlaeckTextStateRef addStateChannel(const char *channelName, const __FlashStringHelper *value);
  BlaeckBoolStateRef addStateChannel(const char *channelName, bool *value);
  BlaeckNumericStateRef addStateChannel(const char *channelName, byte *value);
  BlaeckNumericStateRef addStateChannel(const char *channelName, short *value);
  BlaeckNumericStateRef addStateChannel(const char *channelName, unsigned short *value);
  BlaeckNumericStateRef addStateChannel(const char *channelName, int *value);
  BlaeckNumericStateRef addStateChannel(const char *channelName, unsigned int *value);
  BlaeckNumericStateRef addStateChannel(const char *channelName, long *value);
  BlaeckNumericStateRef addStateChannel(const char *channelName, unsigned long *value);
  BlaeckNumericStateRef addStateChannel(const char *channelName, float *value);
  BlaeckNumericStateRef addStateChannel(const char *channelName, double *value);

  // The same, with an F() name, which stays in flash instead of being copied.
  BlaeckTextStateRef addStateChannel(const __FlashStringHelper *channelName, BlaeckTextTag);
  BlaeckBoolStateRef addStateChannel(const __FlashStringHelper *channelName, BlaeckBoolTag);
  BlaeckNumericStateRef addStateChannel(const __FlashStringHelper *channelName, BlaeckNumericTag type);
  BlaeckTextStateRef addStateChannel(const __FlashStringHelper *channelName, const char *value);
  /*!
    @brief   Registers a state channel bound to a fixed flash text value.

    The name and text are retained without RAM copies. Use writeState(channelName)
    to send the bound value, rather than pushing another text value.

    @code
      device.addStateChannel(F("Build"), F("Production"));
      device.writeState(F("Build"));
    @endcode
  */
  BlaeckTextStateRef addStateChannel(const __FlashStringHelper *channelName, const __FlashStringHelper *value);
  BlaeckBoolStateRef addStateChannel(const __FlashStringHelper *channelName, bool *value);
  BlaeckNumericStateRef addStateChannel(const __FlashStringHelper *channelName, byte *value);
  BlaeckNumericStateRef addStateChannel(const __FlashStringHelper *channelName, short *value);
  BlaeckNumericStateRef addStateChannel(const __FlashStringHelper *channelName, unsigned short *value);
  BlaeckNumericStateRef addStateChannel(const __FlashStringHelper *channelName, int *value);
  BlaeckNumericStateRef addStateChannel(const __FlashStringHelper *channelName, unsigned int *value);
  BlaeckNumericStateRef addStateChannel(const __FlashStringHelper *channelName, long *value);
  BlaeckNumericStateRef addStateChannel(const __FlashStringHelper *channelName, unsigned long *value);
  BlaeckNumericStateRef addStateChannel(const __FlashStringHelper *channelName, float *value);
  BlaeckNumericStateRef addStateChannel(const __FlashStringHelper *channelName, double *value);

  /*!
    @brief   Sends a text value on a state channel.

    A host shows it, but it isn't logged as data.

    @param   channelName  A channel added with addStateChannel().
    @param   text         The value. Anything past 255 bytes is cut off, with a
                          warning on the debug stream the first time.

    @warning The value is dropped if the channel doesn't exist, carries a number
             (use writeState(channelName)), or belongs to a command (use
             writeCommandState()). The debug stream says which.

    @code
      char text[40];
      snprintf(text, sizeof(text), "up %lu s", millis() / 1000UL);
      device.writeState(F("Status"), text);
    @endcode
  */
  void writeState(const char *channelName, const char *text);

  // Sends the channel's current value, read from its variable or getter.
  void writeState(const char *channelName);

  // The same two, with an F() name.
  void writeState(const __FlashStringHelper *channelName, const char *text);
  void writeState(const __FlashStringHelper *channelName);
  /*!
    @brief   Sends flash text on an unbound text state channel.

    The text is read during this call, not copied into a retained value. The same
    ownership, type and 255-byte limit checks apply as for RAM text.

    @code
      device.addStateChannel(F("Status"), BlaeckText);
      device.writeState(F("Status"), F("Running"));
    @endcode
  */
  void writeState(const __FlashStringHelper *channelName, const __FlashStringHelper *text);
  // The same flash value with a RAM channel name.
  void writeState(const char *channelName, const __FlashStringHelper *text);
  void writeState(const char *channelName, decltype(nullptr)) { writeState(channelName, static_cast<const char *>(nullptr)); }
  void writeState(const __FlashStringHelper *channelName, decltype(nullptr)) { writeState(channelName, static_cast<const char *>(nullptr)); }

  /*!
    @brief   Sends a number on a state channel that was added with a type tag.

    The value is converted to the channel's type, so 20 on a float channel sends 20.0.

    @param   channelName  A channel added with a tag such as BlaeckFloat. A channel
                          with a variable or getter refuses this.
    @param   value        The value.

    @code
      device.writeState(F("Temperature"), 20.5f);
    @endcode
  */
  void writeState(const char *channelName, bool value);
  void writeState(const char *channelName, byte value);
  void writeState(const char *channelName, short value);
  void writeState(const char *channelName, unsigned short value);
  void writeState(const char *channelName, int value);
  void writeState(const char *channelName, unsigned int value);
  void writeState(const char *channelName, long value);
  void writeState(const char *channelName, unsigned long value);
  void writeState(const char *channelName, float value);
  void writeState(const char *channelName, double value);

  // The same, with an F() name.
  void writeState(const __FlashStringHelper *channelName, bool value);
  void writeState(const __FlashStringHelper *channelName, byte value);
  void writeState(const __FlashStringHelper *channelName, short value);
  void writeState(const __FlashStringHelper *channelName, unsigned short value);
  void writeState(const __FlashStringHelper *channelName, int value);
  void writeState(const __FlashStringHelper *channelName, unsigned int value);
  void writeState(const __FlashStringHelper *channelName, long value);
  void writeState(const __FlashStringHelper *channelName, unsigned long value);
  void writeState(const __FlashStringHelper *channelName, float value);
  void writeState(const __FlashStringHelper *channelName, double value);

  /*!
    @brief   Sends a command's current value on its withOwnState() channel.

    Call it after the value changes, usually from the handler.

    @param   command  The command's name. In a handler, pass its command argument.
    @note    Does nothing for a command without withOwnState().

    @code
      void onSetOffset(const char *command, const char *const *params, byte paramCount)
      {
        Offset = (float)atof(params[0]);
        device.writeCommandState(command);
      }
    @endcode
  */
  void writeCommandState(const char *command);
  /*!
    @brief   Sends a command's owned state using a flash command name.

    @code
      device.writeCommandState(F("SET_SPEED"));
    @endcode
  */
  void writeCommandState(const __FlashStringHelper *command);
  void writeCommandState(decltype(nullptr)) { writeCommandState(static_cast<const char *>(nullptr)); }

  // ----- Events -----
  // With BLAECK_ENABLE_EVENTS=0 these compile but do nothing.

  /*!
    @brief   Adds an event channel, for reporting things that happen.

    @param   channelName  The name a host shows. It is copied.
    @param   eventTypes   The events the channel can report, comma-separated.
    @return  A handle for describing how a host shows the channel.

    @warning A channel with no event types, or with a blank one, is refused.

    @code
      device.addEventChannel(F("Activity"), F("idle_warning,resumed"))
          .withIcon(F("mdi:pulse"));
    @endcode
  */
  BlaeckEventChannelRef addEventChannel(const char *channelName, BlaeckString eventTypes);

  // The same, with an F() name.
  BlaeckEventChannelRef addEventChannel(const __FlashStringHelper *channelName, BlaeckString eventTypes);

  /*!
    @brief   Adds one more event type to an existing event channel.

    For types that depend on the hardware fitted.

    @param   channelName  A channel added with addEventChannel().
    @param   eventType    The new type.
    @return  False if the type is blank or a duplicate, the channel doesn't exist, or
             the type table is full. Each is reported on the debug stream.

    @code
      device.addEventChannel(F("Activity"), F("idle_warning,resumed"));
      if (hasBatteryMonitor)
        device.addEventType(F("Activity"), F("low_battery"));
    @endcode
  */
  bool addEventType(const char *channelName, BlaeckString eventType);
  bool addEventType(const __FlashStringHelper *channelName, BlaeckString eventType);

  /*!
    @brief   Reports an event on an event channel.

    A host shows it, but it isn't logged as data.

    @param   channelName  A channel added with addEventChannel().
    @param   eventType    One of that channel's event types.

    @warning An unknown channel or type is dropped, with a note on the debug stream.
             Types are case-sensitive.

    @code
      device.writeEvent(F("Activity"), F("idle_warning"));
    @endcode
  */
  void writeEvent(const char *channelName, BlaeckString eventType);

  // The same, with an F() name.
  void writeEvent(const __FlashStringHelper *channelName, BlaeckString eventType);

  // ----- Data Write -----

  /*!
    @brief   Sets a signal's value and sends it right away.

    Separate from the timed interval, so a value can go out the moment something
    happens. There are overloads for each type, and ones taking a timestamp.

    @param   signalName  The signal's name.
    @param   value       The new value.
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
  void write(const char *signalName, bool value);
  void write(const char *signalName, byte value);
  void write(const char *signalName, short value);
  void write(const char *signalName, unsigned short value);
  void write(const char *signalName, int value);
  void write(const char *signalName, unsigned int value);
  void write(const char *signalName, long value);
  void write(const char *signalName, unsigned long value);
  void write(const char *signalName, float value);
  void write(const char *signalName, double value);

  /*!
    @brief   Points a text signal at a value and sends it right away.

    @param   signalName  The registered text signal's name.
    @param   value       Null-terminated text.
    @warning The text is not copied. Keep its buffer valid until replaced by another
             text write or the signal is removed. Later reports read the same memory.
             Use a global/static buffer or a string literal, not a local array or
             a temporary String's c_str().

    @code
      device.addSignal(F("Status"), "Idle");
      device.write("Status", "Running");
    @endcode
  */
  void write(const char *signalName, const char *value);

  void write(const char *signalName, bool value, unsigned long long timestamp);
  void write(const char *signalName, byte value, unsigned long long timestamp);
  void write(const char *signalName, short value, unsigned long long timestamp);
  void write(const char *signalName, unsigned short value, unsigned long long timestamp);
  void write(const char *signalName, int value, unsigned long long timestamp);
  void write(const char *signalName, unsigned int value, unsigned long long timestamp);
  void write(const char *signalName, long value, unsigned long long timestamp);
  void write(const char *signalName, unsigned long value, unsigned long long timestamp);
  void write(const char *signalName, float value, unsigned long long timestamp);
  void write(const char *signalName, double value, unsigned long long timestamp);

  /*!
    @brief   Points a text signal at a value and sends it with a supplied timestamp.

    @param   signalName  The registered text signal's name.
    @param   value       Null-terminated text.
    @param   timestamp   Sample time in microseconds in the configured time base.
    @warning The text is not copied. Keep its buffer valid until replaced by another
             text write or the signal is removed. Later reports read the same memory.
             Use a global/static buffer or a string literal, not a local array or
             a temporary String's c_str().

    @code
      device.addSignal(F("Status"), "Idle");
      device.write("Status", "Running", 123456ULL);
    @endcode
  */
  void write(const char *signalName, const char *value, unsigned long long timestamp);

  /*!
    @brief   Returns a signal's index, for the faster by-index calls.

    Calls by name compare against every signal's name, so look the index up once in
    setup() for anything that runs often.

    @param   signalName  The name the signal was added with.
    @return  Its index, or -1 if there is no signal by that name.

    @code
      int tempIndex = device.findSignalIndex("Temperature");
      device.write(tempIndex, readSensor());
    @endcode
  */
  int findSignalIndex(const char *signalName);
  /*!
    @brief   Looks up a signal by its flash name, including any numeric suffix.

    @return  Its index, or -1 if there is no match.

    @code
      int statusIndex = device.findSignalIndex(F("Status"));
    @endcode
  */
  int findSignalIndex(const __FlashStringHelper *signalName);
  int findSignalIndex(decltype(nullptr)) { return findSignalIndex(static_cast<const char *>(nullptr)); }

  // The same, by index.
  void write(int signalIndex, bool value);
  void write(int signalIndex, byte value);
  void write(int signalIndex, short value);
  void write(int signalIndex, unsigned short value);
  void write(int signalIndex, int value);
  void write(int signalIndex, unsigned int value);
  void write(int signalIndex, long value);
  void write(int signalIndex, unsigned long value);
  void write(int signalIndex, float value);
  void write(int signalIndex, double value);

  /*!
    @brief   Points a text signal at a value and sends it, looking it up by index.

    @param   signalIndex  The registered text signal's index.
    @param   value        Null-terminated text.
    @warning The text is not copied. Keep its buffer valid until replaced by another
             text write or the signal is removed. Later reports read the same memory.
             Use a global/static buffer or a string literal, not a local array or
             a temporary String's c_str().

    @code
      device.addSignal(F("Status"), "Idle");
      int statusIndex = device.findSignalIndex("Status");
      device.write(statusIndex, "Running");
    @endcode
  */
  void write(int signalIndex, const char *value);

  void write(int signalIndex, bool value, unsigned long long timestamp);
  void write(int signalIndex, byte value, unsigned long long timestamp);
  void write(int signalIndex, short value, unsigned long long timestamp);
  void write(int signalIndex, unsigned short value, unsigned long long timestamp);
  void write(int signalIndex, int value, unsigned long long timestamp);
  void write(int signalIndex, unsigned int value, unsigned long long timestamp);
  void write(int signalIndex, long value, unsigned long long timestamp);
  void write(int signalIndex, unsigned long value, unsigned long long timestamp);
  void write(int signalIndex, float value, unsigned long long timestamp);
  void write(int signalIndex, double value, unsigned long long timestamp);

  /*!
    @brief   Points a text signal at a value and sends it by index with a timestamp.

    @param   signalIndex  The registered text signal's index.
    @param   value        Null-terminated text.
    @param   timestamp    Sample time in microseconds in the configured time base.
    @warning The text is not copied. Keep its buffer valid until replaced by another
             text write or the signal is removed. Later reports read the same memory.
             Use a global/static buffer or a string literal, not a local array or
             a temporary String's c_str().

    @code
      device.addSignal(F("Status"), "Idle");
      int statusIndex = device.findSignalIndex("Status");
      device.write(statusIndex, "Running", 123456ULL);
    @endcode
  */
  void write(int signalIndex, const char *value, unsigned long long timestamp);

  /*!
    @brief   Retains flash text as a signal's value and sends it immediately.

    Later reports read the same flash text. No RAM copy is made just to retain it;
    change tracking still uses its existing last-sent snapshot. RAM and flash writes
    can replace each other without changing the signal's reporting policies.

    @code
      device.addSignal(F("Status"), F("Idle"));
      device.write("Status", F("Running"));
    @endcode
  */
  void write(const char *signalName, const __FlashStringHelper *value);
  // The same, with the timestamp in microseconds in the configured time base.
  void write(const char *signalName, const __FlashStringHelper *value, unsigned long long timestamp);
  // The same flash text, addressed by signal index.
  void write(int signalIndex, const __FlashStringHelper *value);
  void write(int signalIndex, const __FlashStringHelper *value, unsigned long long timestamp);

  void write(const char *name, decltype(nullptr)) { write(name, static_cast<const char *>(nullptr)); }
  void write(const char *name, decltype(nullptr), unsigned long long timestamp) { write(name, static_cast<const char *>(nullptr), timestamp); }
  void write(int index, decltype(nullptr)) { write(index, static_cast<const char *>(nullptr)); }
  void write(int index, decltype(nullptr), unsigned long long timestamp) { write(index, static_cast<const char *>(nullptr), timestamp); }

  /*!
    @brief   Writes a signal using a flash name, without a temporary name buffer.

    Accepts the same numeric, RAM-text and flash-text values as the index overloads.
    Text ownership and reporting behavior are unchanged.

    @code
      device.write(F("Temperature"), 23.5);
      device.write(F("Status"), F("Running"));
    @endcode
  */
  void write(const __FlashStringHelper *name, bool value) { write(findSignalIndex(name), value); }
  void write(const __FlashStringHelper *name, byte value) { write(findSignalIndex(name), value); }
  void write(const __FlashStringHelper *name, short value) { write(findSignalIndex(name), value); }
  void write(const __FlashStringHelper *name, unsigned short value) { write(findSignalIndex(name), value); }
  void write(const __FlashStringHelper *name, int value) { write(findSignalIndex(name), value); }
  void write(const __FlashStringHelper *name, unsigned int value) { write(findSignalIndex(name), value); }
  void write(const __FlashStringHelper *name, long value) { write(findSignalIndex(name), value); }
  void write(const __FlashStringHelper *name, unsigned long value) { write(findSignalIndex(name), value); }
  void write(const __FlashStringHelper *name, float value) { write(findSignalIndex(name), value); }
  void write(const __FlashStringHelper *name, double value) { write(findSignalIndex(name), value); }
  void write(const __FlashStringHelper *name, const char *value) { write(findSignalIndex(name), value); }
  void write(const __FlashStringHelper *name, const __FlashStringHelper *value) { write(findSignalIndex(name), value); }
  void write(const __FlashStringHelper *name, decltype(nullptr)) { write(findSignalIndex(name), static_cast<const char *>(nullptr)); }

  /*!
    @brief   Writes a signal using a flash name and an explicit timestamp.

    @param   timestamp  Sample time in microseconds in the configured time base.

    @code
      device.write(F("Status"), F("Running"), 123456ULL);
    @endcode
  */
  void write(const __FlashStringHelper *name, bool value, unsigned long long timestamp) { write(findSignalIndex(name), value, timestamp); }
  void write(const __FlashStringHelper *name, byte value, unsigned long long timestamp) { write(findSignalIndex(name), value, timestamp); }
  void write(const __FlashStringHelper *name, short value, unsigned long long timestamp) { write(findSignalIndex(name), value, timestamp); }
  void write(const __FlashStringHelper *name, unsigned short value, unsigned long long timestamp) { write(findSignalIndex(name), value, timestamp); }
  void write(const __FlashStringHelper *name, int value, unsigned long long timestamp) { write(findSignalIndex(name), value, timestamp); }
  void write(const __FlashStringHelper *name, unsigned int value, unsigned long long timestamp) { write(findSignalIndex(name), value, timestamp); }
  void write(const __FlashStringHelper *name, long value, unsigned long long timestamp) { write(findSignalIndex(name), value, timestamp); }
  void write(const __FlashStringHelper *name, unsigned long value, unsigned long long timestamp) { write(findSignalIndex(name), value, timestamp); }
  void write(const __FlashStringHelper *name, float value, unsigned long long timestamp) { write(findSignalIndex(name), value, timestamp); }
  void write(const __FlashStringHelper *name, double value, unsigned long long timestamp) { write(findSignalIndex(name), value, timestamp); }
  void write(const __FlashStringHelper *name, const char *value, unsigned long long timestamp) { write(findSignalIndex(name), value, timestamp); }
  void write(const __FlashStringHelper *name, const __FlashStringHelper *value, unsigned long long timestamp) { write(findSignalIndex(name), value, timestamp); }
  void write(const __FlashStringHelper *name, decltype(nullptr), unsigned long long timestamp) { write(findSignalIndex(name), static_cast<const char *>(nullptr), timestamp); }

  // Preserve null-name calls without ambiguity between the RAM and flash overloads.
  template<class Name, class T, class = typename blaeck_detail::NullName<Name>::type>
  auto write(Name, T value)
      -> decltype(this->write(static_cast<const char *>(nullptr), value), void())
  {
    write(static_cast<const char *>(nullptr), value);
  }
  template<class Name, class T, class = typename blaeck_detail::NullName<Name>::type>
  auto write(Name, T value, unsigned long long timestamp)
      -> decltype(this->write(static_cast<const char *>(nullptr), value, timestamp), void())
  {
    write(static_cast<const char *>(nullptr), value, timestamp);
  }

  // ----- Command callback -----

  /*!
    @brief   Registers a command whose parameters the handler reads as it likes.

    A host lists the command but can't build a control for it. For a control, use
    onNumberCommand(), onSwitchCommand() or another typed command.

    @param   command  The command name. It can't start with `#`, `@` or `BLAECK.`.
    @param   handler  Called with the parameters as received.

    @note    A command that can't be registered (table full, name too long or
             reserved) is reported on the debug stream and counted in
             hasRejectedCommands(). This applies to every command type.

    @code
      device.onCommand("SwitchLED", onSwitchLED);
    @endcode
  */
  void onCommand(const char *command, BlaeckCommandHandler handler);

  // ----- Typed commands -----
  // Like onCommand(), but the returned handle describes the control, so a host can build one:
  //
  //   device.onNumberCommand("SET_FREQ", onSetFreq)
  //       .withRange(0.0f, 2.0f, 0.01f)
  //       .withUnit(F("Hz"));
  //
  // Values are checked against what is declared before the handler runs.

  /*!
    @brief   Registers a command that takes a number.

    The handler reads the value with atof(params[0]). Text that isn't a number is
    rejected before the handler runs.

    @param   command  The command name.
    @param   handler  Called with an accepted value.
    @return  A handle whose only method is withRange(), which must come first.

    @code
      device.onNumberCommand("SET_FREQ", onSetFreq)
          .withRange(0.0f, 2.0f, 0.01f)
          .withUnit(F("Hz"));
    @endcode
  */
  BLAECK_NODISCARD BlaeckNumberCommandNeedsRange onNumberCommand(const char *command, BlaeckCommandHandler handler);

  /*!
    @brief   Registers a command that switches something on or off.

    The handler gets "0" or "1"; any other value is rejected before it runs.

    @param   command  The command name.
    @param   handler  Called with an accepted value.
    @return  A handle for describing the control.

    @code
      device.onSwitchCommand("SET_ENABLE", onSetEnable)
          .withOwnState(F("Enabled"), &Enabled);
    @endcode
  */
  BlaeckSwitchCommandRef onSwitchCommand(const char *command, BlaeckCommandHandler handler);

  /*!
    @brief   Registers a command that picks one option from a list.

    A host may send the option's name or its index; the handler always gets the
    index, so it reads atoi(params[0]).

    @param   command  The command name.
    @param   handler  Called with an accepted value.
    @return  A handle whose only method is withOptions(), which must come first.

    @code
      device.onSelectCommand("SET_WAVE", onSetWave)
          .withOptions(F("Sine,Square,Triangle,Sawtooth"))
          .withOwnState(F("Wave"), &waveIndex);
    @endcode
  */
  BLAECK_NODISCARD BlaeckSelectCommandNeedsOptions onSelectCommand(const char *command, BlaeckCommandHandler handler);

  /*!
    @brief   Registers a command that is a button press.

    It carries no value unless withPressPayload() gives it one.

    @param   command  The command name.
    @param   handler  Called on each press.
    @return  A handle for describing the control.

    @code
      device.onButtonCommand("STATUS", onStatus);
    @endcode
  */
  BlaeckButtonCommandRef onButtonCommand(const char *command, BlaeckCommandHandler handler);

  /*!
    @brief   Registers a command that takes text.

    The handler gets the text decoded, and never longer than withMaxLength().

    @param   command  The command name.
    @param   handler  Called with an accepted value.
    @return  A handle for describing the control.

    @code
      device.onTextCommand("SET_LABEL", onSetLabel)
          .withMaxLength(sizeof(DeviceLabel) - 1)
          .config();
    @endcode
  */
  BlaeckTextCommandRef onTextCommand(const char *command, BlaeckCommandHandler handler);

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
  int _registerSignal(const char *signalName, dataType type, void *address, bool textInFlash = false);
  int _registerSignal(const __FlashStringHelper *signalName, dataType type, void *address, bool textInFlash = false);
  int _registerCommand(const char *command, BlaeckCommandHandler handler, uint8_t kind);
  int _registerStateChannel(const char *channelName, const __FlashStringHelper *flashName,
                            dataType valueType = Blaeck_string, const void *value = nullptr,
                            bool textInFlash = false);
  int _registerEventChannel(const char *channelName, const __FlashStringHelper *flashName, BlaeckString eventTypes);
  void _writeStateText(const char *name, bool nameInFlash, const char *text, bool textInFlash);
  void _writeStateCurrent(const char *name, bool nameInFlash);
  void _writeStateNumber(const char *channelName, long s, unsigned long u, double d, bool nameInFlash = false);
  // The board's current timestamp, or 0 without a board.
  unsigned long long _timeStamp();
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
    Values the sketch pushes itself, such as state channels, are not resent; send them
    again if they may have changed. Calling it on a device that was not missing changes
    nothing.

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

    @code
      if (readFlowFromPump(pumpFlow))
        pump.writeAll();
    @endcode
  */
  void writeAll();

  /*!
    @brief   Sends every signal of this device now, with a timestamp from the caller.

    @param   timestamp  In microseconds, in the epoch of the timestamp mode.

    @code
      pump.writeAll(1723600000000000ULL);
    @endcode
  */
  void writeAll(unsigned long long timestamp);

private:
  BlaeckDeviceRef(Blaeck *owner, byte id) : BlaeckDeviceBase(owner, id) {}

  friend class Blaeck;
  friend class BlaeckDeviceBase;
  friend class BlaeckSignalRefBase;
  friend class BlaeckCommandRefBase;
  friend class BlaeckStateRefBase;
  friend class BlaeckEventChannelRef;
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
    @brief  The device's name, which a host lists it under. Defaults to "Unnamed".

    An empty string or null is sent as "Unnamed" too.

    @note   Only the pointer is kept. A string literal is fine; a name built at
            runtime must be in a global buffer.

    @code
      device.DeviceName = "Waveform Generator Demo";
    @endcode
  */
  const char *DeviceName = BLAECK_DEVICE_NAME_UNNAMED;

  /*!
    @brief  The hardware's name or revision. Defaults to the selected build target.

    Recognised boards use a friendly name, otherwise the core's ARDUINO_BOARD string
    is used if available, or "n/a". This identifies the target selected when compiling,
    not the physical board or PCB revision. Assign your own value to describe custom
    hardware; begin() does not overwrite it.

    @note   Only the pointer is kept. A string literal is fine; a name built at
            runtime must be in a global buffer.

    @code
      device.DeviceHWVersion = "Weather Station PCB v2";
    @endcode
  */
  const char *DeviceHWVersion;

  /*!
    @brief  The firmware's version. Defaults to "n/a".

    @note   Only the pointer is kept. A string literal is fine; a name built at
            runtime must be in a global buffer.

    @code
      device.DeviceFWVersion = "1.0";
    @endcode
  */
  const char *DeviceFWVersion = "n/a";

  // ----- Signals -----

  /*!
    @brief   Removes every signal, so a new set can be added.

    The table keeps its size. The rejection counts are reset too.

    @warning Call writeSymbols() once the new signals are added. Until then a host
             files values under the old names.

    @code
      device.clearAllSignals();
      device.addSignal(F("Temperature"), &Temperature);
      device.writeSymbols();
    @endcode
  */
  void clearAllSignals();

  /*!
    @brief   Reports whether any signal could not be added.

    That happens when the table is full, or when there wasn't enough RAM to build it.

    @return  True if at least one signal was dropped.

    @code
      if (device.hasRejectedSignals())
        Serial.println(F("Raise withSignals() on the begin() chain."));
    @endcode
  */
  bool hasRejectedSignals() const { return _signalRegistrationFailed; }

  /*!
    @brief   Returns how many signals could not be added.

    @return  How many were dropped.

    @code
      Serial.println(device.getRejectedSignalCount());
    @endcode
  */
  uint16_t getRejectedSignalCount() const { return _rejectedSignalCount; }

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

    Sent once per boot, so a host knows to drop what it held from before. read()
    sends it on its first call; call this only to send it earlier. If a host asks for
    the device list first, the list reports the restart instead, and this sends nothing.

    The state channels, event channels, commands and signal descriptions follow it,
    so a host that stayed connected gets them without asking.

    @code
      device.writeRestarted();
    @endcode
  */
  void writeRestarted();

  // ----- Devices -----

  /*!
    @brief   Sends the device's name and versions, followed by each device from addDevice().

    Each entry also says whether the device is marked missing and whether a restart has
    not been reported yet. The device sends this when a host sends <BLAECK.GET_DEVICES>.

    @code
      device.writeDevices();
    @endcode
  */
  void writeDevices();

  /*!
    @brief   Adds a device that a host shows below this one, such as a second board.

    blaeck only reports the device. The sketch talks to it, over I2C, UART or anything
    else, keeps the variables of its signals up to date, and forwards its commands.
    Register the device's signals, commands and channels through the returned handle,
    the same calls as on the board. Add devices in setup(): a host reads the device list
    when it connects.

    A host names the device after the board and the device name, so keep the name
    unique and stable. Signal and channel names only need to be unique within the board
    or within one device; command names within the whole board. The first addDevice()
    makes the board a "master" in the protocol.

    @param   name  The name a host shows. RAM text is copied; an F() literal stays in flash.
    @return  A handle for the device. If the table is full or the name is empty, the
             device is dropped and the handle ignores every call; hasRejections()
             reports it.

    @code
      BlaeckDeviceRef pump = device.addDevice(F("Pump controller"));
      pump.addSignal(F("Flow"), &pumpFlow).withUnit(F("L/min"));
    @endcode
  */
  BlaeckDeviceRef addDevice(BlaeckString name);

  // ----- Symbols -----

  /*!
    @brief   Sends every signal's name and type.

    A host needs this to read the data. The device also sends it when a host sends
    <BLAECK.WRITE_SYMBOLS>.

    @warning Call it after adding, removing or renaming a signal, or a host files
             values under the wrong names.

    @code
      device.clearAllSignals();
      device.addSignal(F("Temperature"), &Temperature);
      device.writeSymbols();
    @endcode
  */
  void writeSymbols();

  // ----- Signal Config -----

  /*!
    @brief   Sends the signals' units, icons and other descriptions.

    Only signals that describe something are included. The device also sends it
    when a host sends <BLAECK.WRITE_SIGNAL_CONFIG>, and on its own after a
    description changes, so a sketch rarely needs to call it.

    @code
      device.writeSignalConfig();
    @endcode
  */
  void writeSignalConfig();

  // ----- Commands -----

  /*!
    @brief   Sends the list of commands the device accepts.

    Typed commands include their kind, range and options, so a host can build a
    control for each. The device also sends it at startup, after commands change,
    and when a host sends <BLAECK.WRITE_COMMANDS>.

    @code
      device.writeCommands();
    @endcode
  */
  void writeCommands();

  // ----- State channels -----
  // With BLAECK_ENABLE_STATE_CHANNELS=0 these compile but do nothing.

  /*!
    @brief   Removes every state channel, so a new set can be added.

    The table keeps its size. Channels that belong to a command's withOwnState()
    stay; clearAllCommandHandlers() removes those with their commands. The new list
    is sent to the host automatically.

    @code
      device.clearAllStateChannels();
      device.addStateChannel(F("Status"), BlaeckText);
    @endcode
  */
  void clearAllStateChannels();

  /*!
    @brief   Sends the list of state channels, with their current values.

    The device also sends it at startup, after the channels change, and when a host
    sends <BLAECK.WRITE_STATE_CHANNELS>, so a sketch rarely needs to call it.

    @code
      device.writeStateChannels();
    @endcode
  */
  void writeStateChannels();

  // ----- Events -----
  // With BLAECK_ENABLE_EVENTS=0 these compile but do nothing.

  /*!
    @brief   Removes every event channel and event type, so a new set can be added.

    Both tables keep their size. The new list is sent to the host automatically.

    @code
      device.clearAllEventChannels();
      device.addEventChannel(F("Activity"), F("idle_warning,resumed"));
    @endcode
  */
  void clearAllEventChannels();

  /*!
    @brief   Sends the list of event channels and their types.

    The device also sends it at startup, after the channels change, and when a host
    sends <BLAECK.WRITE_EVENT_CHANNELS>, so a sketch rarely needs to call it.

    @code
      device.writeEventChannels();
    @endcode
  */
  void writeEventChannels();

  // ----- Data Write All -----

  /*!
    @brief   Sends every signal's value now, regardless of the interval.

    The device also does this when a host sends <BLAECK.WRITE_DATA>.

    @code
      if (Temperature > 40.0f)
        device.writeAll();
    @endcode
  */
  void writeAll();

  /*!
    @brief   Sends every signal's value now, with a timestamp from the caller.

    @param   timestamp  In microseconds, in the epoch of the timestamp mode.

    @code
      device.writeAll(1723600000000000ULL);
    @endcode
  */
  void writeAll(unsigned long long timestamp);

  /*!
    @brief   Sends signals whose automatic reporting policies are due.

    Interval signals require host activation. Immediate writeOnChange() signals
    do not. A signal eligible through both paths is included only once. Nothing
    is sent when no signal qualifies. Call frequently, separately from read() or
    through tick(). The before-write callback runs only for a due interval.
    The first interval after every ACTIVATE includes all interval-enabled signals
    without change filtering. Pause/resume still applies.

    @code
      device.read();
      device.writeIfDue();
    @endcode
  */
  void writeIfDue();

  /*!
    @brief   Services automatic reporting with a caller-supplied frame timestamp.

    @param   timestamp  Microseconds in the selected timestamp mode's epoch.
                        Scheduling and rate limits still use millis().

    @code
      device.writeIfDue(1723600000000000ULL);
    @endcode
  */
  void writeIfDue(unsigned long long timestamp);

  // ----- Tick -----

  /*!
    @brief   Handles incoming commands, then services automatic signal reporting.

    Most sketches need only this in loop(). It is read() followed by
    writeIfDue().

    @code
      void loop()
      {
        Temperature = readSensor();
        device.tick();
      }
    @endcode
  */
  void tick();

  // ----- Timed Data -----

  /*!
    @brief   Returns the data interval in milliseconds, as the host set it.

    Only the host sets it, with BLAECK.ACTIVATE.

    @return  The interval. Only meaningful while isTimedDataActive() is true.

    @code
      if (device.isTimedDataActive())
        Serial.println(device.getIntervalMs());
    @endcode
  */
  unsigned long getIntervalMs() const { return _timedInterval_ms; }
  /*!
    @brief   Reports whether the host has switched timed data on.

    @return  True after BLAECK.ACTIVATE, until BLAECK.DEACTIVATE.

    @code
      if (!device.isTimedDataActive())
        Serial.println(F("nobody has asked for data yet"));
    @endcode
  */
  bool isTimedDataActive() const { return _timedActivated; }

  // ----- Read  -----

  /*!
    @brief   Handles an incoming command, if one has arrived.

    Runs the built-in BLAECK.* commands and the sketch's handlers. It sends no data,
    so use it instead of tick() in a sketch that only takes commands. Call it on
    every loop() pass.

    @code
      void loop()
      {
        device.read();
      }
    @endcode
  */
  void read();

  // ----- Command callback  -----

  /*!
    @brief   Registers a handler that runs for every command.

    It runs after any matching handler, for logging or forwarding.

    @param   handler  Called for every command.

    @note    With this set, a command that matches no other handler is acknowledged
             as accepted instead of unknown.

    @code
      device.onAnyCommand(onAny);
    @endcode
  */
  void onAnyCommand(BlaeckAnyCommandHandler handler);

  /*!
    @brief   Removes every command, including onAnyCommand().

    The table keeps its size. The state channels those commands had from
    withOwnState() are removed too, and both lists are sent to the host.

    @code
      device.clearAllCommandHandlers();
      device.onSwitchCommand("LED", onLED);
    @endcode
  */
  void clearAllCommandHandlers();

  /*!
    @brief   Reports whether any command failed to register.

    @return  True if at least one was dropped.

    @code
      if (device.hasRejectedCommands())
        Serial.println(F("Raise withCommands() on the begin() chain."));
    @endcode
  */
  bool hasRejectedCommands() const { return _rejectedCommandCount > 0; }

  /*!
    @brief   Returns how many commands could not be registered.

    @return  How many were dropped.

    @code
      Serial.println(device.getRejectedCommandCount());
    @endcode
  */
  uint16_t getRejectedCommandCount() const { return _rejectedCommandCount; }

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

  /*!
    @brief   Copies an F() string into a RAM buffer.

    @param   flash    The F() literal.
    @param   out      The buffer. The copy is cut to fit and always terminated.
    @param   outSize  Size of the buffer, including the terminator.
    @return  How many characters were copied.

    @code
      char name[16];
      device.copyFlashName(F("Temperature"), name, sizeof(name));
    @endcode
  */
  static byte copyFlashName(const __FlashStringHelper *flash, char *out, byte outSize);

  // Stores a channel name: an F() name as its pointer, a RAM name as a heap copy. Frees
  // what the slot held before.
  static bool _setChannelName(const char *&slot, bool &inFlash, const char *ram, const __FlashStringHelper *flash);
  // Equality against a stored channel name, whichever memory it lives in.
  static bool _channelNameEquals(const char *stored, bool inFlash, const char *candidate);
  static bool _channelNameEqualsFlash(const char *stored, bool inFlash, const __FlashStringHelper *candidate);

  /*!
    @brief   Reports whether any state channel could not be added.

    That happens when the table is full, the name is too long, or a command's
    withOwnState() already uses the name. A command's own channel counts too; if
    it can't be added, the command reports no value.

    @return  True if at least one was dropped.

    @code
      if (device.hasRejectedStateChannels())
        Serial.println(F("Raise withStateChannels() on the begin() chain."));
    @endcode
  */
  bool hasRejectedStateChannels() const { return _rejectedStateChannelCount > 0; }
  /*!
    @brief   Returns how many state channels could not be added.

    @return  How many were dropped.

    @code
      Serial.println(device.getRejectedStateChannelCount());
    @endcode
  */
  uint16_t getRejectedStateChannelCount() const { return _rejectedStateChannelCount; }

  /*!
    @brief   Reports whether any event channel or event type could not be added.

    printRejections() or the debug stream says which.

    @return  True if at least one was dropped.

    @code
      if (device.hasRejectedEventChannels())
        device.printRejections(&Serial);
    @endcode
  */
  bool hasRejectedEventChannels() const
  {
    return _rejectedEventChannelCount > 0 || _rejectedEventTypeCount > 0;
  }
  /*!
    @brief   Returns how many event channels and event types could not be added.

    @return  Channels and types together.

    @code
      Serial.println(device.getRejectedEventChannelCount());
    @endcode
  */
  uint16_t getRejectedEventChannelCount() const
  {
    return (uint16_t)(_rejectedEventChannelCount + _rejectedEventTypeCount);
  }

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

    Prints nothing when there were no rejections. A rejection can mean a full table,
    an invalid declaration or insufficient memory; increasing capacity may not help.
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
    @brief   Copies the name of a select command's option at a given position.

    @param   command  The select command's name.
    @param   index    Position in the withOptions() list, starting at 0.
    @param   out      Where the name is copied. Left empty if this returns false.
    @param   outSize  Size of out, including the terminator.
    @return  False if the command is not a select, the index is past the end, or the
             name doesn't fit. A name is never cut short.

    @code
      char name[12];
      device.getSelectOptionNameAt("SET_WAVE", waveIndex, name, sizeof(name));
    @endcode
  */
  bool getSelectOptionNameAt(const char *command, byte index, char *out, byte outSize) const;

  /*!
    @brief   Returns the position of an option in a select command's list.

    Case-sensitive.

    @param   command     The select command's name.
    @param   optionName  The option to look for.
    @return  Its position, starting at 0, or -1 if the command isn't a select or has
             no such option.
    @note    Useful for restoring a setting saved as a name. A saved index would point
             at the wrong option if a later firmware reorders the list.

    @code
      char saved[12];
      EEPROM.get(addr, saved);
      long i = device.getSelectOptionIndexOf("SET_WAVE", saved);
      waveIndex = (i >= 0) ? (byte)i : 0;
    @endcode
  */
  long getSelectOptionIndexOf(const char *command, const char *optionName) const;

  /*!
    @brief   Sets a function to refresh values before interval and full snapshots.

    Runs before filtering a due host interval, even if no signals qualify, and before
    writeAll(), including host requests. It does not run for single-signal write()
    or the every-tick writeOnChange() check. It runs from loop(), not an interrupt.

    @param   callback  The refresh function, or nullptr to remove it.

    @code
      device.setBeforeWriteCallback(readAllSensors);
    @endcode
  */
  void setBeforeWriteCallback(void (*callback)());

  /*!
    @brief   Sets whether and how data is timestamped.

    BLAECK_NO_TIMESTAMP, the default, sends none and the host uses arrival time.
    BLAECK_MICROS uses micros(), extended so it keeps counting past its 71-minute
    rollover. BLAECK_UNIX needs a clock from setTimestampCallback().

    @param   mode  A BlaeckTimestampMode value.

    @warning Set it in setup(). Changing it later restarts the count, so timestamps
             before and after don't line up.

    @note    BLAECK_MICROS only notices a rollover when data is sent, so it needs
             data at least every 71 minutes. Otherwise use BLAECK_UNIX.

    @code
      device.setTimestampMode(BLAECK_MICROS);
    @endcode
  */
  void setTimestampMode(BlaeckTimestampMode mode);

  /*!
    @brief   Sets the clock BLAECK_UNIX reads, such as an RTC or NTP time.

    It can be set before or after setTimestampMode().

    @param   callback  Returns microseconds since the Unix epoch.

    @code
      device.setTimestampCallback(unixMicros);
      device.setTimestampMode(BLAECK_UNIX);
    @endcode
  */
  void setTimestampCallback(unsigned long long (*callback)());

  /*!
    @brief   Returns the timestamp mode.

    @return  The mode as set, even if BLAECK_UNIX has no clock yet.

    @code
      if (device.getTimestampMode() == BLAECK_NO_TIMESTAMP)
        Serial.println(F("data carries no time"));
    @endcode
  */
  BlaeckTimestampMode getTimestampMode() const { return _timestampMode; }

  /*!
    @brief   Reports whether timestamps will be real.

    @return  True if a timestamp mode is set and it has a clock to read.

    @warning BLAECK_UNIX without a clock sends 0 as every timestamp.

    @code
      if (!device.hasValidTimestampCallback())
        Serial.println(F("no clock - timestamps will be zero"));
    @endcode
  */
  bool hasValidTimestampCallback() const;

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
      device.begin(Serial).withSignals(2);
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
    @return  A handle for setting the number of connections, table sizes and a debug
             stream. Each has a default, so the handle can be ignored.

    @code
      server.begin();
      device.begin(server)
          .withClients(4)
          .withSignals(50)
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
    TCP client storage is allocated on the first read() or tick(), so check afterward.

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
      device.setClientConnectedCallback(onClientConnected);
    @endcode
  */
  void setClientConnectedCallback(void (*callback)(byte clientNo));

  /*!
    @brief   Sets a function to call when a connection closes.

    A connection that dies without closing, such as one whose cable was pulled, is
    noticed only when the network stack gives up on it.

    @param   callback  Receives the connection's slot, starting at 0.

    @code
      device.setClientDisconnectedCallback(onClientDisconnected);
    @endcode
  */
  void setClientDisconnectedCallback(void (*callback)(byte clientNo));

protected:
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
  // Frees the name copies and metadata records the signal table owns. Must run while
  // _signalCapacity still describes the allocated table.
  void _freeSignalOwned();
#if BLAECK_ENABLE_SIGNAL_META
  // The signal's metadata record, allocated on first use. nullptr if the handle is dead or
  // there is no memory.
  SignalMeta *_ensureSignalMeta(int16_t index);
#endif
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
  SignalReporting *_ensureSignalReporting(int16_t index);
  void _reportSignalPolicyError(const __FlashStringHelper *message);
  void _resetReportingBaselines();
  bool _prepareSignalSnapshot(Signal &signal);
  void _captureSignalSnapshot(Signal &signal);
  bool _signalChanged(const Signal &signal, double delta) const;

  void writeAll(unsigned long messageID, unsigned long long timestamp);

  // Store a value in a signal, converted to its type. False if there is no such signal or it
  // holds text.
  bool _storeSigned(int signalIndex, long value);
  bool _storeUnsigned(int signalIndex, unsigned long value);
  bool _storeFloating(int signalIndex, double value);

  void writeData(unsigned long messageID, int signalIndex_start, int signalIndex_end, bool selectedOnly, unsigned long long timestamp);
  void writeDataFrame(unsigned long MessageID, int signalIndex_start, int signalIndex_end, bool selectedOnly, unsigned long long timestamp, bool intervalReport = false);

  // Forms that echo the message id of the request they answer. Only read() has one.
  void writeRestarted(unsigned long messageID);
  void writeDevices(unsigned long messageID);
  void writeSymbols(unsigned long messageID);
  void writeSignalConfig(unsigned long messageID);
  void writeCommands(unsigned long messageID);
  void writeStateChannels(unsigned long messageID);
  void writeEventChannels(unsigned long messageID);

  void writeSymbolsFrame(unsigned long MessageID);
#if BLAECK_ENABLE_SIGNAL_META
  void writeSignalConfigFrame(unsigned long MessageID);
#endif
  // Add a signal and return its index, or -1 if it was rejected. All addSignal() overloads
  // end up here.
  int _registerSignal(byte deviceId, const char *signalName, dataType type, void *address, bool textInFlash = false);
  int _registerSignal(byte deviceId, const __FlashStringHelper *signalName, dataType type, void *address, bool textInFlash = false);
  // Exactly one of ram and flash is non-null.
  int _registerSignalCommon(byte deviceId, const char *ram, const __FlashStringHelper *flash,
                            dataType type, void *address, bool textInFlash);
  void _writeSignalText(int signalIndex, const void *value, bool inFlash, unsigned long long timestamp);
  void _emitTextBytes(const void *text, bool inFlash, size_t length);
  void _writeCommandState(const char *command, bool inFlash);
  // The lookups behind BlaeckDeviceBase's findSignalIndex(), addEventType() and writeEvent().
  int _findSignalIndex(byte deviceId, const char *signalName);
  int _findSignalIndex(byte deviceId, const __FlashStringHelper *signalName);
  bool _addEventType(byte deviceId, const char *channelName, BlaeckString eventType);
  void _writeEvent(byte deviceId, const char *channelName, BlaeckString eventType);
  // Registers a command and returns its table index, or -1 if it was rejected (counted, and
  // reported on the debug stream).
  int _registerCommand(byte deviceId, const char *command, BlaeckCommandHandler handler, uint8_t kind);
  // Resets an entry's metadata, so registering a name again starts from scratch.
  void _resetCommandMeta(uint16_t handlerIndex, uint8_t kind);
  // Adds a state channel and returns its index, or -1 if it was rejected. Adding an existing
  // name reuses its slot with the metadata cleared. Exactly one of channelName and flashName
  // is set; a flash name is kept as a pointer, a RAM name is copied.
  int _registerStateChannel(byte deviceId, const char *channelName, const __FlashStringHelper *flashName, dataType valueType = Blaeck_string,
                              const void *value = nullptr, bool textInFlash = false);
  void _writeStateText(byte deviceId, const char *name, bool nameInFlash, const char *text, bool textInFlash);
  void _writeStateCurrent(byte deviceId, const char *name, bool nameInFlash);
  void _writeStateNumber(byte deviceId, const char *channelName, long s, unsigned long u, double d, bool nameInFlash = false);
  // As _registerStateChannel(). A redeclared event channel keeps its types.
  int _registerEventChannel(byte deviceId, const char *channelName, const __FlashStringHelper *flashName, BlaeckString eventTypes);
  // Adds one event type per comma-separated field, in order.
  void _addEventTypesCsv(uint16_t channelIndex, const detail::StoredString &eventTypes);
#if BLAECK_ENABLE_COMMAND_META
  void writeCommandsFrame(unsigned long MessageID);
  byte _validateTypedCommand(uint16_t handlerIndex);
  // Adds the channel a command's withOwnState() uses. addStateChannel() refuses such names.
  bool _addOwnedStateChannel(byte deviceId, BlaeckString channelName, BlaeckStateTextGetter getStateText,
                             dataType valueType = Blaeck_string, const void *value = nullptr);
  // Adds the withOwnState() channel and marks the catalogs for sending. False if the channel
  // couldn't be added, and the command then reports no state.
  bool _declareOwnState(uint16_t handlerIndex, BlaeckString channelName,
                        BlaeckStateTextGetter getStateText, dataType valueType, const void *value,
                        bool selectIndex = false);
  bool _declareOwnState(uint16_t handlerIndex, BlaeckString channelName,
                        BlaeckStateTextGetter getStateText);

  static void _percentDecodeInPlace(char *s);
  static long _flashCsvIndexOf(BlaeckString csv, const char *value);
#endif
  // Number of fields in a comma-separated string. Outside the command-metadata guard
  // because event channels use it too.
  static uint16_t _flashCsvOptionCount(BlaeckString csv);

  // True if any field is empty or only spaces. Such a list is refused, because dropping the
  // field would shift every later field's index.
  static bool _flashCsvHasBlankField(BlaeckString csv);
#if BLAECK_ENABLE_STATE_CHANNELS
  void writeStateChannelsFrame(unsigned long MessageID);
  // Index of a declared channel, or -1 when the name was never declared.
  int _findStateChannel(byte deviceId, const char *channelName) const;
  int _findStateChannel(byte deviceId, const __FlashStringHelper *channelName) const;
  int _findStateChannel(byte deviceId, BlaeckString channelName) const
  {
    return channelName.inFlash() ? _findStateChannel(deviceId, reinterpret_cast<const __FlashStringHelper *>(channelName.data()))
                                 : _findStateChannel(deviceId, channelName.data());
  }
#endif
#if BLAECK_ENABLE_EVENTS
  void writeEventChannelsFrame(unsigned long MessageID);
  // Index of a declared event channel, or -1 when the name was never declared.
  int _findEventChannel(byte deviceId, const char *channelName) const;
  int _findEventChannel(byte deviceId, const __FlashStringHelper *channelName) const;
  // Position of an event type within its own channel's list, or -1 when that
  // channel never declared it.
  int _findEventType(uint16_t channelIndex, BlaeckString eventType) const;
  // Compares two flash strings. strcmp_P() can't, because it reads its first argument from RAM.
  static bool _flashStringEquals(const __FlashStringHelper *a, const __FlashStringHelper *b);
#endif

  void writeDevicesFrame(unsigned long MessageID);

  // Sends a catalog with no entries, the answer when that feature is compiled out.
  void _writeEmptyFrame(byte msgKey, unsigned long msg_id);

  static void validatePlatformSizes();

  Print *_debugStream = nullptr;
  bool _storeString(detail::StoredString &slot, BlaeckString value);
  uint16_t _rejectedStringCount = 0;
  Signal *Signals = nullptr;
  // Allocates the signal table on first use.
  bool _ensureSignalTable();
  int _signalIndex = 0;
  unsigned int _signalCapacity = 0;
  bool _signalRegistrationFailed = false;
  uint16_t _rejectedSignalCount = 0;
  uint16_t _rejectedSignalPolicyCount = 0;
#if BLAECK_ENABLE_SIGNAL_META
  // Signal descriptions that couldn't be stored for lack of heap. Counted apart, because no
  // table size fixes it.
  uint16_t _rejectedSignalMetaCount = 0;
#endif
  uint16_t _rejectedCommandCount = 0;
  uint16_t _rejectedStateChannelCount = 0;
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

  // ── Table sizes ───────────────────────────────────────────────────
  enum TableId
  {
    TABLE_SIGNALS,
    TABLE_STATE_CHANNELS,
    TABLE_EVENT_CHANNELS,
    TABLE_EVENT_TYPES,
    TABLE_COMMANDS,
    TABLE_DEVICES
  };
  void _setTableCapacity(TableId table, unsigned int count);
  // For a full table, prints what was dropped and which original begin() setting to raise.
  void _warnTableFull(const __FlashStringHelper *table, unsigned int capacity,
                      const char *droppedName);
  void _warnTableFull(const __FlashStringHelper *table, unsigned int capacity,
                      const __FlashStringHelper *droppedName);
  // One line of printRejections(), for a table that dropped something.
  void _printRejectionLine(Print *out, const __FlashStringHelper *what,
                           uint16_t dropped, unsigned int capacity);

  // The largest size any table accepts. Handles store their index as int16_t, with negative
  // values meaning rejected. In practice RAM runs out long before this.
  static const uint16_t MAX_TABLE_ENTRIES = INT16_MAX;

  // Default table sizes, changed with the begin() chain. A table is allocated whole when its
  // first entry is added.
#if defined(__AVR__)
  #if defined(RAMEND) && (RAMEND >= 0x10FF)
    static const unsigned int DEFAULT_SIGNALS = 24;
    static const byte DEFAULT_STATE_CHANNELS = 8;
    static const byte DEFAULT_EVENT_CHANNELS = 6;
    static const byte DEFAULT_EVENT_TYPES = 20;
    static const byte DEFAULT_COMMANDS = 16;
    static const byte DEFAULT_DEVICES = 4;
  #else
    static const unsigned int DEFAULT_SIGNALS = 8;
    static const byte DEFAULT_STATE_CHANNELS = 3;
    static const byte DEFAULT_EVENT_CHANNELS = 2;
    static const byte DEFAULT_EVENT_TYPES = 8;
    static const byte DEFAULT_COMMANDS = 6;
    static const byte DEFAULT_DEVICES = 2;
  #endif
#else
  static const unsigned int DEFAULT_SIGNALS = 64;
  static const byte DEFAULT_STATE_CHANNELS = 32;
  static const byte DEFAULT_EVENT_CHANNELS = 24;
  static const byte DEFAULT_EVENT_TYPES = 64;
  static const byte DEFAULT_COMMANDS = 32;
  static const byte DEFAULT_DEVICES = 8;
#endif
  // Slave IDs are one byte and 0 is the board itself.
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
  // Longest state and event channel names, terminator included. Defined even when those
  // features are off, because the F() overloads still compile.
#if defined(__AVR__)
  static const byte MAX_STATE_NAME_COUNT = 16;
#else
  static const byte MAX_STATE_NAME_COUNT = 32;
#endif
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
  bool _writesPaused = false;
  bool _writesPausedForever = false;
  unsigned long _writesPausedUntil = 0;

  // Checked once per frame, when it opens, so a pause never cuts a frame in half.
  bool _mayWriteFrame()
  {
    if (!_transportReady())
      return false;

    if (_writesPausedForever)
      return false;

    if (_writesPaused)
    {
      // Signed difference, so this survives the millis() rollover.
      if ((long)(millis() - _writesPausedUntil) < 0)
        return false;

      _writesPaused = false;
    }

    return true;
  }

  void _setWritesPaused(unsigned long ms)
  {
    if (ms == 0)
      ms = BLAECK_PAUSE_WRITES_DEFAULT_MS;
    if (ms > BLAECK_PAUSE_WRITES_MAX_MS)
      ms = BLAECK_PAUSE_WRITES_MAX_MS;

    _writesPaused = true;
    _writesPausedUntil = millis() + ms;
  }

  void _setWritesPausedForever()
  {
    _writesPausedForever = true;
    _writesPaused = false;
  }

  // DeviceName, or "Unnamed" when it is null or empty.
  const char *_deviceName() const
  {
    return (DeviceName == nullptr || DeviceName[0] == '\0')
               ? BLAECK_DEVICE_NAME_UNNAMED
               : DeviceName;
  }

  void _clearWritesPaused()
  {
    _writesPaused = false;
    _writesPausedForever = false;
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

  // Starts a frame. False if no frame may be written (no host yet, or writes paused).
  bool _frameOpen(byte msgKey, unsigned long msgId, bool withCrc = false);
  // False if buffering failed or the transport did not accept every byte.
  bool _frameClose();
  uint32_t _frameCrcEnd()
  {
    _frameCrcOn = false;
    return _crc.calc();
  }
  void _emitByte(byte b)
  {
    if (_frameCrcOn)
      _crc.add(b);
    if (_frameDirect)
      _writeDirect(&b, 1);
    else if (_bufEnsure(1))
      _frameBuf[_framePos++] = b;
    else
      _bufOverflow = true;
  }
  void _emitBytes(const byte *data, size_t len)
  {
    if (_frameCrcOn)
      _crc.add(data, len);
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
  // Sends each device's pending C1 notices; stops at the first that can't be sent.
  void _writeDeviceNotices();
  // One C1 frame; false if it could not be sent.
  bool _writeDeviceNotice(byte deviceId, byte event);
  // write() of one signal: dropped, with a debug note, while its device is missing.
  void _writeSignalNow(int signalIndex, unsigned long long timestamp);
  // writeAll() for one device from addDevice().
  void _writeDeviceSignals(byte deviceId, unsigned long long timestamp);
  // The master/slave and slave ID bytes for an entry of the given device. The board is
  // "single" without devices and "master" with them; a device is a "slave" with its ID.
  void _emitOwner(byte deviceId)
  {
    _emitByte(deviceId != 0 ? 0x02 : (_deviceCount > 0 ? 0x01 : 0x00));
    _emitByte(deviceId);
  }

  static unsigned long long _microsWrapper()
  {
    return (unsigned long long)micros();
  }

  typedef blaeck_detail::CommandHandlerEntry CommandHandlerEntry;
  CommandHandlerEntry *_commandHandlers = nullptr;
  uint16_t _commandCapacity = DEFAULT_COMMANDS;
  // Entries that exist: the capacity once the table is allocated, 0 before. Loops over a
  // table stop here.
  uint16_t _commandSlots() const { return _commandHandlers != nullptr ? _commandCapacity : 0; }
  // Allocates the table on first use. False if there isn't enough RAM.
  bool _ensureCommandTable();

  // ── Devices ───────────────────────────────────────────────────────
  // Added in order and never removed, so a device's slave ID is its index plus one.
  typedef blaeck_detail::DeviceEntry DeviceEntry;
  DeviceEntry *_devices = nullptr;
  uint16_t _deviceCapacity = DEFAULT_DEVICES;
  byte _deviceCount = 0;
  bool _ensureDeviceTable();
  // The entry for a slave ID, or nullptr for 0 or an ID never handed out.
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
  // Needed even with BLAECK_ENABLE_STATE_CHANNELS=0, because the state handles still compile.
  typedef blaeck_detail::StateChannelEntry StateChannelEntry;

  // Set when a catalog has changed since it was last sent; _flushCatalogs() sends it.
  bool _commandCatalogDirty = false;
#if BLAECK_ENABLE_SIGNAL_META
  bool _signalConfigDirty = false;
#endif

  // Sends each catalog that changed since it was last sent. Called after anything that can
  // change one, and before a state or event push. Never sends the symbol list: a host lays out
  // its storage by it, so a changed list mid-session must be sent by the sketch on purpose.
  void _flushCatalogs();

  // The datatype's code in the symbol list. Used by the schema hash too, so it exists
  // without state channels.
  static byte _dtypeCode(dataType t);
#if BLAECK_ENABLE_STATE_CHANNELS
  StateChannelEntry *_stateChannels = nullptr;
  uint16_t _stateChannelCapacity = DEFAULT_STATE_CHANNELS;
  uint16_t _stateChannelSlots() const { return _stateChannels != nullptr ? _stateChannelCapacity : 0; }
  bool _ensureStateChannelTable();
  // A text channel's current value, or nullptr if it has none. buf is used only when an
  // option index has to be turned into its name.
  const char *_channelText(const StateChannelEntry &e, char *buf, byte bufSize, bool *inFlash = nullptr) const;

  // Prints prefix and the channel's name to the debug stream, if there is one.
  void _debugChannel(const __FlashStringHelper *prefix, const StateChannelEntry &e) const;

  // Returns text if it is one of the channel's options, else nullptr (warning once).
  const char *_checkedSelectName(const StateChannelEntry &e, const char *text) const;

  // The 0x90 flag word for one channel.
  uint16_t _stateChannelFlags(const StateChannelEntry &e, bool hasStateValue) const;

  // Writes a numeric channel's value into out (at most 8 bytes) and returns its length. 0 for
  // a text channel or one with no value.
  byte _channelValueBytes(const StateChannelEntry &e, byte *out);

  // Compares a flash string with a RAM string.
  static bool _flashStringEqualsName(const __FlashStringHelper *flashName, const char *name);
  // Sends a state value. writeState() checks the channel first; writeCommandState() calls this
  // directly for a command's own channel.
  void _writeStateFrame(int channelIndex, const char *text, const byte *pushed = nullptr, byte pushedLen = 0, bool textInFlash = false);
  // A pushed number converted to the channel's type, as bytes.
  byte _valueBytes(dataType declared, long s, unsigned long u, double d, byte *out);
  // Finds the channel for a writeState() push, or returns -1 (with a warning) if the push is
  // refused.
  int _stateChannelForPush(byte deviceId, const char *channelName, bool wantText, bool nameInFlash = false);
#endif
#if BLAECK_ENABLE_EVENTS
  typedef blaeck_detail::EventChannelEntry EventChannelEntry;
  EventChannelEntry *_eventChannels = nullptr;
  uint16_t _eventChannelCapacity = DEFAULT_EVENT_CHANNELS;
  uint16_t _eventChannelSlots() const { return _eventChannels != nullptr ? _eventChannelCapacity : 0; }
  bool _ensureEventChannelTable();

  // One table of event types for all channels. Each entry names its channel; a type's index
  // is its position among its channel's entries.
  typedef blaeck_detail::EventTypeEntry EventTypeEntry;
  static const byte WHOLE_STRING = blaeck_detail::WHOLE_STRING;

  // Where the entry's name starts in its stored string, and its length.
  static void _eventTypeExtent(const EventTypeEntry &e, unsigned int &start, unsigned int &len);
  // Compares the entry's name with eventType.
  static bool _eventTypeEquals(const EventTypeEntry &e, BlaeckString eventType);
  // Emits the entry's name with a terminator.
  void _emitEventType0(const EventTypeEntry &e);
  EventTypeEntry *_eventTypes = nullptr;
  uint16_t _eventTypeCapacity = DEFAULT_EVENT_TYPES;
  uint16_t _eventTypeSlots() const { return _eventTypes != nullptr ? _eventTypeCapacity : 0; }
  bool _ensureEventTypeTable();
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
#if BLAECK_ENABLE_STATE_CHANNELS
  bool _stateCatalogDirty = false;
#endif
#if BLAECK_ENABLE_EVENTS
  bool _eventCatalogDirty = false;
#endif
#if BLAECK_ENABLE_COMMAND_META
  // A select value sent by name, rewritten as its index for the handler.
  char _selectIndexScratch[8] = {0};
#endif

  void (*_beforeWriteCallback)() = nullptr;

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

  friend class BlaeckSignalRefBase;
  friend bool blaeck_detail::optionsAccepted(BlaeckString, Print *,
                                             const char *, bool);
  friend class BlaeckCommandRefBase;
  friend class BlaeckStateRefBase;
  friend class BlaeckEventChannelRef;
  friend class BlaeckBeginRef;
  friend class BlaeckDeviceRef;
  friend class BlaeckDeviceBase;

private:
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
// declarations. (Keep the blank line below, or this comment becomes withSignals()'s hover.)

// A literal size above 32767 fails the build: GCC removes the call when the check is false
// and fails the link with this message when it is true.
#if defined(__GNUC__) && !defined(__clang__)
extern void blaeck_capacity_above_32767() __attribute__((error(
    "BLAECK: a table capacity above 32767 cannot be indexed - a handle names its slot "
    "with an int16_t. Ask for 32767 or fewer.")));
  #define BLAECK_CHECK_CAPACITY(count)                                     \
    do {                                                                   \
      if (__builtin_constant_p(count) &&                                   \
          (unsigned long)(count) > (unsigned long)Blaeck::MAX_TABLE_ENTRIES) \
        blaeck_capacity_above_32767();                                     \
    } while (0)
#else
  #define BLAECK_CHECK_CAPACITY(count) do { } while (0)
#endif

inline BlaeckBeginRef &BlaeckBeginRef::withClients(byte count)
{
  if (_owner != nullptr)
    _owner->_setMaxClients(count);
  return *this;
}

inline BlaeckBeginRef &BlaeckBeginRef::withSignals(unsigned int count)
{
  BLAECK_CHECK_CAPACITY(count);
  if (_owner != nullptr)
    _owner->_setTableCapacity(Blaeck::TABLE_SIGNALS, count);
  return *this;
}

inline BlaeckBeginRef &BlaeckBeginRef::withStateChannels(unsigned int count)
{
  BLAECK_CHECK_CAPACITY(count);
#if BLAECK_ENABLE_STATE_CHANNELS
  if (_owner != nullptr)
    _owner->_setTableCapacity(Blaeck::TABLE_STATE_CHANNELS, count);
#else
  (void)count;
#endif
  return *this;
}

inline BlaeckBeginRef &BlaeckBeginRef::withEventChannels(unsigned int count)
{
  BLAECK_CHECK_CAPACITY(count);
#if BLAECK_ENABLE_EVENTS
  if (_owner != nullptr)
    _owner->_setTableCapacity(Blaeck::TABLE_EVENT_CHANNELS, count);
#else
  (void)count;
#endif
  return *this;
}

inline BlaeckBeginRef &BlaeckBeginRef::withEventTypes(unsigned int count)
{
  BLAECK_CHECK_CAPACITY(count);
#if BLAECK_ENABLE_EVENTS
  if (_owner != nullptr)
    _owner->_setTableCapacity(Blaeck::TABLE_EVENT_TYPES, count);
#else
  (void)count;
#endif
  return *this;
}

inline BlaeckBeginRef &BlaeckBeginRef::withCommands(unsigned int count)
{
  BLAECK_CHECK_CAPACITY(count);
  if (_owner != nullptr)
    _owner->_setTableCapacity(Blaeck::TABLE_COMMANDS, count);
  return *this;
}

inline BlaeckBeginRef &BlaeckBeginRef::withDevices(unsigned int count)
{
  if (_owner != nullptr)
    _owner->_setTableCapacity(Blaeck::TABLE_DEVICES, count);
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
    _owner->_commandCatalogDirty = true;
}

inline void BlaeckCommandRefBase::_warnRangeIgnored(float mn, float mx) const
{
#if BLAECK_ENABLE_COMMAND_META
  if (_owner == nullptr || _owner->_debugStream == nullptr)
    return;
  _owner->_debugStream->print(F("withRange ignored, max must be above min: "));
  if (auto *e = _entry())
  {
    _owner->_debugStream->print(e->command);
    _owner->_debugStream->print(' ');
  }
  _owner->_debugStream->print('[');
  _owner->_debugStream->print(mn);
  _owner->_debugStream->print(F(", "));
  _owner->_debugStream->print(mx);
  _owner->_debugStream->println(F("]. Any value is accepted and no range is declared."));
#else
  (void)mn;
  (void)mx;
#endif
}

inline void BlaeckCommandRefBase::_warnStepIgnored(float st) const
{
#if BLAECK_ENABLE_COMMAND_META
  if (_owner == nullptr || _owner->_debugStream == nullptr)
    return;
  _owner->_debugStream->print(F("step ignored, must be above zero: "));
  if (auto *e = _entry())
  {
    _owner->_debugStream->print(e->command);
    _owner->_debugStream->print(' ');
  }
  _owner->_debugStream->print(st);
  _owner->_debugStream->println(F(". No resolution is declared and the host chooses one."));
#else
  (void)st;
#endif
}

inline void BlaeckCommandRefBase::_warnStepTooFine(float st) const
{
#if BLAECK_ENABLE_COMMAND_META
  if (_owner == nullptr || _owner->_debugStream == nullptr)
    return;
  _owner->_debugStream->print(F("step below 0.001: "));
  if (auto *e = _entry())
  {
    _owner->_debugStream->print(e->command);
    _owner->_debugStream->print(' ');
  }
  // Six places: the default two would print it as 0.00.
  _owner->_debugStream->print(st, 6);
  _owner->_debugStream->println(F(". Sent as declared, but Home Assistant refuses the whole "
                                  "control rather than only the step."));
#else
  (void)st;
#endif
}

inline void BlaeckCommandRefBase::_warnMaxLengthTooLong(unsigned int maxLength) const
{
#if BLAECK_ENABLE_COMMAND_META
  if (_owner == nullptr || _owner->_debugStream == nullptr)
    return;
  _owner->_debugStream->print(F("max length above 255: "));
  if (auto *e = _entry())
  {
    _owner->_debugStream->print(e->command);
    _owner->_debugStream->print(' ');
  }
  _owner->_debugStream->print(maxLength);
  _owner->_debugStream->println(F(". Ignored, and 255 kept: Home Assistant caps an entity's "
                                  "state at 255 characters and refuses the control outright "
                                  "above it."));
#else
  (void)maxLength;
#endif
}

inline bool BlaeckCommandRefBase::_optionsAccepted(BlaeckString optionsCsv) const
{
#if BLAECK_ENABLE_COMMAND_META
  auto *e = _entry();
  return blaeck_detail::optionsAccepted(optionsCsv,
                                        _owner != nullptr ? _owner->_debugStream : nullptr,
                                        e != nullptr ? e->command : nullptr, false);
#else
  (void)optionsCsv;
  return false;
#endif
}

inline void BlaeckCommandRefBase::_setOwnState(BlaeckString channelName,
                                               dataType valueType, const void *value,
                                               bool selectIndex)
{
#if BLAECK_ENABLE_COMMAND_META && BLAECK_ENABLE_STATE_CHANNELS
  if (auto *e = _entry())
  {
    detail::StoredString name = e->stateSignal;
    if (!_storeString(name, channelName))
      return;
    if (_owner->_declareOwnState((uint16_t)_index, name, nullptr, valueType, value, selectIndex))
    {
      const bool changed = e->stateSignal != BlaeckString(name) || e->stateSource != BLAECK_STATE_CHANNEL;
      e->stateSignal = name;
      e->stateSource = BLAECK_STATE_CHANNEL;
      if (changed)
        _markDirty();
    }
  }
#else
  (void)channelName;
  (void)valueType;
  (void)value;
  (void)selectIndex;
#endif
}

inline void BlaeckCommandRefBase::_setOwnState(BlaeckString channelName, BlaeckStateTextGetter getStateText)
{
#if BLAECK_ENABLE_COMMAND_META && BLAECK_ENABLE_STATE_CHANNELS
  if (auto *e = _entry())
  {
    detail::StoredString name = e->stateSignal;
    if (!_storeString(name, channelName))
      return;
    // Link the state only if the channel was added; otherwise the command reports none.
    if (_owner->_declareOwnState((uint16_t)_index, name, getStateText))
    {
      const bool changed = e->stateSignal != BlaeckString(name) || e->stateSource != BLAECK_STATE_CHANNEL;
      e->stateSignal = name;
      e->stateSource = BLAECK_STATE_CHANNEL;
      if (changed)
        _markDirty();
    }
  }
#else
  (void)channelName;
  (void)getStateText;
#endif
}

inline void BlaeckSignalRefBase::_setInterval(BlaeckIntervalMode mode, double delta)
{
  if (_owner != nullptr)
    _owner->_setSignalInterval(_index, mode, delta);
}

inline bool BlaeckCommandRefBase::_storeString(detail::StoredString &slot, BlaeckString value)
{
  return _owner != nullptr && _owner->_storeString(slot, value);
}

inline bool BlaeckStateRefBase::_storeString(detail::StoredString &slot, BlaeckString value)
{
  return _owner != nullptr && _owner->_storeString(slot, value);
}

inline void BlaeckSignalRefBase::_setOnChange(double delta, uint32_t minIntervalMs)
{
  if (_owner != nullptr)
    _owner->_setSignalOnChange(_index, delta, minIntervalMs);
}

inline void BlaeckSignalRefBase::_setOnChange(BlaeckIntervalMode mode)
{
  if (_owner != nullptr)
    _owner->_setSignalOnChange(_index, mode);
}

inline void BlaeckSignalRefBase::_setFlash(BlaeckString value, uint16_t bit)
{
#if BLAECK_ENABLE_SIGNAL_META
  // An empty string counts as not set.
  if (blaeck_detail::flashStrEmpty(value))
    value = nullptr;
  if (SignalMeta *m = _owner != nullptr ? _owner->_ensureSignalMeta(_index) : nullptr)
  {
    detail::StoredString *slot;
    switch (bit)
    {
    case BLAECK_SIG_HAS_UNIT:         slot = &m->Unit; break;
    case BLAECK_SIG_HAS_DEVICE_CLASS: slot = &m->DeviceClass; break;
    case BLAECK_SIG_HAS_DISPLAY_NAME: slot = &m->DisplayName; break;
    default:                          slot = &m->Icon; break;
    }
    const uint16_t flags = (value != nullptr) ? (uint16_t)(m->MetaFlags | bit)
                                              : (uint16_t)(m->MetaFlags & ~bit);

    // Only a real change marks the catalog, since this may run on every loop() pass.
    if (*slot != value || m->MetaFlags != flags)
    {
      if (!_owner->_storeString(*slot, value))
        return;
      m->MetaFlags = flags;
      _owner->_signalConfigDirty = true;
    }
  }
#else
  (void)value;
  (void)bit;
#endif
}

inline void BlaeckSignalRefBase::_setBit(uint16_t bit, bool on)
{
#if BLAECK_ENABLE_SIGNAL_META
  if (SignalMeta *m = _owner != nullptr ? _owner->_ensureSignalMeta(_index) : nullptr)
  {
    const uint16_t flags = on ? (uint16_t)(m->MetaFlags | bit)
                              : (uint16_t)(m->MetaFlags & ~bit);
    if (m->MetaFlags != flags)
    {
      m->MetaFlags = flags;
      _owner->_signalConfigDirty = true;
    }
  }
#else
  (void)bit;
  (void)on;
#endif
}

inline void BlaeckSignalRefBase::_setStateClass(BlaeckStateClass stateClass)
{
#if BLAECK_ENABLE_SIGNAL_META
  if (SignalMeta *m = _owner != nullptr ? _owner->_ensureSignalMeta(_index) : nullptr)
  {
    const uint16_t flags =
        (uint16_t)((m->MetaFlags & ~BLAECK_SIG_STATE_CLASS_MASK) |
                   (((uint16_t)stateClass << BLAECK_SIG_STATE_CLASS_SHIFT) &
                    BLAECK_SIG_STATE_CLASS_MASK));
    if (m->MetaFlags != flags)
    {
      m->MetaFlags = flags;
      _owner->_signalConfigDirty = true;
    }
  }
#else
  (void)stateClass;
#endif
}

inline void BlaeckSignalRefBase::_setOptions(BlaeckString optionsCsv)
{
#if BLAECK_ENABLE_SIGNAL_META
  // A refused list leaves the signal as it was.
  if (_owner == nullptr || _index < 0 || _index >= _owner->_signalIndex ||
      !blaeck_detail::optionsAccepted(optionsCsv, _owner->_debugStream,
                                      _owner->Signals[_index].SignalName,
                                      _owner->Signals[_index].NameInFlash))
    return;
  if (SignalMeta *m = _owner->_ensureSignalMeta(_index))
  {
    const uint16_t flags = (optionsCsv != nullptr)
                               ? (uint16_t)(m->MetaFlags | BLAECK_SIG_HAS_OPTIONS)
                               : (uint16_t)(m->MetaFlags & ~BLAECK_SIG_HAS_OPTIONS);
    if (m->Options != optionsCsv || m->MetaFlags != flags)
    {
      if (!_owner->_storeString(m->Options, optionsCsv))
        return;
      m->MetaFlags = flags;
      _owner->_signalConfigDirty = true;
    }
  }
#else
  (void)optionsCsv;
#endif
}

inline void BlaeckSignalRefBase::_setDisplayPrecision(uint8_t decimals)
{
#if BLAECK_ENABLE_SIGNAL_META
  if (SignalMeta *m = _owner != nullptr ? _owner->_ensureSignalMeta(_index) : nullptr)
  {
    const uint16_t flags = (uint16_t)(m->MetaFlags | BLAECK_SIG_HAS_DISPLAY_PRECISION);
    if (m->DisplayPrecision != decimals || m->MetaFlags != flags)
    {
      m->DisplayPrecision = decimals;
      m->MetaFlags = flags;
      _owner->_signalConfigDirty = true;
    }
  }
#else
  (void)decimals;
#endif
}

inline void BlaeckSignalRefBase::_setNameSuffix(uint8_t suffix)
{
  if (_owner == nullptr || _index < 0 || _owner->Signals == nullptr ||
      static_cast<unsigned int>(_index) >= _owner->_signalCapacity)
    return;
  Signal &s = _owner->Signals[_index];
  s.NameSuffix = suffix;
  s.HasSuffix = 1;
  // The suffix changes the name, and the name is part of the schema hash.
  _owner->_schemaHash = _owner->_computeSchemaHash();
}

inline blaeck_detail::StateChannelEntry * BlaeckStateRefBase::_entry() const
{
#if BLAECK_ENABLE_STATE_CHANNELS
  if (_owner != nullptr && _index >= 0)
    return &_owner->_stateChannels[_index];
#endif
  return nullptr;
}

inline void BlaeckStateRefBase::_markDirty() const
{
#if BLAECK_ENABLE_STATE_CHANNELS
  if (_owner != nullptr)
    _owner->_stateCatalogDirty = true;
#endif
}

inline Print *BlaeckStateRefBase::_debugStream() const
{
  return _owner != nullptr ? _owner->_debugStream : nullptr;
}

inline BlaeckEventChannelRef BlaeckEventChannelRef::withIcon(BlaeckString icon)
{
#if BLAECK_ENABLE_EVENTS
  if (_index >= 0 && _owner != nullptr)
    // Only a real change marks the catalog.
    if (_owner->_eventChannels[_index].icon != icon)
    {
      if (!_owner->_storeString(_owner->_eventChannels[_index].icon, icon))
        return *this;
      _owner->_eventCatalogDirty = true;
    }
#else
  (void)icon;
  // Clang's -Wunused-private-field would otherwise fail -Werror builds with events disabled.
  (void)_owner;
  (void)_index;
#endif
  return *this;
}

inline BlaeckEventChannelRef BlaeckEventChannelRef::diagnostic(bool on)
{
#if BLAECK_ENABLE_EVENTS
  if (_index >= 0 && _owner != nullptr)
    if (_owner->_eventChannels[_index].diagnostic != on)
    {
      _owner->_eventChannels[_index].diagnostic = on;
      _owner->_eventCatalogDirty = true;
    }
#else
  (void)on;
#endif
  return *this;
}

inline BlaeckEventChannelRef BlaeckEventChannelRef::withDeviceClass(BlaeckString deviceClass)
{
#if BLAECK_ENABLE_EVENTS
  if (_index >= 0 && _owner != nullptr)
    if (_owner->_eventChannels[_index].deviceClass != deviceClass)
    {
      if (!_owner->_storeString(_owner->_eventChannels[_index].deviceClass, deviceClass))
        return *this;
      _owner->_eventCatalogDirty = true;
    }
#else
  (void)deviceClass;
#endif
  return *this;
}

inline BlaeckEventChannelRef BlaeckEventChannelRef::disabledByDefault(bool on)
{
#if BLAECK_ENABLE_EVENTS
  if (_index >= 0 && _owner != nullptr)
    if (_owner->_eventChannels[_index].disabledByDefault != on)
    {
      _owner->_eventChannels[_index].disabledByDefault = on;
      _owner->_eventCatalogDirty = true;
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
