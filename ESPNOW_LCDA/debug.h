#pragma once

// debug.h - centralized debug macros. Define ENABLE_DEBUG to enable Serial output.

#ifdef ENABLE_DEBUG
#include <Arduino.h>
#define DBG_BEGIN(baud) Serial.begin(baud)
#define DBG_AVAILABLE() Serial.available()
#define DBG_READ() Serial.read()
#define DBG_PRINT(...) Serial.print(__VA_ARGS__)
#define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define DBG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DBG_BEGIN(baud) ((void)0)
#define DBG_AVAILABLE() (0)
#define DBG_READ() (0)
#define DBG_PRINT(...) ((void)0)
#define DBG_PRINTLN(...) ((void)0)
#define DBG_PRINTF(...) ((void)0)
#endif
