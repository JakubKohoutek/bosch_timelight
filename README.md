# BOSCH Dishwasher TimeLight Module Replacement

## Motivation

The TimeLight projector module on my Bosch dishwasher (model SMV68MX04E/29) failed, and a replacement costs around €100. Since I wasn't willing to pay that much for a simple time display, I decided to build my own solution. This project creates an ESP8266-based module that listens to the dishwasher's internal communication bus and displays the remaining wash time on a web interface at `http://dishwasher.local`, completely replacing the need for the expensive projector module.

## Features

- **D-Bus 2 Protocol Reader** - Decodes Bosch dishwasher communication
- **Web Interface** - Modern display at `http://dishwasher.local` with auto-refresh
- **Logging System** - Records all frames to LittleFS with automatic truncation for further analysis
- **WebSerial Console** - Remote debugging at `/webserial`
- **OTA Updates** - Over-the-air firmware updates

## Hardware Setup

- **ESP8266** (Wemos D1 Mini in my case)
- **Connection** - GPIO4 (D2) to D-Bus DATA line (must lower the voltage!), RX-only via SoftwareSerial
- **Baud Rate** - 9600 (8N1)

### Time Light module

The Time Light module is connected with 3 wires: 
- Gnd
- Vcc (12 volts)
- Data (5 volts)


## WiFi Configuration

Create a `credentials.h` file in your Arduino libraries folder at `Arduino/libraries/Credentials/credentials.h`:

```cpp
#define STASSID "your-wifi-ssid"
#define STAPSK "your-wifi-password"
```

On macOS/Linux, the typical path is: `~/Documents/Arduino/libraries/Credentials/credentials.h`  
On Windows: `Documents\Arduino\libraries\Credentials\credentials.h`

## Web Interface

- **Main page:** `http://dishwasher.local/` - Shows remaining time
- **Logs:** `http://dishwasher.local/logs` - Frame logs
- **WebSerial:** `http://dishwasher.local/webserial` - Debug console
- **API:** `http://dishwasher.local/api/time` - JSON endpoint: `{"time":"01:23","status":"Remaining Time"}`

## D-Bus 2 Protocol

### Overview

D-Bus 2 is a single-wire bus protocol used in Bosch appliances. All participants read/write simultaneously on the DATA line at 9600 baud (8N1), though newer devices may use up to 38400 baud.

### Frame Format

**Example - Generic Frame:**
```
05 14 10 05 00 FF 00 DE 62 1A
│  │  └─────┬──────┘ └─┬─┘ │
│  │        │          │   └─ ACK byte: 0x1A
│  │        │          └───── CRC16-XMODEM Checksum (2 bytes): 0xDE62
│  │        └──────────────── 5 data bytes (command, [subcommand] + parameters)
│  └───────────────────────── DS: 0x14 (Dest=0x1, Sub=0x4)
└──────────────────────────── Length: 5 data bytes (except dest, checksum and ack)
```

**Time Information Frame:**
```
0a 65 20 08 49 34 30 07 37 00 01 0d 8a 83 6a
│  │  └─┬─┘ └──────────┬──────────┘ └─┬─┘ │
│  │    │              │              │   └─ ACK by TimeLight module: 0x6A
│  │    │              │              └───── Checksum: 0x8A83
│  │    │              └──────────────────── Bytes with parameters (8 bytes)
│  │    └─────────────────────────────────── Command and subcommand (2 bytes)
│  └──────────────────────────────────────── Destination
└─────────────────────────────────────────── Length (10 bytes)

Time frame structure:
0a      - Length: 10 (0x0a) data bytes
65      - TimeLight module destination
20      - Time/Display command group
08      - Subcommand - "Full time report"
49      - Remaining time value: 73 (0x49) minutes = 1h 13m
34 30 07 37 - Other data, uncertain meaning, but maybe:
└─┬─┘ │  └─── Counter that increases by 1 every minute if program is not running. Maximum value 
  │   │       is 0x3b (0-59). Stops counting and is fixed since the progam is started 
  │   │       - end time minutes according to some internal clock?
  │   └────── Counter that increases by 1 once the following (minute?) byte reaches 60
  │           if program is not running. Stops counting and is fixed since the progam is started 
  │           - end time hours according to some internal clock?
  └────────── Program (e.g. Eco, Normal) and attributes (e.g. Hygiene+, steam) bytes
00 - Washing phase (0x03 Pre-wash, 0x02 Wash, 0x01 Rinse, 0x00 Dry)
01 0d   - Remaining time value again in two bytes: 01 (0x01) hours, 13 (0x0d) minutes
8a 83   - Checksum
6a      - ACK
```

**Other useful commands:**
```
05 65 20 04 00 00 00 a7 fb 6a  Subcommand 04 + params 00 => program start
03 65 20 00 64 a1 a1 6a        Subcommand 00 + param from 0 to 0x64 (100), progress report (end at 0x64 %)
03 65 20 06 02 07 67 6a        Subcommand 06 => sensor status, door open (0b0???) vs. closed (0b1???)
03 65 20 12 00 e8 92 6a        Subcommand 12 + param 00 => machine off
```

### Acknowledgement

After each frame, sender waits ~1.042ms (1 byte time at 9600 baud).
Receiver inserts ACK byte there:
```
ACK = 0x0A | (DS & 0xF0)
```
- Lower nibble: Always `0xA`
- Upper nibble: Mirrors destination from DS
- `0x6A` therefore means "TimeLight module acknowledged"

### Implementation Notes

This project detects frames by looking for a specific pattern:
```
[Length=0x??] 0x65 0x20 [data...] [checksum] 0x6A
```

**Detection strategy:**
- Monitors the bus for the byte sequence that precedes `0x65`
- When `0x65` is detected, captures the previous byte (length) and `0x65` itself
- Continues reading until frame is complete based on length
- Validates frame ends with acknowledgement byte `0x6A`
- Frame timeout: 2000µs gap indicates end of transmission

**Why this works for TimeLight:**
- TimeLight frames consistently use DS=0x20
- The length byte appears before what the code identifies as 0x65
- This pattern is specific enough to reliably detect time information

**Important:** This implementation is optimized for TimeLight time reporting frames.
Other D-Bus 2 commands with different DS values or message formats exist but are not captured.

## References

- [B/S/H/ Home Appliances GitHub project](https://github.com/hn/bsh-home-appliances/)
- [Blogpost by nophead](https://hydraraptor.blogspot.com/2022/07/diy-repair-nightmare.html)