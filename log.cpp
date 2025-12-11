#include <LittleFS.h>
#include <WebSerial.h>
#include <time.h>

#include "log.h"

const char* logFilePath = "/log.txt";
const size_t maxLogFileSize = 1024 * 100; // 100 KB max log size

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
        
        // Skip carriage returns entirely
        if (c == '\r') {
            continue;
        }
        
        if (c == '\n') {
            // Null terminate the buffer
            buffer[bufferIndex] = '\0';
            
            // Only process non-empty lines (trim whitespace-only lines too)
            if (bufferIndex > 0) {
                // Check if line is all whitespace
                bool hasContent = false;
                for (int i = 0; i < bufferIndex; i++) {
                    if (buffer[i] != ' ' && buffer[i] != '\t') {
                        hasContent = true;
                        break;
                    }
                }
                
                if (hasContent) {
                    if (skippingPhase) {
                        // Count this line in bytes to skip
                        bytesSkipped += bufferIndex + 1; // +1 for newline
                        if (bytesSkipped >= bytesToRemove) {
                            skippingPhase = false;
                            String truncationMessage = "[" + getTimestamp() + "] [LOG] --- Older entries removed due to size limit ---";
                            tempFile.println(truncationMessage);
                            // Write the current line
                            tempFile.println(buffer);
                        }
                    } else {
                        // Write the line
                        tempFile.println(buffer);
                    }
                }
            }
            
            // Reset buffer for next line
            bufferIndex = 0;
        } else if (bufferIndex < 255) {
            // Add character to buffer
            buffer[bufferIndex++] = c;
        }
        // If buffer is full (255 chars), silently skip remaining chars until newline
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
    
    // Always print to Serial
    Serial.println(timestampedMessage);
    
    // Only print to WebSerial if WiFi is connected
    if (WiFi.status() == WL_CONNECTED) {
        WebSerial.println(timestampedMessage);
    }

    truncateLogIfNeeded();

    File logFile = LittleFS.open(logFilePath, "a");
    if (!logFile) {
        Serial.println("[LOG] Failed to open log file");
        if (WiFi.status() == WL_CONNECTED) {
            WebSerial.println("[LOG] Failed to open log file");
        }
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