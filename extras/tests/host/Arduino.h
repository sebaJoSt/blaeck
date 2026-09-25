#pragma once

// Host-only Arduino surface for exercising the real Blaeck core and transports.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <string>
#include <type_traits>

using byte = uint8_t;
using PGM_P = const char *;
class __FlashStringHelper;
#if BLAECK_TEST_SEPARATE_FLASH
struct HostFlashRegion
{
  const byte *data;
  size_t size;
  HostFlashRegion *next;
};
inline HostFlashRegion *&hostFlashRegions() { static HostFlashRegion *head = nullptr; return head; }

template<size_t N>
struct HostFlashLiteral
{
  byte bytes[N + 1];
  HostFlashRegion region;
  explicit HostFlashLiteral(const char (&text)[N])
      : region{bytes, N, hostFlashRegions()}
  {
    for (size_t i = 0; i < N; ++i)
      bytes[i] = static_cast<byte>(text[i]) ^ 0x80;
    bytes[N] = 0;
    hostFlashRegions() = &region;
  }
};
// Encoded storage makes direct RAM reads observably wrong, including the terminator.
#define F(text) ([]() -> const __FlashStringHelper * { \
  static HostFlashLiteral<sizeof(text)> literal(text); \
  return reinterpret_cast<const __FlashStringHelper *>(literal.bytes); }())
inline byte pgm_read_byte(const void *p)
{
  const uintptr_t address = reinterpret_cast<uintptr_t>(p);
  for (HostFlashRegion *r = hostFlashRegions(); r != nullptr; r = r->next)
    if (address >= reinterpret_cast<uintptr_t>(r->data) &&
        address - reinterpret_cast<uintptr_t>(r->data) < r->size)
      return *static_cast<const byte *>(p) ^ 0x80;
  return *static_cast<const byte *>(p);
}
#else
#define F(text) reinterpret_cast<const __FlashStringHelper *>(text)
inline byte pgm_read_byte(const void *p) { return *static_cast<const byte *>(p); }
#endif
#define PROGMEM
inline uint32_t &hostMillis() { static uint32_t value = 0; return value; }
inline uint32_t &hostMicros() { static uint32_t value = 0; return value; }
inline unsigned long millis() { return hostMillis(); }
inline unsigned long micros() { return hostMicros(); }
inline void yield() {}

class Print
{
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *data, size_t size)
  {
    size_t written = 0;
    for (size_t i = 0; i < size; ++i)
      written += write(data[i]);
    return written;
  }
  size_t write(const char *text) { return write(reinterpret_cast<const uint8_t *>(text), strlen(text)); }
  size_t print(const char *text) { return write(text); }
  size_t print(const __FlashStringHelper *text)
  {
    size_t written = 0;
    const byte *p = reinterpret_cast<const byte *>(text);
    byte c;
    while ((c = pgm_read_byte(p++)) != 0)
      written += write(c);
    return written;
  }
  size_t print(char c) { return write(static_cast<uint8_t>(c)); }
  template<class T>
  typename std::enable_if<std::is_integral<T>::value, size_t>::type print(T value)
  {
    return print(std::to_string(value).c_str());
  }
  size_t print(double value, int digits = 2)
  {
    char buffer[80];
    snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
    return print(buffer);
  }
  size_t println() { return print("\r\n"); }
  template<class T> size_t println(T value) { return print(value) + println(); }
  virtual void flush() {}
};

class Stream : public Print
{
public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
};
