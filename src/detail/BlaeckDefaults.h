/*
  Shared compile-time configuration for both transports.
  Class-layout settings must be identical in every translation unit.
*/
#ifndef BLAECK_DEFAULTS_H
#define BLAECK_DEFAULTS_H

#if defined __has_include
  #if __has_include(<BlaeckSerialConfig.h>) || __has_include(<BlaeckTCPConfig.h>)
    #error "Migrate legacy BlaeckSerialConfig.h/BlaeckTCPConfig.h settings to one BlaeckConfig.h for the unified library."
  #endif
  #if __has_include(<BlaeckConfig.h>)
    #include <BlaeckConfig.h>
  #endif
#endif

// Preserve a build-wide buffering override, while allowing independent defaults.
#ifndef BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT
  #if defined(BLAECK_BUFFERED_WRITES_DEFAULT)
    #define BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT BLAECK_BUFFERED_WRITES_DEFAULT
  #elif defined(__AVR__)
    #define BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT false
  #else
    #define BLAECK_SERIAL_BUFFERED_WRITES_DEFAULT true
  #endif
#endif

#ifndef BLAECK_TCP_BUFFERED_WRITES_DEFAULT
  #if defined(BLAECK_BUFFERED_WRITES_DEFAULT)
    #define BLAECK_TCP_BUFFERED_WRITES_DEFAULT BLAECK_BUFFERED_WRITES_DEFAULT
  #else
    #define BLAECK_TCP_BUFFERED_WRITES_DEFAULT true
  #endif
#endif

#endif
