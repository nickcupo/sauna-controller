#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

// ------------------------------
// Hardware configuration
// ------------------------------
constexpr uint8_t DS18B20_PIN = 15;       // Temperature sensor
constexpr uint8_t I2C_SDA_PIN = 21;       // OLED SDA
constexpr uint8_t I2C_SCL_PIN = 22;       // OLED SCL
constexpr uint8_t ENCODER_CLK_PIN = 25;   // Rotary encoder CLK
constexpr uint8_t ENCODER_DT_PIN = 26;    // Rotary encoder DT
constexpr uint8_t ENCODER_SW_PIN = 27;    // Rotary encoder SW
constexpr uint8_t BULBS_PER_GROUP = 2;    // logical bulbs on each relay
constexpr uint8_t BULB_GROUP_COUNT = 3;   // three relays total
constexpr uint8_t BULB_PINS[BULB_GROUP_COUNT] = {12, 13, 14};  // one relay pin per group

// OLED
constexpr uint8_t SCREEN_WIDTH = 128;
constexpr uint8_t SCREEN_HEIGHT = 64;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Networking: copy include/secrets.example.h to include/secrets.h and fill it in.
#include "secrets.h"
constexpr const char *MDNS_HOSTNAME = "sauna"; // resolves as sauna.local

// Static IP configuration (set USE_STATIC_IP to true to enable)
constexpr bool USE_STATIC_IP = false;
IPAddress staticIP(192, 168, 1, 100);      // Choose an IP outside your DHCP range
IPAddress gateway(192, 168, 1, 1);         // Your router IP
IPAddress subnet(255, 255, 255, 0);        // Standard subnet mask
IPAddress dns(192, 168, 1, 1);             // Usually same as gateway

// ------------------------------
// Control parameters
// ------------------------------
constexpr double DEFAULT_SETPOINT_F = 160.0;
constexpr double TEMP_MIN_F = 100.0;
constexpr double TEMP_MAX_F = 200.0;
constexpr unsigned long MIN_ON_OFF_MS = 15000; // 15 seconds

// Control parameters (can be modified via web UI)
double deadbandF = 2.0;      // ±°F deadband around setpoint

// Proportional control zones (degrees below setpoint)
constexpr double ZONE_LOW = 5.0;   // 1 bulb zone (setpoint - 5°F)
constexpr double ZONE_MED = 10.0;  // 2 bulbs zone (setpoint - 10°F)
constexpr double ZONE_HIGH = 15.0; // 3 bulbs zone (setpoint - 15°F)

// Timing intervals
constexpr unsigned long TEMP_READ_INTERVAL = 30000;   // 30 seconds
constexpr unsigned long CONTROL_INTERVAL = 30000;     // 30 seconds
constexpr unsigned long DISPLAY_INTERVAL = 1000;      // 1 second refresh
constexpr unsigned long UI_TIMEOUT = 10000;           // 10 seconds to auto-return

// ------------------------------
// State structures
// ------------------------------
struct BulbState {
  uint8_t pin;
  bool on;
  unsigned long lastChange;      // last on/off timestamp
  unsigned long lastOnTimestamp;  // when turned on (for LIFO off selection)
  uint32_t cycles;
  uint64_t runtimeMs;
};

enum class UIMode { NORMAL, SETPOINT, STATS };

// ------------------------------
// Globals
// ------------------------------
OneWire oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);
Preferences prefs;
WebServer server(80);

BulbState bulbs[BULB_GROUP_COUNT];
bool adjacency[BULB_GROUP_COUNT][BULB_GROUP_COUNT] = {
  {false, true, false},
  {true, false, true},
  {false, true, false}}; // linear layout, configurable per group

bool manualMode = false;
bool heatEnabled = true; // master enable for thermostat mode
bool otaInProgress = false;

double currentTempF = 0;
double setpointF = DEFAULT_SETPOINT_F;
uint8_t targetBulbCount = 0; // 0-3 bulbs based on temp difference

// Default control values
constexpr double DEFAULT_DEADBAND = 2.0;

// UI
volatile int8_t encoderDelta = 0;
UIMode uiMode = UIMode::NORMAL;
unsigned long lastInteraction = 0;
bool settingMode = false;  // True when adjusting setpoint
unsigned long lastFlash = 0;  // For flashing display in set mode
bool flashState = true;  // Current flash state (on/off)

// Button debounce state
volatile int buttonState = HIGH;
volatile unsigned long lastButtonChange = 0;
constexpr unsigned long BUTTON_DEBOUNCE_MS = 50;  // 50ms debounce

// Encoder state tracking
volatile int lastEncoderCLK = HIGH;
volatile int lastEncoderDT = HIGH;
volatile unsigned long lastEncoderChange = 0;

// Timers
unsigned long lastTempRead = 0;
unsigned long lastControl = 0;
unsigned long lastDisplay = 0;
unsigned long lastServerLog = 0;

// Stats persistence
uint8_t cyclesSinceSave = 0;

