#ifndef _LOG_H
#define _LOG_H

#if CORE_DEBUG_LEVEL > 0
  #define LOG_INIT() Serial.begin(115200);
  #define LOG_PRINTF Serial.printf
  #define LOG_PRINTLN(line) Serial.println(line)
  #define LOG_PRINT(text) Serial.print(text)
#else
  #define LOG_INIT()
  #define LOG_PRINTF(format, ...)
  #define LOG_PRINTLN(line)
  #define LOG_PRINT(text)
#endif

#endif