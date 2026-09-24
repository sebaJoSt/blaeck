#pragma once
#include "Arduino.h"

class Client : public Stream
{
public:
  virtual uint8_t connected() = 0;
  virtual void stop() = 0;
  virtual operator bool() = 0;
};