// Forward declarations
void initHardware();
void initDisplay();
void initNetwork();
void initOTA();
void initWebServer();
void loadPreferences();
void saveSetpoint();
void saveCycles();
void saveHeatEnabled();
void saveThresholds();
void loadThresholds();
void readTemperature();
void updateControl();
void updateBulbs();
void handleEncoder();
void handleButton();
void renderDisplay();
void renderNormal();
void renderSetpoint();
void renderStats();
void handleRoot();
void handleStatus();
void handleSetpoint();
void handleBulbs();
void handleStats();
void handleManualMode();
void handlePower();
void handleToggle();
void handleThresholds();
void handleResetThresholds();
uint8_t calculateTargetBulbs();
int selectBulbToTurnOn();
int selectBulbToTurnOff();
bool hasActiveAdjacent(uint8_t index);
void applyBulbState(uint8_t index, bool turnOn);
void noteInteraction();

// ------------------------------
// Setup
// ------------------------------
void setup() {
  Serial.begin(115200);
  initHardware();
  initDisplay();
  loadPreferences();
  loadThresholds();
  initNetwork();
  initOTA();
  initWebServer();

  Serial.println("Sauna Controller started (Threshold-based control)");
}

// ------------------------------
// Loop
// ------------------------------
void loop() {
  unsigned long now = millis();

  // Handle OTA updates - must be first and frequent
  ArduinoOTA.handle();
  if (otaInProgress) {
    return;  // Skip everything else during OTA
  }

  handleEncoder();
  handleButton();
  
  // Handle web requests - single call only (don't block OTA)
  server.handleClient();

  if (now - lastTempRead >= TEMP_READ_INTERVAL) {
    lastTempRead = now;
    readTemperature();
  }

  if (now - lastControl >= CONTROL_INTERVAL) {
    lastControl = now;
    updateControl();
    updateBulbs();
  }

  // Update display - faster refresh in setting mode for flashing
  unsigned long displayInterval = settingMode ? 250 : DISPLAY_INTERVAL;
  if (now - lastDisplay >= displayInterval) {
    lastDisplay = now;
    renderDisplay();
  }

  // Debug: Log server connectivity status periodically
  if (now - lastServerLog >= 30000) {
    lastServerLog = now;
    Serial.printf("Server online (WiFi: %s, IP: %s)\n", 
                  WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "none");
  }

  // Auto-exit setting mode after timeout
  if (settingMode && (now - lastInteraction) > UI_TIMEOUT) {
    settingMode = false;
    saveSetpoint();
    Serial.println("Setting mode timeout - saved setpoint");
  }

  // Auto-return to normal UI
  if (uiMode != UIMode::NORMAL && (now - lastInteraction) > UI_TIMEOUT) {
    uiMode = UIMode::NORMAL;
  }
}

// ------------------------------
// Initialization helpers
// ------------------------------
void initHardware() {
  // Bulb pins
  for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
    bulbs[i] = {BULB_PINS[i], false, 0, 0, 0, 0};
    pinMode(BULB_PINS[i], OUTPUT);
    digitalWrite(BULB_PINS[i], LOW);
  }

  // Encoder
  pinMode(ENCODER_CLK_PIN, INPUT_PULLUP);
  pinMode(ENCODER_DT_PIN, INPUT_PULLUP);
  pinMode(ENCODER_SW_PIN, INPUT_PULLUP);
  lastEncoderCLK = digitalRead(ENCODER_CLK_PIN);
  lastEncoderDT = digitalRead(ENCODER_DT_PIN);

  sensors.begin();
}

void initDisplay() {
  // Initialize I2C
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(10000);  // 10kHz I2C - very slow for noisy power supplies
  delay(200);  // Let I2C stabilize
  
  // Scan for I2C devices
  Serial.println("Scanning I2C...");
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("Found I2C device at 0x%02X\n", addr);
    }
  }
  
  // Try 0x3C first (most common), fallback to 0x3D
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("0x3C failed, trying 0x3D...");
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
      Serial.println("SSD1306 allocation failed on both addresses");
      for (;;) {
        delay(100);
      }
    }
  }
  
  display.setRotation(2);  // Correct rotation (0=normal, 1=90°, 2=180°, 3=270°)
  display.clearDisplay();
  display.display();
  delay(10);  // Small delay after display update
  Serial.println("Display initialized at 10kHz I2C (noise-resistant mode)");
}

