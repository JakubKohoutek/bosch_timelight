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

// Lookahead buffer to detect frame start pattern: <any>, 0x65
uint8_t        prevByte2                = 0;    // previous byte
bool           inFrame                  = false;

// Remaining time tracking
int            remainingHours           = 0;
int            remainingMinutes         = 0;
unsigned long  lastUpdateTime           = 0;

void turnOffWiFi() {
    logMessage(String("[MAIN] Turning the WiFi off"));
    WiFi.disconnect();
    WiFi.forceSleepBegin();
    delay(1); //For some reason the modem won't go to sleep unless you do a delay
}

void turnOnWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED){
        delay(500);
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
        html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
        html += "<title>Dishwasher TimeLight</title>";
        html += "<style>";
        html += "body { font-family: Arial, sans-serif; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); }";
        html += ".container { text-align: center; background: white; padding: 40px; border-radius: 20px; box-shadow: 0 10px 40px rgba(0,0,0,0.3); }";
        html += "h1 { color: #333; margin-bottom: 30px; }";
        html += ".time { font-size: 72px; font-weight: bold; color: #667eea; margin: 20px 0; }";
        html += ".status { font-size: 18px; color: #666; margin-top: 20px; }";
        html += "a { color: #667eea; text-decoration: none; margin: 0 10px; }";
        html += "a:hover { text-decoration: underline; }";
        html += "</style>";
        html += "<script>";
        html += "function updateTime() {";
        html += "  fetch('/api/time').then(r => r.json()).then(data => {";
        html += "    document.getElementById('time').innerText = data.time;";
        html += "    document.getElementById('status').innerText = data.status;";
        html += "  });";
        html += "}";
        html += "setInterval(updateTime, 10000);";
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
            html += "<div class='status' id='status'>Remaining Time</div>";
        } else {
            html += "<div class='time' id='time'>--:--</div>";
            html += "<div class='status' id='status'>No data</div>";
        }
        
        html += "<div style='margin-top: 30px;'>";
        html += "<a href='/logs'>View Logs</a>";
        html += "<a href='/webserial'>WebSerial</a>";
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
        
        json += "\",\"status\":\"" + status + "\"}";
        request->send(200, "application/json", json);
    });

    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Sorry, not found. Did you mean to go to /webserial ?");
    });

    server.begin();
    
    // Setup mDNS for dishwasher.local (after server starts)
    if (MDNS.begin("dishwasher")) {
        Serial.println("mDNS responder started: dishwasher.local");
        MDNS.addService("http", "tcp", 80);
    } else {
        Serial.println("Error setting up mDNS responder!");
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
        logMessage((
          String("Available commands:\n") +
          "nothing\n"
        ).c_str());
    } else {
        logMessage(String("Unknown command '") + command + "' with value '" + value +"'");
    }
}

void loop() {
    OTA::handle();
    MDNS.update();
    // read all available bytes
    while (dbus.available()) {
        uint8_t b = dbus.read();
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
                        if (frameBuffer[frameLen - 1] == 0x6A && frameBuffer[2] == 0x20) {
                            char logLine[256];
                            int pos = snprintf(logLine, sizeof(logLine), "[TimeLight FRAME] ");
                            
                            for (int i = 0; i < frameLen && pos < 200; i++) {
                                pos += snprintf(logLine + pos, sizeof(logLine) - pos, "%02x ", frameBuffer[i]);
                            }
                            
                            if (frameLen >= 5 && frameBuffer[3] == 0x08) {
                                uint8_t hexValue = frameBuffer[4];
                                remainingHours = hexValue / 60;
                                remainingMinutes = hexValue % 60;
                                lastUpdateTime = millis();
                                snprintf(logLine + pos, sizeof(logLine) - pos, "Remaining time: %02d:%02d", remainingHours, remainingMinutes);
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
}
