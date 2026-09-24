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
#define F(text) reinterpret_cast<const __FlashStringHelper *>(text)
#define PROGMEM
inline byte pgm_read_byte(const void *p) { return *static_cast<const byte *>(p); }
inline unsigned long millis() { return 0; }
inline unsigned long micros() { return 0; }
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
  size_t print(const __FlashStringHelper *text) { return print(reinterpret_cast<const char *>(text)); }
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