void initNetwork() {
  WiFi.disconnect(true);  // Disconnect and turn off to clear any old settings
  delay(1000);
  
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);  // Don't store credentials to avoid conflicts
  
  Serial.printf("WiFi Mode: STA, Hostname: %s\n", MDNS_HOSTNAME);
  
  // Configure static IP if enabled
  if (USE_STATIC_IP) {
    if (!WiFi.config(staticIP, gateway, subnet, dns)) {
      Serial.println("Static IP configuration failed");
    } else {
      Serial.printf("Using static IP: %s\n", staticIP.toString().c_str());
    }
  }
  
  // Scan for available networks
  Serial.println("Scanning for WiFi networks...");
  int n = WiFi.scanNetworks();
  Serial.printf("Found %d networks:\n", n);
  bool targetFound = false;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    bool isTarget = (ssid == WIFI_SSID);
    if (isTarget) targetFound = true;
    Serial.printf("  %d: %s%s (%d dBm) Ch:%d %s\n", 
      i + 1, 
      ssid.c_str(),
      isTarget ? " <-- TARGET" : "",
      WiFi.RSSI(i),
      WiFi.channel(i),
      WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "Open" : "Encrypted");
  }
  WiFi.scanDelete();
  
  if (!targetFound) {
    Serial.printf("WARNING: Target SSID '%s' not found in scan!\n", WIFI_SSID);
  }
  
  Serial.printf("Attempting to connect to WiFi: %s\n", WIFI_SSID);
  Serial.printf("Password length: %d characters\n", strlen(WIFI_PASS));
  
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  unsigned long start = millis();
  int dotCount = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {  // Extended to 30 seconds
    delay(500);
    Serial.print('.');
    dotCount++;
    
    // Print status every 5 seconds
    if (dotCount % 10 == 0) {
      Serial.printf(" [%d sec, Status: %d]\n", (millis() - start) / 1000, WiFi.status());
    }
  }
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("✓ WiFi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("  RSSI: %d dBm\n", WiFi.RSSI());
    
    if (MDNS.begin(MDNS_HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("✓ mDNS started: http://%s.local/\n", MDNS_HOSTNAME);
    } else {
      Serial.println("✗ mDNS start failed");
    }
  } else {
    Serial.printf("✗ WiFi connect failed (Status: %d), continuing offline\n", WiFi.status());
    Serial.println("  Status codes: 0=IDLE, 1=CONNECTING, 2=AUTH_FAIL, 3=NO_SSID_AVAIL, 4=CONNECT_FAIL, 5=GOT_IP, etc");
  }
}

void initOTA() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("OTA disabled (WiFi not connected)");
    return;
  }

  ArduinoOTA.setHostname(MDNS_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("OTA start: " + type);

    for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
      digitalWrite(BULB_PINS[i], LOW);
    }

    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(0, 20);
    display.println("OTA");
    display.println("Updating...");
    display.display();
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA end, rebooting");
    otaInProgress = false;
  });

  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial.printf("OTA error %u: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready");
}

void initWebServer() {
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/set", handleSetpoint);
  server.on("/bulbs", handleBulbs);
  server.on("/stats", handleStats);
  server.on("/manual", handleManualMode);
  server.on("/power", handlePower);
  server.on("/toggle", handleToggle);
  server.on("/thresholds", handleThresholds);
  server.on("/resetthresh", handleResetThresholds);
  
  // Add a 404 handler to debug routing issues
  server.onNotFound([]() {
    Serial.printf("404 Not Found: %s %s\n", server.method() == HTTP_GET ? "GET" : 
                                             server.method() == HTTP_POST ? "POST" : "OTHER",
                  server.uri().c_str());
    server.send(404, "text/plain", "404 - Page not found");
  });
  
  server.begin();
  Serial.println("Web server started on port 80");
}

void loadPreferences() {
  prefs.begin("thermo", false);
  setpointF = prefs.getDouble("setpoint", DEFAULT_SETPOINT_F);
  setpointF = constrain(setpointF, TEMP_MIN_F, TEMP_MAX_F);
  heatEnabled = prefs.getBool("heatOn", true);
  for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "c%u", i);
    bulbs[i].cycles = prefs.getULong(key, 0);
    snprintf(key, sizeof(key), "r%u", i);
    bulbs[i].runtimeMs = prefs.getULong64(key, 0);
  }
}

void saveSetpoint() {
  prefs.putDouble("setpoint", setpointF);
}

void saveHeatEnabled() {
  prefs.putBool("heatOn", heatEnabled);
}

void saveThresholds() {
  prefs.putDouble("deadband", deadbandF);
  Serial.printf("Saved deadband: %.1f\u00b0F\n", deadbandF);
}

void loadThresholds() {
  deadbandF = prefs.getDouble("deadband", DEFAULT_DEADBAND);
  Serial.printf("Loaded deadband: %.1f\u00b0F\n", deadbandF);
}

void saveCycles() {
  for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "c%u", i);
    prefs.putULong(key, bulbs[i].cycles);
    snprintf(key, sizeof(key), "r%u", i);
    prefs.putULong64(key, bulbs[i].runtimeMs);
  }
  cyclesSinceSave = 0;
}

// PID functions removed - not used in threshold-based control

// ------------------------------
// Sensors and control
// ------------------------------
void readTemperature() {
  sensors.requestTemperatures();
  double tempF = sensors.getTempFByIndex(0);
  if (tempF == DEVICE_DISCONNECTED_F) {
    Serial.println("Temp read failed");
    return;
  }
  currentTempF = tempF;
  double error = setpointF - currentTempF;
  Serial.printf("Temp: %.2fF Set: %.2fF Error: %.2f°F\n", currentTempF, setpointF, error);
}

void updateControl() {
  double error = setpointF - currentTempF;
  targetBulbCount = calculateTargetBulbs();
  Serial.printf("Control: Error=%.2f°F, Target=%u bulbs\n", error, targetBulbCount);
}

