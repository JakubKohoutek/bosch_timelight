#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncWebServer.h>
#include <WebSerial.h>
#include <credentials.h>
#include <LittleFS.h>
#include <time.h>
#include <SoftwareSerial.h>
#include "ota.h"
#include "memory.h"
#include "log.h"

// Prague timezone with DST: CET-1CEST,M3.5.0/02,M10.5.0/03
#define MY_TZ "CET-1CEST,M3.5.0/02,M10.5.0/03"
#define MY_NTP_SERVER "pool.ntp.org"
#define DBUS_PIN 4      // D2
#define LED_PIN LED_BUILTIN
#define SUBCOMMAND_POS 3
#define FULL_TIME_REPORT_SUBCMD 0x08
#define PROGRESS_REPORT_SUBCMD 0x00
#define PROGRAM_START_SUBCMD 0x04

SoftwareSerial dbus(DBUS_PIN, -1); // RX only, no TX
AsyncWebServer server(80);
const char*    ssid                     = STASSID;
const char*    password                 = STAPSK;
const char*    deviceId                 = "dishwasher";

const int      MAX_FRAME                = 32;
uint8_t        frameBuffer[MAX_FRAME];
int            frameLen                 = 0;
unsigned long  lastByteTime             = 0;
const unsigned long FRAME_TIMEOUT       = 2000; // microseconds
const int      MAX_BYTES_PER_LOOP       = 64; // Processing max 64 bytes per loop avoids network issues

// Lookahead buffer to detect frame start pattern: <any>, 0x65
uint8_t        prevByte2                = 0;    // previous byte
bool           inFrame                  = false;

// Remaining time tracking
int            remainingHours           = 0;
int            remainingMinutes         = 0;
unsigned long  lastUpdateTime           = 0;

// Other state info tracking
uint8_t        progressPercentage       = 0;
bool           programRunning           = false;

void turnOffWiFi() {
    logMessage(String("[MAIN] Turning the WiFi off"));
    WiFi.disconnect();
    WiFi.forceSleepBegin();
    delay(1); //For some reason the modem won't go to sleep unless you do a delay
}

void turnOnWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false); // Don't write to flash every time - prevents flash wear
    WiFi.setSleepMode(WIFI_NONE_SLEEP); // Disable WiFi sleep to prevent disconnections
    WiFi.setOutputPower(20.5); // Maximum transmit power for better stability
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) { // 20 seconds timeout
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(String("[MAIN] WiFi connected with signal strength: ") + WiFi.RSSI() + " dBm");
    } else {
        Serial.println("\nWiFi connection failed!");
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    dbus.begin(9600); // Bosch uses 9600 baud rate
    Serial.println(String("\nTimeLight module frame logger starting..."));

    turnOnWiFi();
    
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
    configTime(MY_TZ, MY_NTP_SERVER);

    // Inititate eeprom memory
    initiateMemory();

    // Initiate log
    initiateLog();

    // Initiate over the air programming
    OTA::initialize(deviceId);

    // WebSerial is accessible at "<IP Address>/webserial" in browser
    WebSerial.begin(&server);
    WebSerial.msgCallback(handleWebSerialMessage);

    // Set up the html server
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta charset='UTF-8'>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1.0, user-scalable=no'>";
        html += "<title>Dishwasher TimeLight</title>";
        html += "<style>";
        html += "* { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }";
        html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif; display: flex; justify-content: center; align-items: center; min-height: 100vh; padding: clamp(10px, 3vw, 15px); background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); }";
        html += ".container { text-align: center; background: white; padding: clamp(25px, 6vw, 40px); border-radius: clamp(15px, 4vw, 20px); box-shadow: 0 8px 32px rgba(0,0,0,0.2); width: 100%; max-width: 450px; }";
        html += "h1 { color: #333; margin-bottom: clamp(20px, 5vw, 30px); font-size: clamp(22px, 6.5vw, 32px); line-height: 1.2; }";
        html += ".time { font-size: clamp(56px, 16vw, 72px); font-weight: bold; color: #667eea; margin: clamp(10px, 3vw, 15px) 0; line-height: 1; text-shadow: 0 2px 4px rgba(102,126,234,0.1); }";
        html += ".status { font-size: clamp(15px, 4.2vw, 18px); color: #666; margin: clamp(8px, 2vw, 12px) 0; }";
        html += ".running-status { display: inline-flex; align-items: center; gap: 8px; font-size: clamp(14px, 3.8vw, 16px); padding: clamp(8px, 2.5vw, 12px) clamp(16px, 4vw, 20px); border-radius: 20px; margin: clamp(10px, 3vw, 15px) 0; font-weight: 500; }";
        html += ".running-status.active { background: rgba(76,175,80,0.15); color: #2e7d32; border: 1px solid rgba(76,175,80,0.3); }";
        html += ".running-status.inactive { background: rgba(158,158,158,0.08); color: #757575; border: 1px solid rgba(158,158,158,0.2); }";
        html += ".status-dot { width: 8px; height: 8px; border-radius: 50%; }";
        html += ".status-dot.active { background: #4caf50; animation: pulse 2s infinite; }";
        html += ".status-dot.inactive { background: #bdbdbd; }";
        html += "@keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }";
        html += ".empty-state { font-size: clamp(15px, 4vw, 17px); color: #999; margin: clamp(20px, 5vw, 30px) 0; font-style: italic; }";
        html += ".last-updated { font-size: clamp(11px, 3vw, 12px); color: #aaa; margin-top: clamp(8px, 2vw, 10px); font-style: italic; }";
        html += ".progress-container { margin: clamp(18px, 5vw, 25px) 0; }";
        html += ".progress-label { font-size: clamp(13px, 3.8vw, 15px); color: #666; margin-bottom: 10px; font-weight: 500; }";
        html += ".progress-bar { width: 100%; height: clamp(32px, 8vw, 36px); background: #e0e0e0; border-radius: 18px; overflow: visible; position: relative; box-shadow: inset 0 2px 4px rgba(0,0,0,0.1); }";
        html += ".progress-fill { height: 100%; background: linear-gradient(90deg, #667eea 0%, #764ba2 100%); border-radius: 18px; transition: width 0.5s ease; position: absolute; top: 0; left: 0; box-shadow: 0 2px 8px rgba(102,126,234,0.3); }";
        html += ".progress-text { position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); color: #333; font-weight: bold; font-size: clamp(13px, 3.8vw, 15px); z-index: 1; text-shadow: 0 1px 2px rgba(255,255,255,0.8); }";
        html += ".links { margin-top: clamp(20px, 5vw, 30px); display: flex; gap: clamp(12px, 3vw, 15px); justify-content: center; flex-wrap: wrap; }";
        html += "a { color: #667eea; text-decoration: none; font-size: clamp(15px, 4.2vw, 16px); padding: clamp(12px, 3vw, 14px) clamp(20px, 5vw, 24px); border: 2px solid #667eea; border-radius: 10px; display: inline-block; transition: all 0.3s; min-width: clamp(110px, 28vw, 130px); font-weight: 500; box-shadow: 0 2px 8px rgba(102,126,234,0.15); }";
        html += "a:hover, a:active { background: #667eea; color: white; transform: translateY(-2px); box-shadow: 0 4px 12px rgba(102,126,234,0.3); }";
        html += ".ip-toggle { font-size: clamp(11px, 3vw, 12px); color: #667eea; cursor: pointer; margin-top: clamp(15px, 4vw, 20px); text-decoration: underline; user-select: none; }";
        html += ".ip-info { font-size: clamp(11px, 3vw, 12px); color: #999; margin-top: clamp(10px, 3vw, 12px); padding: clamp(12px, 3vw, 15px); background: #f5f5f5; border-radius: 10px; display: none; }";
        html += ".ip-info.show { display: block; }";
        html += ".ip-address { font-family: 'Courier New', monospace; font-weight: bold; color: #667eea; font-size: clamp(13px, 3.5vw, 14px); }";
        html += ".bookmark-hint { font-size: clamp(10px, 2.8vw, 11px); color: #666; margin-top: 6px; }";
        html += "@media (max-width: 480px) { body { padding: 10px; } .container { padding: 20px; } }";
        html += "</style>";
        html += "<script>";
        html += "let lastUpdate = Date.now();";
        html += "let fetchInProgress = false;";
        html += "function updateTime() {";
        html += "  if (fetchInProgress) return;";
        html += "  fetchInProgress = true;";
        html += "  fetch('/api/time').then(r => r.json()).then(data => {";
        html += "    const timeEl = document.getElementById('time');";
        html += "    const statusEl = document.getElementById('status');";
        html += "    timeEl.innerText = data.time;";
        html += "    if(data.time === 'Done!') {";
        html += "      statusEl.innerText = 'Status: Complete';";
        html += "    } else if(data.time === '--:--') {";
        html += "      if(statusEl) statusEl.style.display = 'none';";
        html += "    } else {";
        html += "      if(statusEl) { statusEl.style.display = 'block'; statusEl.innerText = data.status; }";
        html += "    }";
        html += "    if(data.progress !== undefined) {";
        html += "      document.getElementById('progress-fill').style.width = data.progress + '%';";
        html += "      document.getElementById('progress-text').innerText = data.progress + '%';";
        html += "    }";
        html += "    const runningEl = document.getElementById('running-status');";
        html += "    if(data.running) {";
        html += "      runningEl.className = 'running-status active';";
        html += "      runningEl.innerHTML = '<span class=\"status-dot active\" id=\"status-dot\"></span>Running';";
        html += "    } else {";
        html += "      runningEl.className = 'running-status inactive';";
        html += "      runningEl.innerHTML = '<span class=\"status-dot inactive\" id=\"status-dot\"></span>Idle';";
        html += "    }";
        html += "    lastUpdate = Date.now();";
        html += "    updateLastUpdated();";
        html += "    fetchInProgress = false;";
        html += "  }).catch(e => {";
        html += "    console.error('Update failed:', e);";
        html += "    fetchInProgress = false;";
        html += "  });";
        html += "}";
        html += "function updateLastUpdated() {";
        html += "  const seconds = Math.floor((Date.now() - lastUpdate) / 1000);";
        html += "  const el = document.getElementById('last-updated');";
        html += "  if(el) el.innerText = seconds === 0 ? 'Just updated' : 'Updated ' + seconds + 's ago';";
        html += "}";
        html += "function toggleIP() {";
        html += "  document.getElementById('ip-info').classList.toggle('show');";
        html += "}";
        html += "setInterval(updateTime, 10000);";
        html += "setInterval(updateLastUpdated, 1000);";
        html += "</script>";
        html += "</head><body>";
        html += "<div class='container'>";
        html += "<h1>🍽️ Dishwasher Status</h1>";
        
        if (lastUpdateTime > 0) {
            html += "<div class='time' id='time'>";
            if (remainingHours > 0 || remainingMinutes > 0) {
                if (remainingHours < 10) html += "0";
                html += String(remainingHours) + ":";
                if (remainingMinutes < 10) html += "0";
                html += String(remainingMinutes);
            } else {
                html += "Done!";
            }
            html += "</div>";
            html += "<div class='status' id='status'>";
            if (remainingHours == 0 && remainingMinutes == 0) {
                html += "Status: Complete";
            } else {
                html += "Remaining Time";
            }
            html += "</div>";
        } else {
            html += "<div class='time' id='time'>--:--</div>";
            html += "<div class='empty-state'>⏳ Waiting for dishwasher data...</div>";
        }

        // Add program running status
        if (programRunning) {
            html += "<div class='running-status active' id='running-status'>";
            html += "<span class='status-dot active' id='status-dot'></span>Running";
            html += "</div>";
        } else {
            html += "<div class='running-status inactive' id='running-status'>";
            html += "<span class='status-dot inactive' id='status-dot'></span>Idle";
            html += "</div>";
        }
        
        // Add last updated indicator
        html += "<div class='last-updated' id='last-updated'>Just updated</div>";
        
        // Add progress bar
        html += "<div class='progress-container'>";
        html += "<div class='progress-label'>Progress</div>";
        html += "<div class='progress-bar'>";
        html += "<div class='progress-fill' id='progress-fill' style='width:" + String(progressPercentage) + "%'></div>";
        html += "<span class='progress-text' id='progress-text'>" + String(progressPercentage) + "%</span>";
        html += "</div></div>";
        
        html += "<div class='links'>";
        html += "<a href='/logs'>📋 View Logs</a>";
        html += "<a href='/webserial'>💻 WebSerial</a>";
        html += "</div>";
        
        // Add collapsible IP address info for bookmarking
        html += "<div class='ip-toggle' onclick='toggleIP()'>📱 Show bookmark info</div>";
        html += "<div class='ip-info' id='ip-info'>";
        html += "<strong>Bookmark this page:</strong><br>";
        html += "<span class='ip-address'>http://" + WiFi.localIP().toString() + "/</span><br>";
        html += "<span class='bookmark-hint'>(Tap browser menu → Add to Home Screen)</span>";
        html += "</div>";
        
        html += "</div></body></html>";
        request->send(200, "text/html", html);
    });

    server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LittleFS, logFilePath, "text/plain");
    });

    server.on("/api/time", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{\"time\":\"";
        String status = "";
        
        if (lastUpdateTime > 0) {
            if (remainingHours > 0 || remainingMinutes > 0) {
                if (remainingHours < 10) json += "0";
                json += String(remainingHours) + ":";
                if (remainingMinutes < 10) json += "0";
                json += String(remainingMinutes);
                status = "Remaining Time";
            } else {
                json += "Done!";
                status = "Remaining Time";
            }
        } else {
            json += "--:--";
            status = "No data";
        }
        
        json += "\",\"status\":\"" + status + "\"";
        json += ",\"progress\":" + String(progressPercentage);
        json += ",\"running\":" + String(programRunning ? "true" : "false");
        json += "}";
        request->send(200, "application/json", json);
    });

    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Sorry, not found. Did you mean to go to /webserial ?");
    });

    server.begin();
    
    // Setup mDNS for dishwasher.local (after server starts)
    if (MDNS.begin("dishwasher")) {
        logMessage(String("mDNS responder started: dishwasher.local"));
        MDNS.addService("http", "tcp", 80);
    } else {
        logMessage(String("Error setting up mDNS responder!"));
    }
}

