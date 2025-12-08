#include <LittleFS.h>
#include <WebSerial.h>
#include <time.h>

#include "log.h"

const char* logFilePath = "/log.txt";
const size_t maxLogFileSize = 4096 * 100; // 400KB max log size (increased from 40KB)

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

    // File is too large - remove oldest lines one by one
    // Use char buffer instead of String to minimize heap fragmentation
    String tempPath = "/log_temp.txt";
    File tempFile = LittleFS.open(tempPath, "w");
    if (!tempFile) {
        logFile.close();
        WebSerial.println("[LOG] Failed to create temp file for truncation");
        return;
    }

    // Calculate how many bytes we need to remove (with some buffer)
    size_t bytesToRemove = fileSize - (maxLogFileSize * 3 / 4); // Keep at 75% capacity
    size_t bytesSkipped = 0;
    bool skippingPhase = true;
    
    char buffer[256]; // Fixed buffer for line reading
    int bufferIndex = 0;

    while (logFile.available()) {
        char c = logFile.read();
        
        if (c == '\n' || bufferIndex >= 255) {
            buffer[bufferIndex] = '\0';
            
            if (skippingPhase) {
                bytesSkipped += bufferIndex + 1; // +1 for newline
                if (bytesSkipped >= bytesToRemove) {
                    skippingPhase = false;
                    tempFile.println("[LOG] --- Older entries removed due to size limit ---");
                }
            } else {
                tempFile.println(buffer);
            }
            
            bufferIndex = 0;
        } else {
            buffer[bufferIndex++] = c;
        }
    }

    logFile.close();
    tempFile.close();

    // Replace original file with temp file
    LittleFS.remove(logFilePath);
    LittleFS.rename(tempPath, logFilePath);
    
    WebSerial.println("[LOG] Removed oldest entries to maintain size limit");
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