uint8_t calculateTargetBulbs() {
  double error = setpointF - currentTempF;
  
  // Above setpoint + deadband: turn off all heating
  if (error < -deadbandF) {
    Serial.println("Above setpoint - all bulbs OFF");
    return 0;
  }
  
  // Within deadband: maintain current state
  if (fabs(error) <= deadbandF) {
    Serial.println("Within deadband - maintaining current state");
    return 255; // sentinel meaning hold
  }
  
  // Below setpoint: proportional heating based on temperature deficit
  if (error >= ZONE_HIGH) {
    Serial.printf("%.1f°F below setpoint - 3 bulbs (MAX heat)\n", error);
    return 3;
  } else if (error >= ZONE_MED) {
    Serial.printf("%.1f°F below setpoint - 2 bulbs (MED heat)\n", error);
    return 2;
  } else if (error > deadbandF) {
    Serial.printf("%.1f°F below setpoint - 1 bulb (LOW heat)\n", error);
    return 1;
  }
  
  // Within lower deadband
  return 255; // hold
}

bool hasActiveAdjacent(uint8_t index) {
  for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
    if (i == index) continue;
    if (adjacency[index][i] && bulbs[i].on) {
      return true;
    }
  }
  return false;
}

int selectBulbToTurnOn() {
  long bestScore = -2147483647;
  int bestIdx = -1;
  unsigned long now = millis();
  for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
    BulbState &b = bulbs[i];
    if (b.on) continue;
    if (now - b.lastChange < MIN_ON_OFF_MS) {
      Serial.printf("Bulb %u blocked from ON for %lu ms\n", i + 1, MIN_ON_OFF_MS - (now - b.lastChange));
      continue;
    }
    long score = (long)((now - b.lastChange) / 1000); // prefer idle longer
    if (hasActiveAdjacent(i)) {
      score -= 10000; // strong penalty to avoid neighbors
    }
    score -= (long)b.cycles * 5;     // wear leveling
    score -= (long)(b.runtimeMs / 60000); // runtime penalty per minute
    if (score > bestScore) {
      bestScore = score;
      bestIdx = i;
    }
  }
  return bestIdx;
}

int selectBulbToTurnOff() {
  unsigned long newest = 0;
  int idx = -1;
  unsigned long now = millis();
  for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
    BulbState &b = bulbs[i];
    if (!b.on) continue;
    if (now - b.lastChange < MIN_ON_OFF_MS) {
      Serial.printf("Bulb %u blocked from OFF for %lu ms\n", i + 1, MIN_ON_OFF_MS - (now - b.lastChange));
      continue;
    }
    if (b.lastOnTimestamp >= newest) {
      newest = b.lastOnTimestamp;
      idx = i;
    }
  }
  return idx;
}

void applyBulbState(uint8_t index, bool turnOn) {
  BulbState &b = bulbs[index];
  if (b.on == turnOn) return;
  unsigned long now = millis();
  if (turnOn) {
    digitalWrite(b.pin, HIGH);
    b.on = true;
    b.lastChange = now;
    b.lastOnTimestamp = now;
    b.cycles++;
    cyclesSinceSave++;
    Serial.printf("Bulb %u ON (cycles=%lu)\n", index + 1, (unsigned long)b.cycles);
    if (cyclesSinceSave >= 10) {
      saveCycles();
    }
  } else {
    digitalWrite(b.pin, LOW);
    b.on = false;
    b.lastChange = now;
    if (b.lastOnTimestamp > 0) {
      b.runtimeMs += (now - b.lastOnTimestamp);
    }
    Serial.printf("Bulb %u OFF (runtime=%.1f min)\n", index + 1, b.runtimeMs / 60000.0);
  }
}

void updateBulbs() {
  if (manualMode) {
    return; // manual test mode leaves relays under user control
  }
  if (!heatEnabled) {
    // thermostat off: ensure relays go off
    for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
      if (bulbs[i].on) applyBulbState(i, false);
    }
    return;
  }
  uint8_t target = targetBulbCount;
  if (target == 255) {
    return; // deadband hold
  }
  uint8_t active = 0;
  for (auto &b : bulbs) {
    if (b.on) active++;
  }
  Serial.printf("Target bulbs: %u Active: %u\n", target, active);

  unsigned long guard = 0;
  while (active < target && guard < 10) {
    int idx = selectBulbToTurnOn();
    if (idx < 0) break;
    applyBulbState(idx, true);
    active++;
    guard++;
  }

  guard = 0;
  while (active > target && guard < 10) {
    int idx = selectBulbToTurnOff();
    if (idx < 0) break;
    applyBulbState(idx, false);
    active--;
    guard++;
  }
}

// ------------------------------
// UI handling
// ------------------------------
void noteInteraction() {
  lastInteraction = millis();
}