void handleWebSerialMessage(uint8_t *data, size_t len){
    // Process message into command:value pair  
    String command = "";
    String value   = "";
    boolean beforeColon = true;
    for(int i=0; i < len; i++){
        if (char(data[i]) == ':'){
            beforeColon = false;
        } else if (beforeColon) {
            command += char(data[i]);
        } else {
            value += char(data[i]);
        }
    }

    if(command.equals("help")) {
        logMessage(
          String("Available commands:\n") +
          "nothing\n"
        );
    } else {
        logMessage(String("Unknown command '") + command + "' with value '" + value +"'");
    }
}

void loop() {
    // Check WiFi connection periodically and reconnect if needed
    static unsigned long lastWiFiCheck = 0;
    static unsigned long lastSignalLog = 0;
    
    if (millis() - lastWiFiCheck > 10000) { // Check every 10 seconds (more frequent)
        lastWiFiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            logMessage("[WIFI] Connection lost, reconnecting...");
            WiFi.disconnect();
            delay(100);
            WiFi.begin(ssid, password); // Use WiFi.begin() instead of reconnect()
            
            // Wait up to 10 seconds for reconnection
            int attempts = 0;
            while (WiFi.status() != WL_CONNECTED && attempts < 20) {
                delay(500);
                attempts++;
            }
            
            if (WiFi.status() == WL_CONNECTED) {
                logMessage(String("[WIFI] Reconnected successfully"));
            } else {
                logMessage(String("[WIFI] Reconnection failed, will retry later"));
            }
        }
    }
    
    // Log WiFi signal strength every 10 minutes
    if (millis() - lastSignalLog > 600000) { // 600000 ms = 10 minutes
        lastSignalLog = millis();
        if (WiFi.status() == WL_CONNECTED) {
            int rssi = WiFi.RSSI();
            String quality = "unknown";
            if (rssi >= -50) {
                quality = "Excellent";
            } else if (rssi >= -60) {
                quality = "Very good";
            } else if (rssi >= -70) {
                quality = "Good";
            } else if (rssi >= -80) {
                quality = "Fair";
            } else if (rssi >= -90) {
                quality = "Poor";
            } else {
                quality = "Very poor";
            }
            logMessage(String("[WIFI] Signal strength: ") + rssi + " dBm (" + quality + ")");
        }
    }
    
    OTA::handle();
    
    // Only update mDNS if WiFi is connected, and limit frequency
    static unsigned long lastMDNSUpdate = 0;
    if (WiFi.status() == WL_CONNECTED && (millis() - lastMDNSUpdate > 1000)) {
        lastMDNSUpdate = millis();
        MDNS.update();
    }

    // Read available bytes with yield for WiFi stack to avoid blocking network operations
    int bytesProcessed = 0;
    while (dbus.available() && bytesProcessed < MAX_BYTES_PER_LOOP) {
        uint8_t b = dbus.read();
        bytesProcessed++;
        lastByteTime = micros();
        digitalWrite(LED_PIN, HIGH); // show activity

        // Detect frame start pattern: <length byte>, 0x65
        if (!inFrame) {
            if (b == 0x65) {
                // Pattern detected! Start new frame with length byte and 0x65
                frameLen = 0;
                frameBuffer[frameLen++] = prevByte2;  // length byte (before 0x65)
                frameBuffer[frameLen++] = b;          // 0x65
                inFrame = true;
            }
            prevByte2 = b;
        } else {
            // We're in a frame, continue collecting bytes
            if (frameLen < MAX_FRAME) {
                frameBuffer[frameLen++] = b;
                
                // Check if frame is complete based on length byte
                // Frame structure: [length] [0x65] [0x20] [data...] [checksum] [0x6A]
                // Length byte indicates bytes after 0x65 0x20
                // Total frame = length + 0x65 + 0x20 + (length bytes) + checksum + 0x6A
                // = 1 + 1 + 1 + length + 1 + 1 = length + 5
                if (frameLen >= 3) {
                    uint8_t expectedLen = frameBuffer[0] + 5; // length + 0x65 + 0x20 + checksum + 0x6A
                    if (frameLen >= expectedLen) {
                        // Frame complete, process immediately
                        digitalWrite(LED_PIN, LOW);
                        
                        // Verify frame ends with 0x6A
                        if (frameBuffer[frameLen - 1] == 0x6A && frameBuffer[2] == 0x20 && frameLen >= 5) {
                            char logLine[256];
                            int pos = snprintf(logLine, sizeof(logLine), "[TimeLight FRAME] ");
                            
                            for (int i = 0; i < frameLen && pos < 200; i++) {
                                pos += snprintf(logLine + pos, sizeof(logLine) - pos, "%02x ", frameBuffer[i]);
                            }
                            
                            // Handle full time report
                            if (frameBuffer[SUBCOMMAND_POS] == FULL_TIME_REPORT_SUBCMD) {
                                uint8_t remainingTimeHex = frameBuffer[4];
                                remainingHours = remainingTimeHex / 60;
                                remainingMinutes = remainingTimeHex % 60;
                                lastUpdateTime = millis();
                                snprintf(logLine + pos, sizeof(logLine) - pos, "Remaining time: %02d:%02d", remainingHours, remainingMinutes);
                            }
                            
                            // Handle progress report including percentage and program completion
                            if (frameBuffer[SUBCOMMAND_POS] == PROGRESS_REPORT_SUBCMD) {
                                progressPercentage = frameBuffer[4];
                                if (progressPercentage == 100) {
                                    programRunning = false;
                                    snprintf(logLine + pos, sizeof(logLine) - pos, "Program completed");
                                } else if (progressPercentage > 0 && !programRunning) {
                                    programRunning = true;
                                    snprintf(logLine + pos, sizeof(logLine) - pos, "Program run identified");
                                } else {
                                    snprintf(logLine + pos, sizeof(logLine) - pos, "Progress: %d%%", progressPercentage);
                                }
                            }

                            // Handle program start indication
                            if (frameBuffer[SUBCOMMAND_POS] == PROGRAM_START_SUBCMD) {
                                if (frameBuffer[4] == 0x00 && frameBuffer[5] == 0x00 && frameBuffer[6] == 0x00) {
                                    programRunning = true;
                                    snprintf(logLine + pos, sizeof(logLine) - pos, "Program started");
                                }
                            }
                            
                            logMessage(String(logLine));
                        }
                        
                        // Reset for next frame
                        frameLen = 0;
                        inFrame = false;
                    }
                }
            }
        }
    }

    // check for frame timeout (handle micros() overflow)
    unsigned long elapsed = micros() - lastByteTime;
    if (frameLen > 0 && elapsed < 4294967295UL && elapsed > FRAME_TIMEOUT) {
        digitalWrite(LED_PIN, LOW);

        // process frames starting with <any>, 0x65, 0x20
        if (frameLen >= 4 && frameBuffer[1] == 0x65 && frameBuffer[2] == 0x20) {
            char logLine[256];
            int pos = snprintf(logLine, sizeof(logLine), "[TimeLight FRAME] ");
            
            for (int i = 0; i < frameLen && pos < 200; i++) {
                pos += snprintf(logLine + pos, sizeof(logLine) - pos, "%02x ", frameBuffer[i]);
            }
            
            if (frameLen >= 5 && frameBuffer[3] == 0x08) {
                uint8_t hexValue = frameBuffer[4];
                uint8_t hours = hexValue / 60;
                uint8_t minutes = hexValue % 60;
                snprintf(logLine + pos, sizeof(logLine) - pos, "Remaining time: %02d:%02d", hours, minutes);
            }
            
            logMessage(String(logLine));
        }

        // reset for next frame
        frameLen = 0;
        inFrame = false;
    }
    
    // Yield to WiFi/network stack - critical for maintaining connectivity
    yield();
}
