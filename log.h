#include <ESP8266WiFi.h>

#ifndef LOG_H
#define LOG_H

extern const char* logFilePath;

String        getTimestamp();

void          initiateLog  ();

void          truncateLogIfNeeded ();

void          logMessage (const String& message);

String        readLog();

#endif // LOG_H