void handleEncoder() {
  // Only adjust setpoint when in setting mode
  if (!settingMode) {
    return;
  }
  
  unsigned long now = millis();
  
  // Require minimum time between reads to filter noise
  if (now - lastEncoderChange < 100) {
    return;  // Ignore reads that are too close together
  }
  
  int clk = digitalRead(ENCODER_CLK_PIN);
  int dt = digitalRead(ENCODER_DT_PIN);
  
  // Only act on falling edge of CLK
  if (lastEncoderCLK == HIGH && clk == LOW) {
    // Double-check the reading after a small delay
    delayMicroseconds(500);
    int clk2 = digitalRead(ENCODER_CLK_PIN);
    int dt2 = digitalRead(ENCODER_DT_PIN);
    
    // Both readings must match to be valid
    if (clk2 == LOW && dt == dt2) {
      int delta = (dt == HIGH) ? 1 : -1;
      
      setpointF += delta;
      setpointF = constrain(setpointF, TEMP_MIN_F, TEMP_MAX_F);
      noteInteraction();
      Serial.printf("Encoder %s: Set=%.0f°F\n", delta > 0 ? "+" : "-", setpointF);
      
      lastEncoderChange = now;
    }
  }
  
  lastEncoderCLK = clk;
  lastEncoderDT = dt;
}

void handleButton() {
  unsigned long now = millis();
  int currentRead = digitalRead(ENCODER_SW_PIN);
  
  // Detect state change with debounce
  if (currentRead != buttonState) {
    if (now - lastButtonChange >= BUTTON_DEBOUNCE_MS) {
      buttonState = currentRead;
      lastButtonChange = now;
      
      if (currentRead == LOW) {  // Button pressed (LOW with pullup)
        settingMode = !settingMode;  // Toggle setting mode
        noteInteraction();
        
        if (settingMode) {
          Serial.println("✓ Entered SET MODE - rotate to adjust");
          flashState = true;
          lastFlash = now;
        } else {
          Serial.println("✓ Exited SET MODE - saving setpoint");
          Serial.printf("  Setpoint: %.1f°F\n", setpointF);
          saveSetpoint();
        }
      }
    }
  }
}

// ------------------------------
// Display
// ------------------------------

void renderDisplay() {
  unsigned long now = millis();
  
  // Handle flashing in setting mode (300ms on/off)
  if (settingMode && now - lastFlash >= 300) {
    flashState = !flashState;
    lastFlash = now;
  }
  
  display.clearDisplay();
  display.setTextColor(WHITE);
  
  // Current Temperature - Line 1-2
  display.setCursor(0, 0);
  display.setTextSize(2);
  display.print("Temp:");
  display.print(currentTempF, 0);
  display.print("F");
  
  // Target Temperature - Line 3-4 (flashes in setting mode)
  display.setCursor(0, 20);
  display.setTextSize(2);
  
  if (settingMode) {
    // In setting mode: flash the setpoint line
    if (flashState) {
      display.print(">Set:");
      display.print(setpointF, 0);
      display.print("F<");
    } else {
      // Don't print setpoint line during flash-off
    }
    
    // Show hint at bottom
    display.setCursor(0, 48);
    display.setTextSize(1);
    display.print("Rotate to adjust");
  } else {
    // Normal mode
    display.print("Set: ");
    display.print(setpointF, 0);
    display.print("F");
  }
  
  display.display();
  delay(5);  // Brief pause after I2C transfer for stability
}

void renderBulbBar() {
  display.setCursor(0, 54);
  display.print("Bulbs: ");
  for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
    display.print(bulbs[i].on ? 'O' : '-');
  }
}

void renderNormal() {
  display.setTextSize(2);
  display.print("T: ");
  display.print(currentTempF, 1);
  display.print("F\n");
  display.setTextSize(1);
  display.print("Set: ");
  display.print(setpointF, 1);
  display.print("F  Err:");
  double error = setpointF - currentTempF;
  display.print(error, 1);
  display.println("F");

  if (fabs(error) < deadbandF) {
    display.println("At Target");
  } else {
    display.print("Heating: ");
    uint8_t active = 0;
    for (auto &b : bulbs) if (b.on) active++;
    display.print(active);
    display.println(" bulb(s)");
  }
  renderBulbBar();
}

void renderSetpoint() {
  display.setTextSize(2);
  display.print("Set: ");
  display.print(setpointF, 0);
  display.println("F");
  display.setTextSize(1);
  display.println("Rotate to adjust");
  display.println("Range 150-200F");
  renderBulbBar();
}

void renderStats() {
  display.setTextSize(1);
  display.println("Cycles per group:");
  for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
    display.printf("%u:%lu ", i + 1, (unsigned long)bulbs[i].cycles);
  }
  display.println();
  unsigned long uptimeSec = millis() / 1000;
  display.printf("Uptime: %luh%lum\n", uptimeSec / 3600, (uptimeSec / 60) % 60);
  renderBulbBar();
}

// ------------------------------
// Web server handlers
// ------------------------------
String bulbJsonArray() {
  String json = "[";
  for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
    if (i) json += ',';
    json += String("{\"on\":") + (bulbs[i].on ? "true" : "false") +
            ",\"cycles\":" + String(bulbs[i].cycles) +
            ",\"runtime\":" + String((unsigned long)(bulbs[i].runtimeMs / 1000)) +
            ",\"size\":" + String(BULBS_PER_GROUP) + "}";
  }
  json += "]";
  return json;
}

