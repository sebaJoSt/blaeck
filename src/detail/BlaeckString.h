#pragma once

#include <Arduino.h>
#include <new>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace blaeck
{

/*!
  @brief A borrowed string argument that accepts ordinary text or F() literals.

  Conversion does not allocate. Configuration calls copy ordinary text when retaining it;
  this view itself must not outlive its source.

  @code
    device.addSignal("Voltage", &voltage).withUnit("V");
  @endcode
*/
class BlaeckString
{
public:
  BlaeckString(const char *text = nullptr) : _text(text), _flash(false) {}
  BlaeckString(const __FlashStringHelper *text)
      : _text(reinterpret_cast<const char *>(text)), _flash(true) {}
  BlaeckString(decltype(nullptr)) : BlaeckString() {}
  const char *data() const { return _text; }
  bool inFlash() const { return _flash; }
  byte read(size_t offset = 0) const
  {
    return _flash ? pgm_read_byte(_text + offset) : static_cast<byte>(_text[offset]);
  }
  bool operator==(decltype(nullptr)) const { return _text == nullptr; }
  bool operator!=(decltype(nullptr)) const { return _text != nullptr; }
  bool operator==(BlaeckString other) const
  {
    if (_text == nullptr || other._text == nullptr)
      return _text == other._text;
    for (size_t i = 0;; ++i)
    {
      const byte c = read(i);
      if (c != other.read(i))
        return false;
      if (c == 0)
        return true;
    }
  }
  bool operator!=(BlaeckString other) const { return !(*this == other); }
  void printTo(Print &out) const
  {
    if (_text != nullptr)
    {
      if (_flash)
        out.print(reinterpret_cast<const __FlashStringHelper *>(_text));
      else
        out.print(_text);
    }
  }

private:
  const char *_text;
  bool _flash;
};

namespace detail
{

// RAM copies can be shared by command-owned state and CSV event entries.
class StoredString
{
  struct Block
  {
    uint32_t refs;
  };
  const void *_data = nullptr;
  bool _owned = false;

  void release()
  {
    if (_owned && --static_cast<Block *>(const_cast<void *>(_data))->refs == 0)
      ::operator delete(const_cast<void *>(_data));
    _data = nullptr;
    _owned = false;
  }

public:
  StoredString() {}
  StoredString(decltype(nullptr)) {}
  ~StoredString() { release(); }
  StoredString(const StoredString &other) : _data(other._data), _owned(other._owned)
  {
    if (_owned)
      ++static_cast<Block *>(const_cast<void *>(_data))->refs;
  }
  StoredString &operator=(const StoredString &other)
  {
    if (this != &other)
    {
      StoredString copy(other);
      const void *data = _data;
      const bool owned = _owned;
      _data = copy._data;
      _owned = copy._owned;
      copy._data = data;
      copy._owned = owned;
    }
    return *this;
  }
  StoredString &operator=(decltype(nullptr)) { release(); return *this; }
  operator BlaeckString() const
  {
    if (_owned)
      return reinterpret_cast<const char *>(static_cast<const Block *>(_data) + 1);
    return reinterpret_cast<const __FlashStringHelper *>(_data);
  }
  bool operator==(decltype(nullptr)) const { return _data == nullptr; }
  bool operator!=(decltype(nullptr)) const { return _data != nullptr; }
  bool operator!=(BlaeckString other) const { return BlaeckString(*this) != other; }
  byte read(size_t offset = 0) const { return BlaeckString(*this).read(offset); }
  void printTo(Print &out) const { BlaeckString(*this).printTo(out); }

  // Failure leaves the old value intact; the caller reports it through its rejection path.
  bool set(BlaeckString value)
  {
    if (BlaeckString(*this) == value)
      return true;
    if (value == nullptr || value.inFlash())
    {
      release();
      _data = value.data();
      return true;
    }
    const size_t length = strlen(value.data());
    const size_t overhead = sizeof(Block) + 1;
    if (length > SIZE_MAX - overhead)
      return false;
    Block *copy = static_cast<Block *>(::operator new(overhead + length, std::nothrow));
    if (copy == nullptr)
      return false;
    new (copy) Block{1};
    memcpy(reinterpret_cast<char *>(copy + 1), value.data(), length + 1);
    release();
    _data = copy;
    _owned = true;
    return true;
  }
};

} // namespace detail
} // namespace blaeck
