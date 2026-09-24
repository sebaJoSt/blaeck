#pragma once

// Adapted from Rob Tillaart's CRC library (v1.0.4):
// https://github.com/RobTillaart/CRC
// Fixed CRC-32/ISO-HDLC parameters, using the reflected polynomial directly.
//
// MIT License
//
// Copyright (c) 2021-2026 Rob Tillaart
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <stddef.h>
#include <stdint.h>

namespace blaeck
{
namespace detail
{

class BlaeckCRC32
{
public:
  void restart() { _crc = 0xFFFFFFFFUL; }
  uint32_t calc() const { return _crc ^ 0xFFFFFFFFUL; }

  void add(uint8_t value)
  {
    _crc ^= value;
    for (uint8_t i = 0; i < 8; ++i)
    {
      if (_crc & 1U)
        _crc = (_crc >> 1) ^ 0xEDB88320UL;
      else
        _crc >>= 1;
    }
  }

  void add(const uint8_t *data, size_t length)
  {
    while (length--)
      add(*data++);
  }

private:
  uint32_t _crc = 0xFFFFFFFFUL;
};

} // namespace detail
} // namespace blaeck