void handleRoot() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<style>
  body { background:#f0f0f0; font-family:'Segoe UI',sans-serif; }
  .card { max-width:420px; margin:20px auto; background:#fff; padding:20px; border-radius:12px; box-shadow:0 10px 30px rgba(0,0,0,0.1); }
  h1 { margin:0 0 10px; font-size:22px; }
  .temp { font-size:48px; font-weight:700; text-align:center; margin:10px 0; }
  .tabs { display:flex; gap:10px; margin:10px 0; }
  .tab { flex:1; padding:10px; border-radius:10px; border:1px solid #ddd; text-align:center; cursor:pointer; background:#fafafa; }
  .tab.active { background:#dfe9ff; border-color:#8fa7ff; font-weight:600; }
  .set-controls { display:flex; gap:10px; justify-content:center; margin:10px 0; }
  .set-row { display:flex; gap:8px; margin:10px 0; }
  .set-row input { flex:2; padding:10px; border-radius:8px; border:1px solid #ccc; font-size:16px; }
  .set-row button { flex:1; padding:10px; border:none; border-radius:8px; background:#2c3e50; color:#fff; font-size:16px; cursor:pointer; }
  .btn { flex:1; padding:12px; border:none; color:#fff; font-size:18px; border-radius:8px; cursor:pointer; transition:transform .1s; }
  .btn:active { transform:scale(0.98); }
  .minus { background:#e74c3c; }
  .plus { background:#1abc9c; }
  .manual { background:#3498db; }
  .toggle { background:#8e44ad; font-size:14px; padding:10px; }
  .badge { background:#eee; padding:4px 8px; border-radius:6px; font-size:12px; }
  .bulbs { display:flex; justify-content:space-between; margin:12px 0; }
  .bulb { width:18%; padding:16px 0; border-radius:10px; background:#e0e0e0; text-align:center; position:relative; overflow:hidden; }
  .bulb.on { background:linear-gradient(135deg,#ffb347,#ffcc33); box-shadow:0 0 12px rgba(255,165,0,0.8); }
  .bulb-controls { display:flex; gap:10px; justify-content:space-between; margin:8px 0; }
  .stats { background:#fafafa; padding:12px; border-radius:8px; }
  .meta { display:flex; justify-content:space-between; }
  .hide { display:none; }
  .switch-row { display:flex; align-items:center; gap:10px; margin:10px 0; }
  .switch-row label { font-weight:600; }
  .switch-row button { padding:10px 14px; border:none; border-radius:8px; background:#2c3e50; color:#fff; cursor:pointer; }
</style>
</head>
<body>
  <div class="card">
    <div class="meta"><div id="deadband" class="badge"></div><div id="uptime" class="badge"></div></div>
    <h1>Infrared Thermostat</h1>
    <div id="temp" class="temp">--.-°F</div>
    <div style="text-align:center">Setpoint: <span id="setpoint">--.-</span>°F</div>
    <div class="tabs">
      <div id="tabThermo" class="tab active" onclick="showTab('thermo')">Thermostat</div>
      <div id="tabManual" class="tab" onclick="showTab('manual')">Manual</div>
      <div id="tabSettings" class="tab" onclick="showTab('settings')">Settings</div>
    </div>
    <div class="set-controls">
      <button class="btn minus" onclick="adjust(-1)">-1°F</button>
      <button class="btn plus" onclick="adjust(1)">+1°F</button>
    </div>
    <div class="set-row">
      <input id="setInput" type="number" step="1" min="150" max="200" placeholder="Enter setpoint (°F)" />
      <button onclick="applySet()">Set</button>
    </div>
    <div id="panelThermo">
      <div class="switch-row">
        <label>Thermostat Power</label>
        <button id="powerBtn" onclick="togglePower()">On</button>
      </div>
      <div class="bulbs" id="bulbs"></div>
      <div class="stats">
        <div>Target Bulbs: <span id="target">--</span></div>
        <div>In Deadband: <span id="dead">--</span></div>
        <div>Error: <span id="error">--</span>°F</div>
        <div>Cycles: <span id="cycles">--</span></div>
        <div>Manual Mode: <span id="manualState">--</span></div>
      </div>
    </div>
    <div id="panelManual" class="hide">
      <div class="bulb-controls" id="bulbControls"></div>
      <div class="bulbs" id="manualBulbs"></div>
    </div>
    <div id="panelSettings" class="hide">
      <h2 style="font-size:18px;margin:10px 0;">Control Settings</h2>
      <div class="stats" style="padding:16px;">
        <div style="margin:12px 0;">
          <label style="display:block;margin-bottom:4px;font-weight:600;">Deadband (±°F)</label>
          <input type="number" id="inputDeadband" step="0.5" min="0.5" max="10" style="width:100%;padding:8px;border:1px solid #ccc;border-radius:6px;" />
          <small style="color:#666;">Temperature tolerance around setpoint (maintains current state within this range)</small>
        </div>
        <button style="width:100%;padding:12px;background:#27ae60;color:#fff;border:none;border-radius:8px;font-size:16px;cursor:pointer;margin-top:10px;" onclick="saveThresholds()">Save Settings</button>
        <button style="width:100%;padding:10px;background:#e74c3c;color:#fff;border:none;border-radius:8px;font-size:14px;cursor:pointer;margin-top:8px;" onclick="resetThresholds()">Reset to Default</button>
        <div style="margin-top:20px;padding:12px;background:#f9f9f9;border-radius:6px;font-size:13px;">
          <strong>How it works:</strong><br/>
          • Within deadband: maintains current heating<br/>
          • 2-5°F below: 1 bulb<br/>
          • 5-10°F below: 2 bulbs<br/>
          • 10°F+ below: 3 bulbs (max heat)<br/>
          • Above setpoint: all off
        </div>
      </div>
    </div>
  </div>
<script>
let manual = false;
let heatEnabled = true;
let currentTab = 'thermo';
let editingSet = false;
let editingThresh = false;
async function fetchStatus(){
  const r = await fetch('/status');
  const s = await r.json();
  document.getElementById('temp').innerText = s.temp.toFixed(1)+'°F';
  document.getElementById('setpoint').innerText = s.setpoint.toFixed(1);
  document.getElementById('target').innerText = s.targetBulbs;
  document.getElementById('error').innerText = (s.setpoint - s.temp).toFixed(1);
  document.getElementById('dead').innerText = s.inDeadband ? 'Yes' : 'No';
  document.getElementById('deadband').innerText = 'Deadband ±'+s.deadband+'°F';
  document.getElementById('uptime').innerText = 'Uptime '+s.uptime;
  if (!editingSet) {
    document.getElementById('setInput').value = s.setpoint.toFixed(0);
  }
  if (!editingThresh) {
    document.getElementById('inputDeadband').value = s.deadband.toFixed(1);
  }
  manual = !!s.manual;
  heatEnabled = !!s.heatEnabled;
  document.getElementById('manualState').innerText = manual ? 'Enabled' : 'Disabled';
  document.getElementById('powerBtn').innerText = heatEnabled ? 'On' : 'Off';
  const bulbs = document.getElementById('bulbs');
  bulbs.innerHTML='';
  const controls = document.getElementById('bulbControls');
  controls.innerHTML='';
  const manualBulbs = document.getElementById('manualBulbs');
  manualBulbs.innerHTML='';
  s.bulbs.forEach((b,i)=>{
    const div=document.createElement('div');
    div.className='bulb'+(b.on?' on':'');
    div.innerText='G'+(i+1)+' ('+b.size+'x)';
    bulbs.appendChild(div);

    const btn=document.createElement('button');
    btn.className='btn toggle';
    btn.innerText='Toggle G'+(i+1);
    btn.onclick=()=>toggleGroup(i);
    controls.appendChild(btn);

    const mdiv=document.createElement('div');
    mdiv.className='bulb'+(b.on?' on':'');
    mdiv.innerText='G'+(i+1)+' ('+b.size+'x)';
    manualBulbs.appendChild(mdiv);
  });
  const totalCycles = s.cycles.reduce((a,b)=>a+b,0);
  const avg = s.cycles.length ? totalCycles / s.cycles.length : 0;
  document.getElementById('cycles').innerText = totalCycles + ' (avg ' + avg.toFixed(0) + ')';
}
async function adjust(delta){ await fetch('/set?temp='+delta); fetchStatus(); }
async function applySet(){
  const v = parseFloat(document.getElementById('setInput').value);
  if (isNaN(v)) return;
  await fetch('/set?value='+v);
  fetchStatus();
}
async function toggleManual(){
  const next = manual ? 'off' : 'on';
  await fetch('/manual?state='+next);
  fetchStatus();
}
async function togglePower(){
  const next = heatEnabled ? 'off' : 'on';
  await fetch('/power?state='+next);
  fetchStatus();
}
async function toggleGroup(idx){
  await fetch('/toggle?group='+idx+'&state=toggle');
  fetchStatus();
}
async function saveThresholds(){
  const db = parseFloat(document.getElementById('inputDeadband').value);
  if (isNaN(db)) {
    alert('Please enter a valid number');
    return;
  }
  await fetch('/thresholds?deadband='+db);
  fetchStatus();
  alert('Settings saved!');
}
async function resetThresholds(){
  if (confirm('Reset deadband to factory default (2°F)?')) {
    await fetch('/resetthresh');
    fetchStatus();
  }
}
function showTab(name){
  currentTab = name;
  document.getElementById('panelThermo').classList.toggle('hide', name !== 'thermo');
  document.getElementById('panelManual').classList.toggle('hide', name !== 'manual');
  document.getElementById('panelSettings').classList.toggle('hide', name !== 'settings');
  document.getElementById('tabThermo').classList.toggle('active', name === 'thermo');
  document.getElementById('tabManual').classList.toggle('active', name === 'manual');
  document.getElementById('tabSettings').classList.toggle('active', name === 'settings');
}
const setInput = document.getElementById('setInput');
setInput.addEventListener('focus', ()=>{ editingSet = true; });
setInput.addEventListener('blur', ()=>{ editingSet = false; });
const dbInput = document.getElementById('inputDeadband');
dbInput.addEventListener('focus', ()=>{ editingThresh = true; });
dbInput.addEventListener('blur', ()=>{ editingThresh = false; });
fetchStatus();
setInterval(fetchStatus,2000);
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", page);
}

void handleStatus() {
  bool inDead = fabs(setpointF - currentTempF) < deadbandF;
  String json = "{";
  json += "\"temp\":" + String(currentTempF, 2) + ",";
  json += "\"setpoint\":" + String(setpointF, 2) + ",";
  json += "\"targetBulbs\":" + String(targetBulbCount) + ",";
  json += "\"deadband\":" + String(deadbandF, 1) + ",";
  json += "\"inDeadband\":" + String(inDead ? "true" : "false") + ",";
  json += "\"manual\":" + String(manualMode ? "true" : "false") + ",";
  json += "\"heatEnabled\":" + String(heatEnabled ? "true" : "false") + ",";
  json += "\"bulbs\":" + bulbJsonArray() + ",";
  json += "\"cycles\":";
  json += "[";
  for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
    if (i) json += ',';
    json += String(bulbs[i].cycles);
  }
  json += "],";
  unsigned long up = millis()/1000;
  char buf[32];
  snprintf(buf,sizeof(buf),"%luh %lum", up/3600, (up/60)%60);
  json += "\"uptime\":\"" + String(buf) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleSetpoint() {
  double newSet = setpointF;
  if (server.hasArg("value")) {
    newSet = server.arg("value").toDouble();
  } else if (server.hasArg("temp")) {
    double delta = server.arg("temp").toDouble();
    newSet = setpointF + delta;
  } else {
    server.send(400, "text/plain", "temp or value query required");
    return;
  }
  setpointF = constrain(newSet, TEMP_MIN_F, TEMP_MAX_F);
  setpointF = constrain(setpointF, TEMP_MIN_F, TEMP_MAX_F);
  saveSetpoint();
  noteInteraction();
  server.send(200, "application/json", String("{\"setpoint\":") + String(setpointF, 1) + "}");
}

void handleBulbs() {
  server.send(200, "application/json", bulbJsonArray());
}

void handleStats() {
  String json = "{";
  json += "\"cycles\":";
  json += "[";
  for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
    if (i) json += ',';
    json += String(bulbs[i].cycles);
  }
  json += "],";
  json += "\"runtime\":";
  json += "[";
  for (uint8_t i = 0; i < BULB_GROUP_COUNT; i++) {
    if (i) json += ',';
    json += String((unsigned long)(bulbs[i].runtimeMs / 1000));
  }
  json += "]";
  json += "}";
  server.send(200, "application/json", json);
}

void handleManualMode() {
  if (!server.hasArg("state")) {
    server.send(400, "text/plain", "state query required (on/off)");
    return;
  }
  String state = server.arg("state");
  manualMode = (state == "on" || state == "1" || state == "true");
  server.send(200, "application/json",
              String("{\"manual\":") + (manualMode ? "true" : "false") + "}");
}

void handlePower() {
  if (!server.hasArg("state")) {
    server.send(400, "text/plain", "state query required (on/off)");
    return;
  }
  String state = server.arg("state");
  heatEnabled = (state == "on" || state == "1" || state == "true");
  saveHeatEnabled();
  // Apply immediately instead of waiting for the 45s bulb interval
  updateBulbs();
  server.send(200, "application/json",
              String("{\"heatEnabled\":") + (heatEnabled ? "true" : "false") + "}");
}

void handleToggle() {
  if (!server.hasArg("group")) {
    server.send(400, "text/plain", "group query required (0-index)");
    return;
  }
  int idx = server.arg("group").toInt();
  if (idx < 0 || idx >= BULB_GROUP_COUNT) {
    server.send(400, "text/plain", "group out of range");
    return;
  }

  bool turnOn;
  if (server.hasArg("state")) {
    String state = server.arg("state");
    if (state == "toggle") {
      turnOn = !bulbs[idx].on;
    } else {
      turnOn = (state == "on" || state == "1" || state == "true");
    }
  } else {
    turnOn = !bulbs[idx].on;
  }

  manualMode = true; // keep automatic control from fighting manual testing
  applyBulbState(idx, turnOn);
  noteInteraction();

  server.send(200, "application/json",
              String("{\"group\":") + idx + ",\"on\":" + (bulbs[idx].on ? "true" : "false") +
                  ",\"manual\":" + (manualMode ? "true" : "false") + "}");
}

void handleThresholds() {
  if (server.hasArg("deadband")) {
    deadbandF = constrain(server.arg("deadband").toDouble(), 0.5, 10.0);
  }
  
  saveThresholds();
  
  String json = "{";
  json += "\"deadband\":" + String(deadbandF, 1);
  json += "}";
  server.send(200, "application/json", json);
}

void handleResetThresholds() {
  deadbandF = DEFAULT_DEADBAND;
  saveThresholds();
  
  String json = "{";
  json += "\"deadband\":" + String(deadbandF, 1);
  json += "}";
  server.send(200, "application/json", json);
}

