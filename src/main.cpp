/*
 * ERA Smart Node — Standalone Edition [v4.0]
 * ─────────────────────────────────────────────────────────────────────────
 * REWRITTEN BY ERA
 * - Fully Standalone (No Master Server Dependency)
 * - ESPAsyncWebServer for local incoming HTTP control
 * - Capacitive Touch priority overrides
 * - Async WiFi Events
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>
#include <ArduinoOTA.h>
#include <Preferences.h>


// =========================================================================
// CONFIG
// =========================================================================
const char* WIFI_SSID        = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD    = "YOUR_WIFI_PASSWORD";
const char* NODE_API_KEY     = "CHANGE_THIS_KEY"; // Required for incoming requests
const char* NODE_ID          = "appliance-01";
const char* NODE_NAME        = "ERA Appliance Node";
const int   LOCAL_PORT       = 80;
const bool  RELAY_ACTIVE_LOW = true;

// Hardware counts
#define RELAY_COUNT  6
#define TOUCH_COUNT  6
#define LED_PIN      2

const int RELAY_PINS[RELAY_COUNT] = {26, 27, 14, 25, 33, 32};
const int TOUCH_PINS[TOUCH_COUNT] = { 4,  5, 18, 19, 23, 13};
const char* APPLIANCE_NAMES[RELAY_COUNT] = { "Fan", "Light", "TV", "AC", "Geyser", "Pump" };

// Timing
#define DEBOUNCE_MS          200
#define COMMAND_COOLDOWN_MS  400
#define TOUCH_LOCK_MS        600
#define SERIAL_MAX_LEN       32
#define CMD_QUEUE_SIZE       16
#define WDT_TIMEOUT_SECS     10

// =========================================================================
// ENUMS & STRUCTS
// =========================================================================
enum CommandSource : uint8_t { SRC_TOUCH = 0, SRC_API = 1, SRC_SERIAL = 2 };
const char* SOURCE_NAMES[] = { "TOUCH", "API", "SERIAL" };

enum CmdAction : int8_t { ACT_OFF = 0, ACT_ON = 1, ACT_TOGGLE = 2, ACT_ALL_OFF = 3, ACT_ALL_ON = 4 };

struct Command {
  int           relayIdx;
  CmdAction     action;
  CommandSource source;
  unsigned long timestamp;
};

// =========================================================================
// GLOBALS & STATE
// =========================================================================
QueueHandle_t cmdQueue;
Preferences preferences;
portMUX_TYPE  stateMutex = portMUX_INITIALIZER_UNLOCKED;

bool          relayState[RELAY_COUNT]     = {};
bool          lastTouchState[TOUCH_COUNT] = {};
unsigned long lastDebounce[TOUCH_COUNT]   = {};
unsigned long lastCmdTime[RELAY_COUNT]    = {};
unsigned long lastTouchTime[RELAY_COUNT]  = {};

bool          wifiConnected   = false;
unsigned long lastLedToggle   = 0;
bool          ledState        = false;
String        serialBuffer    = "";

struct Diag {
  uint32_t cmdsExecuted  = 0;
  uint32_t cmdsDropped   = 0;
  uint32_t cmdsBlocked   = 0;
  uint32_t touchFires    = 0;
  uint32_t apiFires      = 0;
  uint32_t serialFires   = 0;
  uint32_t wifiDrops     = 0;
} diag;

AsyncWebServer server(LOCAL_PORT);

// =========================================================================
// LOGGING
// =========================================================================
void eraLog(const char* tag, const char* fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.printf("[%8lu][%-7s] %s\n", millis(), tag, buf);
}

// =========================================================================
// JSON BUILDERS
// =========================================================================
String getLocalIP() { return WiFi.localIP().toString(); }

String buildStatusJson() {
  StaticJsonDocument<768> doc;
  doc["node_id"]     = NODE_ID;
  doc["name"]        = NODE_NAME;
  doc["ip"]          = getLocalIP();
  doc["uptime_ms"]   = millis();
  doc["wifi"]        = wifiConnected;
  doc["queue_depth"] = uxQueueMessagesWaiting(cmdQueue);
  
  JsonObject relays = doc.createNestedObject("relays");
  portENTER_CRITICAL(&stateMutex);
  for (int i = 0; i < RELAY_COUNT; i++) {
    JsonObject r = relays.createNestedObject(String(i + 1));
    r["name"]  = APPLIANCE_NAMES[i];
    r["state"] = relayState[i];
    r["pin"]   = RELAY_PINS[i];
  }
  JsonObject touch = doc.createNestedObject("touch");
  for (int i = 0; i < TOUCH_COUNT; i++) touch[String(i + 1)] = lastTouchState[i];
  portEXIT_CRITICAL(&stateMutex);
  
  String out; serializeJson(doc, out);
  return out;
}

String buildDiagJson() {
  StaticJsonDocument<256> doc;
  doc["uptime_ms"]      = millis();
  doc["queue_depth"]    = uxQueueMessagesWaiting(cmdQueue);
  doc["cmds_executed"]  = diag.cmdsExecuted;
  doc["cmds_dropped"]   = diag.cmdsDropped;
  doc["cmds_blocked"]   = diag.cmdsBlocked;
  doc["touch_fires"]    = diag.touchFires;
  doc["api_fires"]      = diag.apiFires;
  doc["serial_fires"]   = diag.serialFires;
  doc["wifi_drops"]     = diag.wifiDrops;
  doc["free_heap"]      = ESP.getFreeHeap();
  String out; serializeJson(doc, out);
  return out;
}

String buildRelayJson(bool ok, int relayNum, bool state, const char* msg) {
  String s = String("{\"ok\":") + (ok ? "true" : "false")
           + ",\"relay\":"   + relayNum
           + ",\"name\":\""  + APPLIANCE_NAMES[relayNum - 1] + "\""
           + ",\"state\":"   + (state ? "true" : "false");
  if (strlen(msg) > 0) s += String(",\"msg\":\"") + msg + "\"";
  s += "}";
  return s;
}

// =========================================================================
// QUEUE MANAGEMENT
// =========================================================================
bool enqueueCommand(int relayIdx, CmdAction action, CommandSource src) {
  if (relayIdx != -1 && (relayIdx < 0 || relayIdx >= RELAY_COUNT)) {
    eraLog("QUEUE", "Bad relay idx %d -- rejected", relayIdx);
    diag.cmdsDropped++; return false;
  }

  Command cmd = { relayIdx, action, src, millis() };
  
  if (xQueueSend(cmdQueue, &cmd, 0) != pdPASS) {
    if (src == SRC_TOUCH) {
      Command discarded;
      xQueueReceive(cmdQueue, &discarded, 0); // Evict oldest
      eraLog("QUEUE", "Evicted oldest command for TOUCH priority");
      diag.cmdsDropped++;
      xQueueSend(cmdQueue, &cmd, 0);
      eraLog("QUEUE", "+cmd relay=%d act=%d src=TOUCH (Priority)", relayIdx + 1, (int)action);
      return true;
    } else {
      eraLog("QUEUE", "Full -- dropping src=%s", SOURCE_NAMES[src]);
      diag.cmdsDropped++;
      return false;
    }
  }
  
  eraLog("QUEUE", "+cmd relay=%d act=%d src=%s depth=%d", 
         relayIdx + 1, (int)action, SOURCE_NAMES[src], uxQueueMessagesWaiting(cmdQueue));
  return true;
}

// =========================================================================
// HARDWARE EXECUTION
// =========================================================================
bool executeRelay(int idx, bool on, CommandSource src) {
  if (idx < 0 || idx >= RELAY_COUNT) return false;
  unsigned long now = millis();

  portENTER_CRITICAL(&stateMutex);
  if (src > SRC_TOUCH && (now - lastTouchTime[idx]) < TOUCH_LOCK_MS) {
    portEXIT_CRITICAL(&stateMutex);
    eraLog("PRIO", "Relay %d blocked touch-lock src=%s", idx + 1, SOURCE_NAMES[src]);
    diag.cmdsBlocked++; return false;
  }
  if (src > SRC_TOUCH && (now - lastCmdTime[idx]) < COMMAND_COOLDOWN_MS) {
    portEXIT_CRITICAL(&stateMutex);
    eraLog("RATE", "Relay %d rate-limited src=%s", idx + 1, SOURCE_NAMES[src]);
    diag.cmdsBlocked++; return false;
  }

  relayState[idx] = on;
  digitalWrite(RELAY_PINS[idx], RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
  
  // Save to NVS outside critical section
  portEXIT_CRITICAL(&stateMutex);
  preferences.putBool(String(idx).c_str(), on);
  
  portENTER_CRITICAL(&stateMutex);
  lastCmdTime[idx] = now;
  if (src == SRC_TOUCH) lastTouchTime[idx] = now;
  portEXIT_CRITICAL(&stateMutex);

  eraLog("RELAY", "%s -> %s (src=%s)", APPLIANCE_NAMES[idx], on ? "ON" : "OFF", SOURCE_NAMES[src]);
  diag.cmdsExecuted++;

  return true;
}

void processCommandQueue() {
  Command cmd;
  if (xQueueReceive(cmdQueue, &cmd, 0) == pdPASS) {
    if (cmd.action == ACT_ALL_OFF || cmd.action == ACT_ALL_ON) {
      bool target = (cmd.action == ACT_ALL_ON);
      portENTER_CRITICAL(&stateMutex);
      for (int i = 0; i < RELAY_COUNT; i++) {
        relayState[i] = target;
        digitalWrite(RELAY_PINS[i], RELAY_ACTIVE_LOW ? (target ? LOW : HIGH) : (target ? HIGH : LOW));
        eraLog("RELAY", "%s -> %s (ALL src=%s)", APPLIANCE_NAMES[i], target ? "ON" : "OFF", SOURCE_NAMES[cmd.source]);
      }
      portEXIT_CRITICAL(&stateMutex);
      for (int i = 0; i < RELAY_COUNT; i++) preferences.putBool(String(i).c_str(), target);
      diag.cmdsExecuted++;
    } else {
      bool targetOn;
      portENTER_CRITICAL(&stateMutex);
      if (cmd.action == ACT_TOGGLE) targetOn = !relayState[cmd.relayIdx];
      else targetOn = (cmd.action == ACT_ON);
      portEXIT_CRITICAL(&stateMutex);
      
      executeRelay(cmd.relayIdx, targetOn, cmd.source);
    }
  }
}

// =========================================================================
// SERIAL HANDLER
// =========================================================================
void processSerialCommand(String cmd) {
  cmd.trim(); cmd.toUpperCase();
  if (cmd.length() == 0 || cmd.length() > SERIAL_MAX_LEN) return;
  eraLog("SERIAL", "CMD: %s", cmd.c_str());
  diag.serialFires++;

  if (cmd == "STATUS") { Serial.println(buildStatusJson()); return; }
  if (cmd == "DIAG")   { Serial.println(buildDiagJson());   return; }

  if (cmd == "ALL:OFF") {
    enqueueCommand(-1, ACT_ALL_OFF, SRC_SERIAL);
    Serial.println("{\"ok\":true,\"msg\":\"all-off queued\"}"); return;
  }
  if (cmd == "ALL:ON") {
    enqueueCommand(-1, ACT_ALL_ON, SRC_SERIAL);
    Serial.println("{\"ok\":true,\"msg\":\"all-on queued\"}"); return;
  }

  if (cmd[0] != 'R') { Serial.println("{\"ok\":false,\"error\":\"Use R<n>:ON|OFF|TOGGLE\"}"); return; }
  int colonPos = cmd.indexOf(':');
  if (colonPos < 2) { Serial.println("{\"ok\":false,\"error\":\"Missing colon\"}"); return; }
  int relayNum = cmd.substring(1, colonPos).toInt();
  if (relayNum < 1 || relayNum > RELAY_COUNT) {
    Serial.printf("{\"ok\":false,\"error\":\"Relay %d out of range\"}\n", relayNum); return;
  }
  
  String action = cmd.substring(colonPos + 1);
  CmdAction act;
  if      (action == "ON")     act = ACT_ON;
  else if (action == "OFF")    act = ACT_OFF;
  else if (action == "TOGGLE") act = ACT_TOGGLE;
  else { Serial.printf("{\"ok\":false,\"error\":\"Unknown action %s\"}\n", action.c_str()); return; }

  bool q = enqueueCommand(relayNum - 1, act, SRC_SERIAL);
  
  portENTER_CRITICAL(&stateMutex);
  bool state = relayState[relayNum - 1];
  portEXIT_CRITICAL(&stateMutex);
  
  Serial.println(buildRelayJson(q, relayNum, state, q ? "queued" : "queue full"));
}

void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) { processSerialCommand(serialBuffer); serialBuffer = ""; }
    } else {
      if (serialBuffer.length() < SERIAL_MAX_LEN) serialBuffer += c;
    }
  }
}

// =========================================================================
// ASYNC REST API
// =========================================================================
bool checkAuth(AsyncWebServerRequest *request) {
  String key;
  if (request->hasHeader("X-API-Key")) key = request->header("X-API-Key");
  else if (request->hasParam("api_key")) key = request->getParam("api_key")->value();
  
  if (key != String(NODE_API_KEY)) {
    eraLog("AUTH", "Rejected -- bad or missing API key");
    request->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}");
    return false;
  }
  return true;
}

void registerRelayEndpoints(int relayNum) {
  int idx = relayNum - 1;
  String base = "/relay/" + String(relayNum);

  server.on((base + "/on").c_str(), HTTP_POST, [idx, relayNum](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    bool q = enqueueCommand(idx, ACT_ON, SRC_API);
    diag.apiFires++;
    portENTER_CRITICAL(&stateMutex); bool state = relayState[idx]; portEXIT_CRITICAL(&stateMutex);
    request->send(q ? 202 : 429, "application/json", buildRelayJson(q, relayNum, state, q ? "queued" : "queue full"));
  });

  server.on((base + "/off").c_str(), HTTP_POST, [idx, relayNum](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    bool q = enqueueCommand(idx, ACT_OFF, SRC_API);
    diag.apiFires++;
    portENTER_CRITICAL(&stateMutex); bool state = relayState[idx]; portEXIT_CRITICAL(&stateMutex);
    request->send(q ? 202 : 429, "application/json", buildRelayJson(q, relayNum, state, q ? "queued" : "queue full"));
  });

  server.on((base + "/toggle").c_str(), HTTP_POST, [idx, relayNum](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    bool q = enqueueCommand(idx, ACT_TOGGLE, SRC_API);
    diag.apiFires++;
    portENTER_CRITICAL(&stateMutex); bool state = relayState[idx]; portEXIT_CRITICAL(&stateMutex);
    request->send(q ? 202 : 429, "application/json", buildRelayJson(q, relayNum, state, q ? "queued" : "queue full"));
  });
}

void setupAsyncAPI() {
  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true,\"node\":\"" + String(NODE_ID) + "\",\"uptime_ms\":" + String(millis()) + "}");
  });
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) { request->send(200, "application/json", buildStatusJson()); });
  server.on("/diag",   HTTP_GET, [](AsyncWebServerRequest *request) { request->send(200, "application/json", buildDiagJson()); });

  for (int i = 1; i <= RELAY_COUNT; i++) registerRelayEndpoints(i);

  server.on("/relay/all/off", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    bool q = enqueueCommand(-1, ACT_ALL_OFF, SRC_API);
    request->send(q ? 202 : 429, "application/json", q ? "{\"ok\":true,\"msg\":\"all-off queued\"}" : "{\"ok\":false,\"msg\":\"queue full\"}");
  });

  server.on("/relay/all/on", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    bool q = enqueueCommand(-1, ACT_ALL_ON, SRC_API);
    request->send(q ? 202 : 429, "application/json", q ? "{\"ok\":true,\"msg\":\"all-on queued\"}" : "{\"ok\":false,\"msg\":\"queue full\"}");
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    if(request->method() == HTTP_OPTIONS) { request->send(200); }
    else { request->send(404, "application/json", "{\"ok\":false,\"error\":\"Not found\"}"); }
  });

  server.begin();
  eraLog("API", "Async REST API ready on port %d", LOCAL_PORT);
}

// =========================================================================
// WIFI EVENTS (Async)
// =========================================================================
void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiConnected = true;
      eraLog("WIFI", "Connected: %s", WiFi.localIP().toString().c_str());
      if (MDNS.begin("era-appliance")) eraLog("MDNS", "era-appliance.local registered");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (wifiConnected) {
        eraLog("WIFI", "Lost -- reconnecting in background");
        wifiConnected = false;
        diag.wifiDrops++;
      }
      // Give WiFi stack a moment before violently reconnecting
      delay(100); 
      WiFi.disconnect(true);
      delay(100);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      break;
    default: break;
  }
}

// =========================================================================
// SETUP
// =========================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=====================================");
  Serial.println("  ERA Smart Node -- Standalone (v4)");
  Serial.println("=====================================");

  cmdQueue = xQueueCreate(CMD_QUEUE_SIZE, sizeof(Command));
  if (cmdQueue == NULL) {
    eraLog("FATAL", "RAM exhausted: cmdQueue creation failed!");
    ESP.restart();
  }

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  // Robust NVS Init
  if (!preferences.begin("relays", false)) {
    eraLog("FATAL", "NVS Mount Failed! Formatting...");
    nvs_flash_erase();
    nvs_flash_init();
    preferences.begin("relays", false);
  }
  for (int i = 0; i < RELAY_COUNT; i++) {
    bool savedState = preferences.getBool(String(i).c_str(), false);
    relayState[i] = savedState;
    
    // Set level BEFORE pinMode to guarantee zero hardware glitch/flicker on reboot
    digitalWrite(RELAY_PINS[i], RELAY_ACTIVE_LOW ? (savedState ? LOW : HIGH) : (savedState ? HIGH : LOW));
    pinMode(RELAY_PINS[i], OUTPUT);
    
    eraLog("INIT", "Relay %d (%s) GPIO %d -> Restored %s", i + 1, APPLIANCE_NAMES[i], RELAY_PINS[i], savedState ? "ON" : "OFF");
  }
  for (int i = 0; i < TOUCH_COUNT; i++) {
    pinMode(TOUCH_PINS[i], INPUT_PULLDOWN);
    eraLog("INIT", "Touch %d GPIO %d", i + 1, TOUCH_PINS[i]);
  }

  setupAsyncAPI();

  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // Enable Hardware Watchdog
  esp_task_wdt_init(WDT_TIMEOUT_SECS, true);
  esp_task_wdt_add(NULL);

  eraLog("WIFI", "Connecting to %s...", WIFI_SSID);

  ArduinoOTA.setHostname(NODE_ID);
  // ArduinoOTA.setPassword("admin"); // Uncomment if you want an OTA password
  ArduinoOTA.onStart([]() { eraLog("OTA", "Update started"); });
  ArduinoOTA.onEnd([]() { eraLog("OTA", "Update finished. Rebooting..."); });
  ArduinoOTA.onError([](ota_error_t error) {
    eraLog("FATAL", "OTA Error[%u]", error);
    ESP.restart();
  });
  ArduinoOTA.begin();

  digitalWrite(LED_PIN, LOW);
  eraLog("ERA", "Hardware Loop ready");
}

// =========================================================================
// HARDWARE LOOP - STRICTLY NON-BLOCKING
// =========================================================================
void loop() {
  esp_task_wdt_reset(); // Feed the watchdog
  unsigned long now = millis();
  ArduinoOTA.handle();
  handleSerial();

  for (int i = 0; i < TOUCH_COUNT; i++) {
    bool touched = (digitalRead(TOUCH_PINS[i]) == HIGH);
    
    portENTER_CRITICAL(&stateMutex);
    bool changed = (touched != lastTouchState[i]);
    bool debounced = ((now - lastDebounce[i]) > DEBOUNCE_MS);
    if (changed && debounced) {
      lastTouchState[i] = touched;
      lastDebounce[i]   = now;
    }
    portEXIT_CRITICAL(&stateMutex);

    if (changed && debounced && touched) {
      eraLog("TOUCH", "Sensor %d -> Relay %d (%s)", i + 1, i + 1, APPLIANCE_NAMES[i]);
      diag.touchFires++;
      enqueueCommand(i, ACT_TOGGLE, SRC_TOUCH);
    }
  }

  processCommandQueue();

  unsigned int blinkMs = wifiConnected ? 2000 : 300;
  if ((now - lastLedToggle) > blinkMs) {
    lastLedToggle = now;
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  }
}
