#include <LittleFS.h>
#include <WebSerial.h>
#include <time.h>

#include "log.h"

const char* logFilePath = "/log.txt";
const size_t maxLogFileSize = 4096 * 10; // 40KB max log size, adjust as needed

void initiateLog() {
    if (!LittleFS.begin()) {
        WebSerial.println("[LOG] LittleFS mount failed");
        return;
    }
    if (!LittleFS.exists(logFilePath)) {
        File logFile = LittleFS.open(logFilePath, "w");
        if (logFile) {
            logFile.println("[LOG] device reset, log initiated!");
            logFile.close();
        }
    } 
    else {
        File logFile = LittleFS.open(logFilePath, "a");
        if (logFile) {
            logFile.println("[LOG] device reset!");
            logFile.close();
        }
    }
}

String getTimestamp() {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    unsigned long millisecond = millis() % 1000;
    char buffer[30];

    snprintf(
        buffer,
        sizeof(buffer), 
        "%02d.%02d.%04d %02d:%02d:%02d.%03lu",
        timeinfo.tm_mday,
        timeinfo.tm_mon + 1,
        timeinfo.tm_year + 1900,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec,
        millisecond
    );

    return String(buffer);
}

void truncateLogIfNeeded() {
    File logFile = LittleFS.open(logFilePath, "r");
    if (!logFile) {
        WebSerial.println("[LOG] Failed to open log file for truncation check");
        return;
    }

    size_t fileSize = logFile.size();
    if (fileSize <= maxLogFileSize) {
        logFile.close();
        return;
    }
    logFile.close();

    // If file is too large, simply clear it to avoid memory issues
    // Reading 40KB into memory on ESP8266 causes crashes
    logFile = LittleFS.open(logFilePath, "w");
    if (!logFile) {
        WebSerial.println("[LOG] Failed to open log file for truncation");
        return;
    }
    logFile.println("[LOG] Log truncated due to size limit");
    logFile.close();
}

void logMessage(const String& message) {
    String timestampedMessage = "[" + getTimestamp() + "] " + message;
    WebSerial.println(timestampedMessage); // Keep WebSerial output for real-time monitoring

    truncateLogIfNeeded();

    File logFile = LittleFS.open(logFilePath, "a");
    if (!logFile) {
        WebSerial.println("[LOG] Failed to open log file");
        return;
    }
    logFile.println(timestampedMessage);
    logFile.close();
}

String readLog() {
    File logFile = LittleFS.open(logFilePath, "r");
    if (!logFile) {
        return "[LOG] Failed to open log file";
    }

    String logContent = logFile.readString();
    logFile.close();
    return logContent.length() > 0 ? logContent : "[LOG] Log file empty or unreadable";
}