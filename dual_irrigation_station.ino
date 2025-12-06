/*
  dual_irrigation_station.ino
  NodeMCU (ESP8266) Dual Zone Irrigation Controller
  - Web UI with Admin / Guest roles
  - Manual ON/OFF for Zone1, Zone2, Pump
  - Per-zone schedules (start time hh:mm and duration seconds/minutes)
  - Soil moisture safety per-zone (analog or digital sensors)
  - Optional rain sensor interlock
  - Pump interlock prevents pump + zone off combination errors
  - Persistent settings stored in SPIFFS (/config.json)
  - Simple logging ring buffer viewable in web UI
  - No command line required (Arduino IDE compatible)
  - Author: generated for user request
  - Date: 2025

  NOTES / ASSUMPTIONS:
  - NodeMCU ESP8266 used. If you use ESP32, change pin defines & WebServer include.
  - NodeMCU has a single ADC pin (A0). The sketch supports:
      1) SINGLE_ANALOG_MODE: both moisture sensors are read using an external analog
         multiplexer or manual wiring to A0 (user must wire hardware), OR
      2) USE_DIGITAL_MOISTURE: two digital-capacitive sensors (D0..D1) that output HIGH/LOW,
      3) USE_ADS1115: optional ADS1115 I2C ADC (uncomment and add library).
  - Relays are driven directly from digital pins (use opto/driver module and correct power).
  - Use separate power for pump and valves; common ground required.
  - Safety: The code includes timeouts and interlocks but does not replace hardware limit switches.
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <FS.h>               // SPIFFS
#include <ArduinoJson.h>
#include <Ticker.h>
#include <time.h>

// ---------- USER CONFIG -------------
const char* WIFI_SSID = "YourWiFiSSID";
const char* WIFI_PASS = "YourWiFiPassword";
String ADMIN_PW = "admin123";
String GUEST_PW = "guest123";
const int WEB_PORT = 80;

// ---------- HARDWARE PINOUT -----------
#define RELAY_ZONE1 D1       // Relay for valve zone 1
#define RELAY_ZONE2 D2       // Relay for valve zone 2
#define RELAY_PUMP  D3       // Relay for pump master
#define LED_STATUS  D0       // Status LED (onboard)

// Soil moisture options - choose one mode by compile flag
#define SINGLE_ANALOG_MODE     // read moisture from A0 for zone sensors (requires mux/switch) 
// #define USE_DIGITAL_MOISTURE  // use two digital moisture modules (D5, D6)
// #define USE_ADS1115           // use ADS1115 I2C ADC (not implemented out-of-the-box)

// If USE_DIGITAL_MOISTURE:
#define MOIST_DIGITAL_1 D5
#define MOIST_DIGITAL_2 D6

// Analog pin (NodeMCU) - A0
#define MOIST_ANALOG_PIN A0

// Rain sensor pin (digital)
#define RAIN_PIN D7          // Active HIGH = raining (or wire inverted if needed)

// Safety timeouts
const unsigned long MAX_VALVE_RUN_MS = 30UL * 60UL * 1000UL; // 30 minutes max per zone
const unsigned long PUMP_MIN_RUN_MS = 2000; // pump minimum run time once started (ms)

// ---------- SCHEDULING / OPERATIONAL ------------
struct Schedule {
  bool enabled = false;
  int startHour = 6;        // 0-23
  int startMinute = 0;      // 0-59
  unsigned long durationSec = 300; // how many seconds to run
};

struct DeviceState {
  bool zone1 = false;
  bool zone2 = false;
  bool pump  = false;
  unsigned long zone1StartTs = 0;
  unsigned long zone2StartTs = 0;
};

Schedule schedZone1;
Schedule schedZone2;
DeviceState state;

// ---------- PERSISTENT STORAGE ------------
const char* CONFIG_PATH = "/config.json";

struct Config {
  String adminPw;
  String guestPw;
  Schedule s1;
  Schedule s2;
  int moistThreshold1; // for analog mode: value 0-1023 below which start allowed
  int moistThreshold2;
  bool rainInterlock;
  String wifiSsid;
  String wifiPass;
} cfg;

// ---------- LOGGING (ring buffer) ----------
#define LOG_SLOTS 100
String logs[LOG_SLOTS];
int logIdx = 0;
void pushLog(const String &msg) {
  logs[logIdx++] = String(millis()) + ": " + msg;
  if (logIdx >= LOG_SLOTS) logIdx = 0;
}

// ---------- WEB SERVER ------------
ESP8266WebServer server(WEB_PORT);

// ---------- STATUS LED TICKER -----------
Ticker statusTicker;
bool ledToggle = false;
void tickLED() {
  ledToggle = !ledToggle;
  digitalWrite(LED_STATUS, ledToggle ? LOW : HIGH); // NodeMCU LED is active LOW
}

// ---------- TIME (NTP) -------------
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 0;
void initTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

// ---------- UTILS -------------
String isoNow() {
  time_t t = time(nullptr);
  if (t == 0) return String(millis()/1000) + "s";
  char buf[32];
  struct tm *tmInfo = localtime(&t);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tmInfo);
  return String(buf);
}

String jsonError(const String &msg) {
  StaticJsonDocument<128> doc;
  doc["ok"] = false;
  doc["error"] = msg;
  String out; serializeJson(doc, out); return out;
}

String jsonOk(const String &msg = "") {
  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  if (msg.length()) doc["msg"] = msg;
  String out; serializeJson(doc, out); return out;
}

// ---------- HARDWARE CONTROL ------------
void setRelay(int pin, bool on) {
  // if relay is active LOW, invert here
  digitalWrite(pin, on ? HIGH : LOW);
}

void applyRelays() {
  setRelay(RELAY_ZONE1, state.zone1);
  setRelay(RELAY_ZONE2, state.zone2);
  setRelay(RELAY_PUMP, state.pump);
}

// turn zone ON with pump handling
void startZone(int zone) {
  unsigned long now = millis();
  if (cfg.rainInterlock) {
    int rain = digitalRead(RAIN_PIN);
    if (rain == HIGH) {
      pushLog("Rain detected: blocking start of zone " + String(zone));
      return;
    }
  }

  if (zone == 1) {
    // ensure pump on
    if (!state.pump) {
      state.pump = true;
      pushLog("Pump started for zone1");
      delay(50); // short settle
    }
    state.zone1 = true;
    state.zone1StartTs = now;
    pushLog("Zone1 ON");
  } else if (zone == 2) {
    if (!state.pump) {
      state.pump = true;
      pushLog("Pump started for zone2");
      delay(50);
    }
    state.zone2 = true;
    state.zone2StartTs = now;
    pushLog("Zone2 ON");
  }
  applyRelays();
}

// stop zone and possibly pump
void stopZone(int zone) {
  if (zone == 1) {
    state.zone1 = false;
    state.zone1StartTs = 0;
    pushLog("Zone1 OFF");
  } else if (zone == 2) {
    state.zone2 = false;
    state.zone2StartTs = 0;
    pushLog("Zone2 OFF");
  }
  // if both zones off, stop pump after short delay to avoid wear
  if (!state.zone1 && !state.zone2) {
    unsigned long ts = millis();
    // ensure pump ran at least min runtime
    if (state.pump && ts - max(state.zone1StartTs, state.zone2StartTs) < PUMP_MIN_RUN_MS) {
      delay(PUMP_MIN_RUN_MS);
    }
    state.pump = false;
    pushLog("Pump stopped (no active zones)");
  }
  applyRelays();
}

// emergency stop all
void stopAll() {
  state.zone1 = false;
  state.zone2 = false;
  state.pump = false;
  state.zone1StartTs = 0;
  state.zone2StartTs = 0;
  applyRelays();
  pushLog("Emergency stop: all off");
}

// ---------- SENSORS -------------
int readAnalogMoisture1() {
#ifdef SINGLE_ANALOG_MODE
  // In single-analog mode you must wire the multiplexer to present zone1 to A0 when called.
  // This function simply reads A0.
  int v = analogRead(MOIST_ANALOG_PIN);
  return v;
#else
  return -1;
#endif
}

int readAnalogMoisture2() {
#ifdef SINGLE_ANALOG_MODE
  // In single-analog mode you must wire mux to present zone2 to A0 before calling.
  int v = analogRead(MOIST_ANALOG_PIN);
  return v;
#else
  return -1;
#endif
}

bool readDigitalMoisture1() {
#ifdef USE_DIGITAL_MOISTURE
  return digitalRead(MOIST_DIGITAL_1) == HIGH; // adjust polarity as needed
#else
  return false;
#endif
}

bool readDigitalMoisture2() {
#ifdef USE_DIGITAL_MOISTURE
  return digitalRead(MOIST_DIGITAL_2) == HIGH;
#else
  return false;
#endif
}

bool isSoilWetEnoughForZone(int zone) {
  // Returns true if moisture reading indicates watering is NOT needed (i.e., soil wet)
  if (cfg.moistThreshold1 <= 0 && cfg.moistThreshold2 <= 0) {
    // thresholds not configured: allow watering
    return false;
  }

#ifdef USE_DIGITAL_MOISTURE
  if (zone == 1) return readDigitalMoisture1(); // true => wet
  else return readDigitalMoisture2();
#else
  // ANALOG: read and compare threshold
  if (zone == 1) {
    int v = readAnalogMoisture1();
    if (v < 0) return false;
    // Note: many sensors produce lower value = wetter (sensor dependent). Here we assume lower = wetter.
    // If your sensor is opposite, invert comparison or calibrate threshold accordingly.
    return (v <= cfg.moistThreshold1);
  } else {
    int v = readAnalogMoisture2();
    if (v < 0) return false;
    return (v <= cfg.moistThreshold2);
  }
#endif
}

// ---------- SCHEDULE CHECK ----------
bool timeMatches(int hour, int minute) {
  time_t now = time(nullptr);
  if (now == 0) return false;
  struct tm *tmInfo = localtime(&now);
  return (tmInfo->tm_hour == hour && tmInfo->tm_min == minute);
}

void checkSchedules() {
  // zone1
  if (schedZone1.enabled) {
    time_t now = time(nullptr);
    if (now != 0) {
      struct tm *tmInfo = localtime(&now);
      if (tmInfo->tm_hour == schedZone1.startHour && tmInfo->tm_min == schedZone1.startMinute) {
        // ensure not already running
        if (!state.zone1) {
          // check moisture and rain interlock
          if (cfg.rainInterlock && digitalRead(RAIN_PIN) == HIGH) {
            pushLog("Schedule blocked for zone1 due to rain");
          } else if (isSoilWetEnoughForZone(1)) {
            pushLog("Schedule skipped for zone1: soil already moist");
          } else {
            startZone(1);
          }
        }
      }
    }
  }

  // zone2
  if (schedZone2.enabled) {
    time_t now = time(nullptr);
    if (now != 0) {
      struct tm *tmInfo = localtime(&now);
      if (tmInfo->tm_hour == schedZone2.startHour && tmInfo->tm_min == schedZone2.startMinute) {
        if (!state.zone2) {
          if (cfg.rainInterlock && digitalRead(RAIN_PIN) == HIGH) {
            pushLog("Schedule blocked for zone2 due to rain");
          } else if (isSoilWetEnoughForZone(2)) {
            pushLog("Schedule skipped for zone2: soil already moist");
          } else {
            startZone(2);
          }
        }
      }
    }
  }
}

// enforce runtime limits
void enforceTimeouts() {
  unsigned long now = millis();
  if (state.zone1 && state.zone1StartTs != 0 && (now - state.zone1StartTs) > MAX_VALVE_RUN_MS) {
    pushLog("Zone1 run timeout reached, stopping");
    stopZone(1);
  }
  if (state.zone2 && state.zone2StartTs != 0 && (now - state.zone2StartTs) > MAX_VALVE_RUN_MS) {
    pushLog("Zone2 run timeout reached, stopping");
    stopZone(2);
  }
}

// ---------- PERSISTENCE (SPIFFS config) ------------
void loadConfig() {
  if (!SPIFFS.exists(CONFIG_PATH)) {
    // set defaults
    cfg.adminPw = ADMIN_PW;
    cfg.guestPw = GUEST_PW;
    cfg.s1 = schedZone1;
    cfg.s2 = schedZone2;
    cfg.moistThreshold1 = 600; // default value (calibrate)
    cfg.moistThreshold2 = 600;
    cfg.rainInterlock = true;
    cfg.wifiSsid = WIFI_SSID;
    cfg.wifiPass = WIFI_PASS;
    pushLog("Using default config");
    return;
  }
  File f = SPIFFS.open(CONFIG_PATH, "r");
  if (!f) { pushLog("Failed open config"); return; }
  size_t size = f.size();
  std::unique_ptr<char[]> buf(new char[size + 1]);
  f.readBytes(buf.get(), size);
  buf[size] = '\0';
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, buf.get());
  if (err) {
    pushLog("Config parse error");
    f.close();
    return;
  }
  cfg.adminPw = String((const char*)doc["adminPw"] | ADMIN_PW);
  cfg.guestPw = String((const char*)doc["guestPw"] | GUEST_PW);
  cfg.s1.enabled = doc["s1"]["enabled"] | false;
  cfg.s1.startHour = doc["s1"]["startHour"] | schedZone1.startHour;
  cfg.s1.startMinute = doc["s1"]["startMinute"] | schedZone1.startMinute;
  cfg.s1.durationSec = doc["s1"]["durationSec"] | schedZone1.durationSec;
  cfg.s2.enabled = doc["s2"]["enabled"] | false;
  cfg.s2.startHour = doc["s2"]["startHour"] | schedZone2.startHour;
  cfg.s2.startMinute = doc["s2"]["startMinute"] | schedZone2.startMinute;
  cfg.s2.durationSec = doc["s2"]["durationSec"] | schedZone2.durationSec;
  cfg.moistThreshold1 = doc["moistThreshold1"] | 600;
  cfg.moistThreshold2 = doc["moistThreshold2"] | 600;
  cfg.rainInterlock = doc["rainInterlock"] | true;
  cfg.wifiSsid = String((const char*)doc["wifiSsid"] | WIFI_SSID);
  cfg.wifiPass = String((const char*)doc["wifiPass"] | WIFI_PASS);

  // write back to runtime schedules
  schedZone1 = cfg.s1;
  schedZone2 = cfg.s2;
  ADMIN_PW = cfg.adminPw;
  GUEST_PW = cfg.guestPw;
  f.close();
  pushLog("Config loaded");
}

void saveConfig() {
  StaticJsonDocument<1024> doc;
  doc["adminPw"] = cfg.adminPw;
  doc["guestPw"] = cfg.guestPw;
  JsonObject s1 = doc.createNestedObject("s1");
  s1["enabled"] = cfg.s1.enabled;
  s1["startHour"] = cfg.s1.startHour;
  s1["startMinute"] = cfg.s1.startMinute;
  s1["durationSec"] = cfg.s1.durationSec;
  JsonObject s2 = doc.createNestedObject("s2");
  s2["enabled"] = cfg.s2.enabled;
  s2["startHour"] = cfg.s2.startHour;
  s2["startMinute"] = cfg.s2.startMinute;
  s2["durationSec"] = cfg.s2.durationSec;
  doc["moistThreshold1"] = cfg.moistThreshold1;
  doc["moistThreshold2"] = cfg.moistThreshold2;
  doc["rainInterlock"] = cfg.rainInterlock;
  doc["wifiSsid"] = cfg.wifiSsid;
  doc["wifiPass"] = cfg.wifiPass;

  File f = SPIFFS.open(CONFIG_PATH, "w");
  if (!f) { pushLog("Failed to open config for write"); return; }
  serializeJson(doc, f);
  f.close();
  pushLog("Config saved");
}

// ---------- WEB HANDLERS ----------
bool checkAuth(String role) {
  if (!server.hasArg("pw")) return false;
  String pw = server.arg("pw");
  if (role == "admin") return (pw == cfg.adminPw);
  else return (pw == cfg.guestPw) || (pw == cfg.adminPw);
}

String pageHeader(const String &title) {
  String h = "<!doctype html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  h += "<title>" + title + "</title>";
  h += "<style>body{font-family:Arial;padding:8px;} .card{padding:10px;margin:8px 0;border:1px solid #ddd;border-radius:6px;} button{padding:8px 12px;margin:4px;}</style>";
  h += "</head><body><h2>" + title + "</h2>";
  return h;
}
String pageFooter() {
  return "<hr><div style='font-size:12px;color:#666'>Dual Zone Irrigation Controller</div></body></html>";
}

void handleRoot() {
  String html = pageHeader("Irrigation Control - Login / Status");
  html += "<div class='card'><form action='/dashboard' method='GET'>";
  html += "Password (admin or guest):<br><input name='pw' type='password' style='padding:6px;width:70%;'><br><button type='submit'>Enter</button></form></div>";
  html += pageFooter();
  server.send(200, "text/html", html);
}

void handleDashboard() {
  if (!server.hasArg("pw")) { server.sendHeader("Location", "/"); server.send(302); return; }
  String pw = server.arg("pw");
  bool isAdmin = (pw == cfg.adminPw);
  bool isGuest = (pw == cfg.guestPw) || isAdmin;
  if (!isGuest) { server.send(403, "text/plain", "Forbidden"); return; }

  String html = pageHeader("Irrigation Dashboard");
  html += "<div class='card'><h3>Device Status</h3>";
  html += "<div>Time: " + isoNow() + "</div>";
  html += "<div>Zone1: " + String(state.zone1 ? "ON" : "OFF") + "</div>";
  html += "<div>Zone2: " + String(state.zone2 ? "ON" : "OFF") + "</div>";
  html += "<div>Pump: " + String(state.pump ? "ON" : "OFF") + "</div>";
  html += "<div>Rain sensor: " + String(digitalRead(RAIN_PIN) == HIGH ? "Raining" : "No rain") + "</div>";
#ifdef USE_DIGITAL_MOISTURE
  html += "<div>Moist1: " + String(readDigitalMoisture1() ? "WET" : "DRY") + "</div>";
  html += "<div>Moist2: " + String(readDigitalMoisture2() ? "WET" : "DRY") + "</div>";
#else
  html += "<div>Moist1 (A0): " + String(readAnalogMoisture1()) + "</div>";
  html += "<div>Moist2 (A0): " + String(readAnalogMoisture2()) + "</div>";
#endif
  html += "</div>";

  // manual controls
  html += "<div class='card'><h3>Manual Controls</h3>";
  html += "<form action='/toggleZone1' method='GET'><input type='hidden' name='pw' value='" + pw + "'>";
  html += "<button type='submit'>" + String(state.zone1 ? "Stop Zone1" : "Start Zone1") + "</button></form>";

  if (isAdmin) {
    html += "<form action='/toggleZone2' method='GET' style='display:inline-block;margin-left:6px;'><input type='hidden' name='pw' value='" + pw + "'>";
    html += "<button type='submit'>" + String(state.zone2 ? "Stop Zone2" : "Start Zone2") + "</button></form>";
    html += "<form action='/stopAll' method='GET' style='display:inline-block;margin-left:6px;'><input type='hidden' name='pw' value='" + pw + "'>";
    html += "<button type='submit'>Stop All</button></form>";
  }

  html += "</div>";

  // schedules & admin
  html += "<div class='card'><h3>Schedules</h3>";
  html += "<div>Zone1: " + String(schedZone1.enabled ? "ENABLED" : "DISABLED") + " @ ";
  html += String(schedZone1.startHour) + ":" + (schedZone1.startMinute < 10 ? "0" : "") + String(schedZone1.startMinute);
  html += " for " + String(schedZone1.durationSec) + "s</div>";
  html += "<div>Zone2: " + String(schedZone2.enabled ? "ENABLED" : "DISABLED") + " @ ";
  html += String(schedZone2.startHour) + ":" + (schedZone2.startMinute < 10 ? "0" : "") + String(schedZone2.startMinute);
  html += " for " + String(schedZone2.durationSec) + "s</div>";

  if (isAdmin) {
    html += "<form action='/saveSchedule' method='POST'><input type='hidden' name='pw' value='" + pw + "'>";
    html += "<h4>Zone1</h4>Enable: <input type='checkbox' name='z1en' " + String(schedZone1.enabled ? "checked" : "") + "><br>";
    html += "Start (HH:MM): <input name='z1hh' value='" + String(schedZone1.startHour) + "' size=2> : <input name='z1mm' value='" + String(schedZone1.startMinute) + "' size=2><br>";
    html += "Duration (sec): <input name='z1dur' value='" + String(schedZone1.durationSec) + "' size=6><br>";
    html += "<h4>Zone2</h4>Enable: <input type='checkbox' name='z2en' " + String(schedZone2.enabled ? "checked" : "") + "><br>";
    html += "Start (HH:MM): <input name='z2hh' value='" + String(schedZone2.startHour) + "' size=2> : <input name='z2mm' value='" + String(sche
