# Dual Zone Irrigation Controller (NodeMCU ESP8266)

This project implements a dual-zone irrigation controller using a NodeMCU (ESP8266).  
Features include:

- Web UI (Admin / Guest) to view status, manual control and configure schedules
- Two independently controlled irrigation zones (valves) + pump interlock
- Per-zone schedules (start time + duration)
- Soil moisture safety checks (analog/digital modes)
- Optional rain sensor interlock
- Persistent settings saved in SPIFFS (/config.json)
- Simple logs accessible from the dashboard

## Files
- `dual_irrigation_station.ino` — main Arduino IDE sketch
- (Optional) store `config.json` in SPIFFS or let device create defaults

## Hardware (example)
- NodeMCU ESP8266
- Relay module x3 (Zone1 valve, Zone2 valve, Pump)
- Soil moisture sensors:
  - Option A (single analog A0): use an external analog multiplexer (e.g., CD74HC4051) to read two sensors on A0, or manually wire/replace as needed.
  - Option B (digital capacitive sensors): use two digital sensor modules (digital outputs).
  - Option C (ADS1115) — add ADS1115 to read multiple analog channels (requires library & small code change).
- Rain sensor (digital) -> D7 (active HIGH when rain)
- Power: use separate power supply for pump/valves. Do NOT power relays/pump from NodeMCU USB.
- Common GND required.

## Pin mapping (default in code)
- D1 — Zone1 relay
- D2 — Zone2 relay
- D3 — Pump relay
- D0 — Status LED (onboard)
- A0 — Analog moisture (if SINGLE_ANALOG_MODE)
- D7 — Rain sensor
- D5/D6 — Digital moisture sensors (if USE_DIGITAL_MOISTURE)

## Libraries
- FS (SPIFFS) — built into ESP8266 core
- ESP8266WiFi
- ESP8266WebServer
- ArduinoJson
- Ticker (built into core)
- (Optional) ADS1115 library if you enable USE_ADS1115 in code

## How to upload
1. Install ESP8266 board package in Arduino IDE.
2. Open `dual_irrigation_station.ino`.
3. Adjust WiFi creds, admin/guest passwords, moisture thresholds, and schedules near top of file.
4. Select board: NodeMCU 1.0 (ESP-12E Module)
5. Upload.
6. After boot, access device web UI:
   - If connected to WiFi: `http://<device-ip>/`
   - If WiFi failed: device sets up `Irrigation-Setup` AP; connect to it and open `http://192.168.4.1/` (if served).

## Configuration & Operation
- The device stores settings in `/config.json` within SPIFFS; editing via serial or replacing the file is possible but the web UI offers schedule & threshold changes (admin only).
- Schedules are evaluated each minute; scheduled runs begin when hour/minute match exactly. Durations are in seconds.
- Moisture thresholds: calibrate your sensors. The default is 600 (for analog where lower = wetter). Adjust to your sensors.

## Safety & Notes
- Always use separate power for valves / pump. The NodeMCU I/O pins cannot source motor/pump current.
- Use flyback diodes, snubbers, or opto-isolated relays for inductive loads.
- The controller includes software safety timeouts (max valve run) but hardware fail-safes are recommended (flow sensor cut-off, pressure switch, float switch).
- Rain sensor polarity and moisture sensor polarity depend on the module you use. Adjust logic in the code accordingly.

## Extending
- Add MQTT integration to publish status to home automation (Home Assistant).
- Add flow sensor to detect leaks or valve failures.
- Add field calibration page to fine-tune moisture thresholds.
- Add mobile-friendly web UI (single page app) served from SPIFFS.

## License
MIT
