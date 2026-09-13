/*
=========================================================
        SMART WASTE SEGREGATION BIN
        ESP32 RECEIVER
=========================================================

mDNS:
    smart-waste-receiver.local

Setup AP:
    SMART-WASTE-SETUP
    Password: waste123

Dashboard:
    http://<RECEIVER-IP>

AI:
    /ai?label=plastic_bottle&confidence=98.7
=========================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ESPmDNS.h>

// =====================================================
// WIFI
// =====================================================

Preferences preferences;

String savedSSID = "";
String savedPassword = "";

bool wifiConnected = false;

const char* AP_SSID = "SMART-WASTE-SETUP";
const char* AP_PASSWORD = "waste123";

const char* MDNS_NAME = "smart-waste-receiver";

// =====================================================
// SERVER
// =====================================================

WebServer server(80);

// =====================================================
// PINS
// =====================================================

// Safety
#define FLAME_PIN 35
#define METAL_SENSOR 34
#define MQ2_PIN 32

// BIO
#define BIO_TRIG 27
#define BIO_ECHO 33

// NONBIO
#define NONBIO_TRIG 16
#define NONBIO_ECHO 17

// METAL
#define METAL_TRIG 25
#define METAL_ECHO 26

// LEDs / buzzer
#define GREEN_LED 2
#define RED_LED 5
#define BUZZER 4

// Servos
#define SELECTOR_SERVO_PIN 18
#define LID_SERVO_PIN 19

// Wi-Fi reset
#define WIFI_RESET_BUTTON 13

// =====================================================
// SERVOS
// =====================================================

Servo selectorServo;
Servo lidServo;

// =====================================================
// SELECTOR CALIBRATION
// =====================================================

#define SELECTOR_NONBIO_US 500
#define SELECTOR_BIO_US 1133
#define SELECTOR_METAL_US 1872

// =====================================================
// LID CALIBRATION
// YOUR LID IS REVERSED
// =====================================================

#define LID_CLOSED_US 2300
#define LID_OPEN_US 1100

// =====================================================
// TIMINGS
// =====================================================

#define SELECTOR_SETTLE_TIME 3000
#define LID_OPEN_TIME 700
#define SORT_RECOVERY_TIME 2000

// =====================================================
// BIN FULL
// =====================================================

#define BIO_FULL_DISTANCE 2.5
#define NONBIO_FULL_DISTANCE 6.0
#define METAL_FULL_DISTANCE 6.0

// IMPORTANT:
// These are monitoring thresholds only.
// They DO NOT block sorting.
// The system continues to catch waste even when a bin
// reaches the threshold; the dashboard/ThingSpeak shows
// the actual distance so the bin level can be monitored.

// =====================================================
// MQ2
// =====================================================

int MQ2_THRESHOLD = 2700;

// =====================================================
// OLED
// =====================================================

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(
  U8G2_R0,
  U8X8_PIN_NONE
);

// =====================================================
// COUNTERS
// =====================================================

unsigned long bioCount = 0;
unsigned long nonbioCount = 0;
unsigned long metalCount = 0;

// =====================================================
// AI
// =====================================================

String lastLabel = "NO_OBJECT";
float lastConfidence = 0;

String currentCategory = "READY";

// =====================================================
// SENSORS
// =====================================================

float bioDistance = -1;
float nonbioDistance = -1;
float metalDistance = -1;

bool flameDetected = false;
bool gasDetected = false;
bool safetyAlarm = false;

// Inductive metal sensor: ACTIVE LOW.
// GPIO34 is input-only and has no internal pull-up.
bool metalDetectedStable = false;

// Metal object latch.
// A metal object is sorted only once while it is held near
// the inductive sensor. The system re-arms after metal is removed.
bool metalSortArmed = true;
unsigned long metalClearSince = 0;

bool readMetalSensorStable() {

  int lowCount = 0;

  for (int i = 0; i < 15; i++) {

    if (digitalRead(METAL_SENSOR) == LOW) {
      lowCount++;
    }

    delayMicroseconds(500);
  }

  return lowCount >= 8;
}

// =====================================================
// SORTING
// =====================================================

bool sorting = false;

// =====================================================
// THINGSPEAK
// =====================================================

// ThingSpeak Write API Key
const char* THINGSPEAK_API_KEY =
  "YOUR_WRITE_API_KEY";

// ThingSpeak fields:
// F1 = BIO ultrasonic distance (cm)
// F2 = NONBIO ultrasonic distance (cm)
// F3 = METAL BIN ultrasonic distance (cm)
// F4 = BIO count
// F5 = NONBIO count
// F6 = METAL count
// F7 = AI confidence (%)
// F8 = category number: 0 READY, 1 BIO, 2 NONBIO, 3 METAL

unsigned long lastThingSpeak = 0;

#define THINGSPEAK_INTERVAL 20000

// =====================================================
// WIFI RESET
// =====================================================

unsigned long wifiButtonStart = 0;

bool wifiButtonActive = false;

// =====================================================
// OLED READY
// =====================================================

void showReadyOLED() {

  oled.clearBuffer();

  oled.setFont(
    u8g2_font_6x10_tf
  );

  oled.setCursor(0, 10);
  oled.print("SMART WASTE BIN");

  oled.setCursor(0, 25);
  oled.print("STATUS: READY");

  oled.setCursor(0, 40);
  oled.print("BIO:");
  oled.print(bioCount);

  oled.setCursor(65, 40);
  oled.print("NB:");
  oled.print(nonbioCount);

  oled.setCursor(0, 55);
  oled.print("METAL:");
  oled.print(metalCount);

  oled.sendBuffer();
}

// =====================================================
// OLED SORTING
// =====================================================

void showSortingOLED(
  String category
) {

  oled.clearBuffer();

  oled.setFont(
    u8g2_font_6x10_tf
  );

  oled.setCursor(0, 12);
  oled.print("SORTING...");

  oled.setCursor(0, 28);
  oled.print("CATEGORY:");

  oled.setCursor(0, 44);
  oled.print(category);

  oled.setCursor(0, 60);
  oled.print("PLEASE WAIT");

  oled.sendBuffer();
}

// =====================================================
// OLED SAFETY
// =====================================================

void showSafetyOLED() {

  oled.clearBuffer();

  oled.setFont(
    u8g2_font_6x10_tf
  );

  oled.setCursor(0, 12);
  oled.print("!!! WARNING !!!");

  if (flameDetected) {

    oled.setCursor(0, 30);
    oled.print("FLAME DETECTED");

  } else if (gasDetected) {

    oled.setCursor(0, 30);
    oled.print("GAS/SMOKE HIGH");
  }

  oled.setCursor(0, 48);
  oled.print("SORTING BLOCKED");

  oled.sendBuffer();
}

// =====================================================
// WIFI AP
// =====================================================

void startAccessPoint() {

  wifiConnected = false;

  WiFi.disconnect(true);

  delay(500);

  WiFi.mode(WIFI_AP);

  WiFi.softAP(
    AP_SSID,
    AP_PASSWORD
  );

  Serial.println();
  Serial.println(
    "=========================================="
  );

  Serial.println(
    " SMART WASTE RECEIVER SETUP"
  );

  Serial.print(
    "AP SSID     : "
  );

  Serial.println(AP_SSID);

  Serial.print(
    "AP PASSWORD : "
  );

  Serial.println(AP_PASSWORD);

  Serial.print(
    "AP IP       : "
  );

  Serial.println(
    WiFi.softAPIP()
  );

  Serial.println(
    "Open: http://192.168.4.1"
  );

  Serial.println(
    "=========================================="
  );
}

// =====================================================
// CONNECT WIFI
// =====================================================

void connectToSavedWiFi() {

  preferences.begin(
    "wifi",
    true
  );

  savedSSID =
    preferences.getString(
      "ssid",
      ""
    );

  savedPassword =
    preferences.getString(
      "password",
      ""
    );

  preferences.end();

  if (savedSSID.length() == 0) {

    Serial.println(
      "[WIFI] No saved credentials."
    );

    startAccessPoint();

    return;
  }

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    savedSSID.c_str(),
    savedPassword.c_str()
  );

  Serial.print(
    "[WIFI] Connecting"
  );

  unsigned long start =
    millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 15000
  ) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    wifiConnected = true;

    Serial.println(
      "[WIFI] CONNECTED"
    );

    Serial.print(
      "[WIFI] IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.print(
      "[WIFI] RSSI: "
    );

    Serial.println(
      WiFi.RSSI()
    );

    if (
      MDNS.begin(MDNS_NAME)
    ) {

      MDNS.addService(
        "http",
        "tcp",
        80
      );

      Serial.println(
        "[mDNS] smart-waste-receiver.local"
      );
    }

  } else {

    Serial.println(
      "[WIFI] Connection failed."
    );

    startAccessPoint();
  }
}

// =====================================================
// ULTRASONIC
// =====================================================

float readUltrasonic(
  int trigPin,
  int echoPin
) {

  digitalWrite(trigPin, LOW);
  delayMicroseconds(4);

  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration =
    pulseIn(
      echoPin,
      HIGH,
      30000
    );

  if (duration == 0)
    return -1;

  float distance =
    duration * 0.0343 / 2.0;

  if (distance < 2.0 || distance > 400.0)
    return -1;

  return distance;
}

// =====================================================
// UPDATE BIN LEVELS
// =====================================================

void updateBinLevels() {

  bioDistance =
    readUltrasonic(
      BIO_TRIG,
      BIO_ECHO
    );

  delay(20);

  nonbioDistance =
    readUltrasonic(
      NONBIO_TRIG,
      NONBIO_ECHO
    );

  delay(20);

  metalDistance =
    readUltrasonic(
      METAL_TRIG,
      METAL_ECHO
    );
}

// =====================================================
// SAFETY
// =====================================================

void updateSafety() {

  flameDetected =
    digitalRead(FLAME_PIN) == LOW;

  int mq2 =
    analogRead(MQ2_PIN);

  gasDetected =
    mq2 >= MQ2_THRESHOLD;

  safetyAlarm =
    flameDetected ||
    gasDetected;

  if (safetyAlarm) {

    digitalWrite(
      RED_LED,
      HIGH
    );

    digitalWrite(
      GREEN_LED,
      LOW
    );

    digitalWrite(
      BUZZER,
      HIGH
    );

    showSafetyOLED();

  } else {

    digitalWrite(
      RED_LED,
      LOW
    );

    digitalWrite(
      BUZZER,
      LOW
    );

    if (!sorting) {

      digitalWrite(
        GREEN_LED,
        HIGH
      );
    }
  }
}

// =====================================================
// SUCCESS BEEP
// =====================================================

void beepSuccess() {

  if (safetyAlarm)
    return;

  digitalWrite(
    BUZZER,
    HIGH
  );

  delay(120);

  digitalWrite(
    BUZZER,
    LOW
  );
}

// =====================================================
// SORT
// =====================================================

void sortWaste(
  String category,
  float confidence
) {

  if (sorting)
    return;

  updateSafety();

  if (safetyAlarm)
    return;

  sorting = true;

  currentCategory =
    category;

  digitalWrite(
    GREEN_LED,
    LOW
  );

  showSortingOLED(
    category
  );

  Serial.println();
  Serial.println(
    "========== SORTING =========="
  );

  Serial.print(
    "CATEGORY: "
  );

  Serial.println(category);

  Serial.print(
    "CONFIDENCE: "
  );

  Serial.println(confidence);

  // -----------------------------------------------
  // SELECTOR
  // -----------------------------------------------

  if (category == "BIO") {

    selectorServo.writeMicroseconds(
      SELECTOR_BIO_US
    );

  } else if (category == "NONBIO") {

    selectorServo.writeMicroseconds(
      SELECTOR_NONBIO_US
    );

  } else if (category == "METAL") {

    selectorServo.writeMicroseconds(
      SELECTOR_METAL_US
    );
  }

  Serial.println(
    "[SERVO] Waiting 3 seconds"
  );

  delay(
    SELECTOR_SETTLE_TIME
  );

  // -----------------------------------------------
  // SAFETY AGAIN
  // -----------------------------------------------

  updateSafety();

  if (safetyAlarm) {

    lidServo.writeMicroseconds(
      LID_CLOSED_US
    );

    sorting = false;

    currentCategory =
      "SAFETY";

    return;
  }

  // -----------------------------------------------
  // OPEN
  // -----------------------------------------------

  lidServo.writeMicroseconds(
    LID_OPEN_US
  );

  delay(
    LID_OPEN_TIME
  );

  // -----------------------------------------------
  // CLOSE
  // -----------------------------------------------

  lidServo.writeMicroseconds(
    LID_CLOSED_US
  );

  // -----------------------------------------------
  // COUNT
  // -----------------------------------------------

  if (category == "BIO") {

    bioCount++;

    preferences.begin(
      "counts",
      false
    );

    preferences.putULong(
      "bio",
      bioCount
    );

    preferences.end();

  } else if (category == "NONBIO") {

    nonbioCount++;

    preferences.begin(
      "counts",
      false
    );

    preferences.putULong(
      "nonbio",
      nonbioCount
    );

    preferences.end();

  } else if (category == "METAL") {

    metalCount++;

    preferences.begin(
      "counts",
      false
    );

    preferences.putULong(
      "metal",
      metalCount
    );

    preferences.end();
  }

  beepSuccess();

  delay(
    SORT_RECOVERY_TIME
  );

  currentCategory =
    "READY";

  sorting = false;

  digitalWrite(
    GREEN_LED,
    HIGH
  );

  showReadyOLED();

  Serial.println(
    "[SYSTEM] READY"
  );
}

// =====================================================
// AI API
// =====================================================

void handleAI() {

  if (!server.hasArg("label")) {

    server.send(
      400,
      "application/json",
      "{\"ok\":false,\"error\":\"label missing\"}"
    );

    return;
  }

  if (sorting) {

    server.send(
      200,
      "application/json",
      "{\"ok\":false,\"message\":\"sorting busy\"}"
    );

    return;
  }

  String label = server.arg("label");
  label.toLowerCase();

  float confidence = 0;

  if (server.hasArg("confidence")) {
    confidence = server.arg("confidence").toFloat();
  }

  lastLabel = label;
  lastConfidence = confidence;

  updateSafety();

  if (safetyAlarm) {

    server.send(
      200,
      "application/json",
      "{\"ok\":false,\"message\":\"SAFETY ALARM\"}"
    );

    return;
  }

  // ---------------------------------------------------
  // READ METAL SENSOR FIRST
  // This is an independent safety/metal override.
  // Even when Python says NO_OBJECT, a detected metal
  // object can be routed to the METAL bin.
  // ---------------------------------------------------

  bool metalDetected = readMetalSensorStable();
  metalDetectedStable = metalDetected;

  if (metalDetected && metalSortArmed) {

    Serial.println(
      "[METAL] PROXIMITY DETECTED DURING AI/NO_OBJECT"
    );

    metalSortArmed = false;

    currentCategory = "METAL";

    server.send(
      200,
      "application/json",
      "{\"ok\":true,\"message\":\"METAL DETECTED - SORTING STARTED\"}"
    );

    sortWaste(
      "METAL",
      confidence
    );

    return;
  }

  // Re-arm only after sensor has stayed clear.
  if (!metalDetected) {

    if (metalClearSince == 0) {
      metalClearSince = millis();
    }

    if (millis() - metalClearSince >= 700) {
      metalSortArmed = true;
    }
  } else {
    metalClearSince = 0;
  }

  // ---------------------------------------------------
  // NO OBJECT
  // ---------------------------------------------------

  if (
    label == "no_object" ||
    label == "no object" ||
    label == "none"
  ) {

    server.send(
      200,
      "application/json",
      "{\"ok\":true,\"message\":\"NO OBJECT\"}"
    );

    return;
  }

  // ---------------------------------------------------
  // AI CONFIDENCE
  // ---------------------------------------------------

  if (confidence < 60.0) {

    server.send(
      200,
      "application/json",
      "{\"ok\":false,\"message\":\"LOW CONFIDENCE\"}"
    );

    return;
  }

  // ---------------------------------------------------
  // CORRECT WASTE CLASSIFICATION
  //
  // BIO:
  //   tomato, brinjal, paper
  //
  // NONBIO:
  //   cup, maaza, plastic_bottle, plastic_cover
  //
  // METAL:
  //   battery, metal_can
  //
  // NOTE:
  // inductive sensor always has priority.
  // ---------------------------------------------------

  String category = "";

  // BIO = vegetables + paper only
  if (
    label == "tomato" ||
    label == "brinjal" ||
    label == "paper"
  ) {

    category = "BIO";
  }

  // NONBIO = paper cup + maaza + plastics
  else if (
    label == "cup" ||
    label == "maaza" ||
    label == "plastic_bottle" ||
    label == "plastic_cover"
  ) {

    category = "NONBIO";
  }

  // METAL = battery + metal can
  else if (
    label == "battery" ||
    label == "metal_can"
  ) {

    category = "METAL";
  }

  else {

    server.send(
      200,
      "application/json",
      "{\"ok\":false,\"message\":\"UNKNOWN LABEL\"}"
    );

    return;
  }

  // ---------------------------------------------------
  // BIN LEVEL
  //
  // Ultrasonic is used for MONITORING only.
  // It does NOT block the waste from being sorted.
  // This is important because the small BIO bin can
  // still physically receive waste.
  // ---------------------------------------------------

  updateBinLevels();

  Serial.print("[BIN LEVEL] BIO=");
  Serial.print(bioDistance, 1);
  Serial.print("cm NONBIO=");
  Serial.print(nonbioDistance, 1);
  Serial.print("cm METAL=");
  Serial.print(metalDistance, 1);
  Serial.println("cm");

  // ---------------------------------------------------
  // ACCEPT SORT
  // ---------------------------------------------------

  server.send(
    200,
    "application/json",
    "{\"ok\":true,\"message\":\"SORTING STARTED\"}"
  );

  if (category == "METAL") {
    // AI battery/metal_can also arms the metal latch off
    // until the physical metal object is removed.
    metalSortArmed = false;
    metalClearSince = 0;
  }

  sortWaste(
    category,
    confidence
  );
}

// =====================================================
// STATUS API
// =====================================================

void handleStatus() {

  bool metal =
    readMetalSensorStable();

  metalDetectedStable = metal;

  String json = "{";

  json += "\"device\":\"ESP32-RECEIVER\"";

  json += ",\"wifi\":";
  json += wifiConnected ? "true" : "false";

  json += ",\"ip\":\"";

  if (wifiConnected)
    json += WiFi.localIP().toString();
  else
    json += WiFi.softAPIP().toString();

  json += "\"";

  json += ",\"hostname\":\"smart-waste-receiver.local\"";

  json += ",\"rssi\":";

  if (wifiConnected)
    json += String(WiFi.RSSI());
  else
    json += "0";

  json += ",\"flame\":";
  json += flameDetected ? "true" : "false";

  json += ",\"gas\":";
  json += gasDetected ? "true" : "false";

  json += ",\"metal\":";
  json += metal ? "true" : "false";

  json += ",\"bio_distance\":";
  json += String(bioDistance, 1);

  json += ",\"nonbio_distance\":";
  json += String(nonbioDistance, 1);

  json += ",\"metal_distance\":";
  json += String(metalDistance, 1);

  json += ",\"bio_count\":";
  json += String(bioCount);

  json += ",\"nonbio_count\":";
  json += String(nonbioCount);

  json += ",\"metal_count\":";
  json += String(metalCount);

  json += ",\"label\":\"";
  json += lastLabel;
  json += "\"";

  json += ",\"confidence\":";
  json += String(lastConfidence, 2);

  json += ",\"category\":\"";
  json += currentCategory;
  json += "\"";

  json += ",\"sorting\":";
  json += sorting ? "true" : "false";

  json += "}";

  server.send(
    200,
    "application/json",
    json
  );
}

// =====================================================
// ROOT
// =====================================================

void handleRoot() {

  String html;

  html += "<!DOCTYPE html>";
  html += "<html>";
  html += "<head>";

  html +=
    "<meta name='viewport' content='width=device-width,initial-scale=1'>";


  html += "<title>Smart Waste Receiver</title>";

  html += "<style>";

  html +=
    "body{font-family:Arial;background:#111;color:white;padding:20px;}";

  html +=
    ".box{background:#222;padding:20px;margin:10px 0;border-radius:12px;}";

  html +=
    ".green{color:#00ff66;}";

  html +=
    ".red{color:#ff4444;}";

  html +=
    ".ip{font-size:30px;font-weight:bold;}";

  html +=
    "input{width:100%;padding:12px;margin:7px 0;box-sizing:border-box;}";

  html +=
    "button{padding:12px 20px;border:0;border-radius:8px;}";

  html += "</style>";

  // IMPORTANT:
  // Do NOT auto-refresh the whole page.
  // A full page refresh steals focus from the SSID/password
  // input boxes and makes typing appear to buffer/freeze.
  html += R"rawliteral(
<script>
async function updateLiveStatus() {
  try {
    const r = await fetch('/status', {cache:'no-store'});
    const d = await r.json();

    const ip = document.getElementById('liveIP');
    const metal = document.getElementById('liveMetal');

    if (ip) {
      ip.textContent = d.ip || '--';
    }

    if (metal) {
      metal.textContent = d.metal ? 'METAL DETECTED' : 'NO METAL';
    }
  } catch(e) {
    // Keep the page and input fields usable if status is unavailable.
  }
}

setInterval(updateLiveStatus, 2000);
</script>
)rawliteral";

  html += "</head><body>";

  html +=
    "<h1>SMART WASTE RECEIVER</h1>";

  html += "<div class='box'>";

  if (wifiConnected) {

    html +=
      "<h2 class='green'>● CONNECTED</h2>";

    html +=
      "<p>Wi-Fi: <b>" +
      savedSSID +
      "</b></p>";

    html +=
      "<p>RECEIVER IP</p>";

    html +=
      "<div id='liveIP' class='ip'>" +
      WiFi.localIP().toString() +
      "</div>";

    html +=
      "<p>mDNS: smart-waste-receiver.local</p>";

  } else {

    html +=
      "<h2>SETUP MODE</h2>";

    html +=
      "<p>Connect to SMART-WASTE-SETUP</p>";

    html +=
      "<p>Password: waste123</p>";

    html +=
      "<p>Open 192.168.4.1</p>";
  }

  html += "</div>";

  html += "<div class='box'>";

  html += "<h2>Bins</h2>";

  html +=
    "<p>BIO: " +
    String(bioDistance, 1) +
    " cm</p>";

  html +=
    "<p>NONBIO: " +
    String(nonbioDistance, 1) +
    " cm</p>";

  html +=
    "<p>METAL: " +
    String(metalDistance, 1) +
    " cm</p>";

  html += "</div>";

  html += "<div class='box'>";

  html += "<h2>Counts</h2>";

  html +=
    "<p>BIO: " +
    String(bioCount) +
    "</p>";

  html +=
    "<p>NONBIO: " +
    String(nonbioCount) +
    "</p>";

  html +=
    "<p>METAL: " +
    String(metalCount) +
    "</p>";

  html += "</div>";

  html += "<div class='box'>";

  html += "<h2>Safety</h2>";

  html +=
    "<p>Flame: " +
    String(flameDetected ? "DETECTED" : "SAFE") +
    "</p>";

  html +=
    "<p>MQ-2: " +
    String(gasDetected ? "HIGH" : "SAFE") +
    "</p>";

  html += "</div>";

  html += "<div class='box'>";

  html += "<h2>Change Wi-Fi</h2>";

  html +=
    "<form method='POST' action='/savewifi'>";

  html +=
    "<input name='ssid' placeholder='Wi-Fi SSID' required>";

  html +=
    "<input name='password' type='password' placeholder='Wi-Fi Password'>";

  html +=
    "<button type='submit'>SAVE WI-FI</button>";

  html += "</form>";

  html += "</div>";

  html += "</body></html>";

  server.send(
    200,
    "text/html",
    html
  );
}

// =====================================================
// SAVE WIFI
// =====================================================

void handleSaveWiFi() {

  if (!server.hasArg("ssid")) {

    server.send(
      400,
      "text/plain",
      "SSID missing"
    );

    return;
  }

  String ssid =
    server.arg("ssid");

  String password =
    server.arg("password");

  preferences.begin(
    "wifi",
    false
  );

  preferences.putString(
    "ssid",
    ssid
  );

  preferences.putString(
    "password",
    password
  );

  preferences.end();

  server.send(
    200,
    "text/html",
    "<html><body>"
    "<h2>Wi-Fi Saved</h2>"
    "<p>Receiver restarting...</p>"
    "</body></html>"
  );

  delay(1500);

  ESP.restart();
}

// =====================================================
// RESET WIFI
// =====================================================

void handleResetWiFi() {

  preferences.begin(
    "wifi",
    false
  );

  preferences.clear();

  preferences.end();

  server.send(
    200,
    "text/html",
    "<html><body>"
    "<h2>Wi-Fi Reset</h2>"
    "<p>Restarting...</p>"
    "</body></html>"
  );

  delay(1000);

  ESP.restart();
}

// =====================================================
// THINGSPEAK
// =====================================================

void sendThingSpeak() {

  if (!wifiConnected) {
    Serial.println("[THINGSPEAK] Wi-Fi not connected");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[THINGSPEAK] Wi-Fi disconnected");
    return;
  }

  if (
    strlen(THINGSPEAK_API_KEY) < 5 ||
    String(THINGSPEAK_API_KEY) == "YOUR_WRITE_API_KEY"
  ) {

    Serial.println(
      "[THINGSPEAK] ERROR: Enter your WRITE API KEY."
    );

    return;
  }

  // Make sure the ThingSpeak values are current.
  updateBinLevels();

  float f1 = bioDistance;
  float f2 = nonbioDistance;
  float f3 = metalDistance;

  if (f1 < 0) f1 = 0;
  if (f2 < 0) f2 = 0;
  if (f3 < 0) f3 = 0;

  float f7 = lastConfidence;

  if (f7 < 0) f7 = 0;
  if (f7 > 100) f7 = 100;

  // ThingSpeak Field 8 must be numeric:
  // 0 READY, 1 BIO, 2 NONBIO, 3 METAL
  int f8 = 0;

  if (currentCategory == "BIO") {
    f8 = 1;
  } else if (currentCategory == "NONBIO") {
    f8 = 2;
  } else if (currentCategory == "METAL") {
    f8 = 3;
  }

  String data;

  data.reserve(320);

  data += "api_key=";
  data += THINGSPEAK_API_KEY;

  data += "&field1=";
  data += String(f1, 1);

  data += "&field2=";
  data += String(f2, 1);

  data += "&field3=";
  data += String(f3, 1);

  data += "&field4=";
  data += String(bioCount);

  data += "&field5=";
  data += String(nonbioCount);

  data += "&field6=";
  data += String(metalCount);

  data += "&field7=";
  data += String(f7, 2);

  data += "&field8=";
  data += String(f8);

  NetworkClientSecure client;
  client.setInsecure();

  HTTPClient http;

  const char* url =
    "https://api.thingspeak.com/update";

  Serial.println();
  Serial.println("========== THINGSPEAK ==========");
  Serial.print("[THINGSPEAK] F1 BIO DISTANCE       = ");
  Serial.println(f1, 1);
  Serial.print("[THINGSPEAK] F2 NONBIO DISTANCE    = ");
  Serial.println(f2, 1);
  Serial.print("[THINGSPEAK] F3 METAL BIN DISTANCE = ");
  Serial.println(f3, 1);
  Serial.print("[THINGSPEAK] F4 BIO COUNT          = ");
  Serial.println(bioCount);
  Serial.print("[THINGSPEAK] F5 NONBIO COUNT       = ");
  Serial.println(nonbioCount);
  Serial.print("[THINGSPEAK] F6 METAL COUNT        = ");
  Serial.println(metalCount);
  Serial.print("[THINGSPEAK] F7 AI CONFIDENCE      = ");
  Serial.println(f7, 2);
  Serial.print("[THINGSPEAK] F8 CATEGORY NUMBER    = ");
  Serial.println(f8);

  if (!http.begin(client, url)) {

    Serial.println(
      "[THINGSPEAK] HTTPS begin FAILED"
    );

    return;
  }

  http.setTimeout(8000);

  http.addHeader(
    "Content-Type",
    "application/x-www-form-urlencoded"
  );

  int httpCode =
    http.POST(data);

  Serial.print(
    "[THINGSPEAK] HTTP CODE = "
  );

  Serial.println(httpCode);

  String response =
    http.getString();

  Serial.print(
    "[THINGSPEAK] RESPONSE = "
  );

  Serial.println(response);

  // ThingSpeak returns a positive entry ID when accepted.
  long entryID =
    response.toInt();

  if (
    httpCode >= 200 &&
    httpCode < 300 &&
    entryID > 0
  ) {

    Serial.print(
      "[THINGSPEAK] SUCCESS. ENTRY ID = "
    );

    Serial.println(entryID);

  } else {

    Serial.println(
      "[THINGSPEAK] UPDATE FAILED"
    );
  }

  http.end();

  Serial.println(
    "================================"
  );
}

// =====================================================
// WIFI RESET BUTTON
// =====================================================

void checkWiFiResetButton() {

  bool pressed =
    digitalRead(
      WIFI_RESET_BUTTON
    ) == LOW;

  if (pressed) {

    if (!wifiButtonActive) {

      wifiButtonActive = true;

      wifiButtonStart =
        millis();

      Serial.println(
        "[WIFI] Reset button pressed"
      );
    }

    if (
      millis() -
      wifiButtonStart >= 5000
    ) {

      Serial.println(
        "[WIFI] RESET"
      );

      preferences.begin(
        "wifi",
        false
      );

      preferences.clear();

      preferences.end();

      delay(500);

      ESP.restart();
    }

  } else {

    wifiButtonActive = false;
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println(
    "=========================================="
  );

  Serial.println(
    " SMART WASTE RECEIVER"
  );

  Serial.println(
    "=========================================="
  );

  // -----------------------------------------------
  // PINS
  // -----------------------------------------------

  pinMode(
    FLAME_PIN,
    INPUT
  );

  pinMode(
    METAL_SENSOR,
    INPUT
  );

  pinMode(
    MQ2_PIN,
    INPUT
  );

  pinMode(
    BIO_TRIG,
    OUTPUT
  );

  pinMode(
    BIO_ECHO,
    INPUT
  );

  pinMode(
    NONBIO_TRIG,
    OUTPUT
  );

  pinMode(
    NONBIO_ECHO,
    INPUT
  );

  pinMode(
    METAL_TRIG,
    OUTPUT
  );

  pinMode(
    METAL_ECHO,
    INPUT
  );

  pinMode(
    GREEN_LED,
    OUTPUT
  );

  pinMode(
    RED_LED,
    OUTPUT
  );

  pinMode(
    BUZZER,
    OUTPUT
  );

  pinMode(
    WIFI_RESET_BUTTON,
    INPUT_PULLUP
  );

  digitalWrite(
    GREEN_LED,
    LOW
  );

  digitalWrite(
    RED_LED,
    LOW
  );

  digitalWrite(
    BUZZER,
    LOW
  );

  // -----------------------------------------------
  // OLED
  // -----------------------------------------------

  Wire.begin(
    21,
    22
  );

  oled.begin();

  // -----------------------------------------------
  // SERVOS
  // -----------------------------------------------

  selectorServo.setPeriodHertz(50);

  selectorServo.attach(
    SELECTOR_SERVO_PIN,
    500,
    2500
  );

  lidServo.setPeriodHertz(50);

  lidServo.attach(
    LID_SERVO_PIN,
    500,
    2500
  );

  selectorServo.writeMicroseconds(
    SELECTOR_NONBIO_US
  );

  lidServo.writeMicroseconds(
    LID_CLOSED_US
  );

  delay(500);

  // -----------------------------------------------
  // COUNTS
  // -----------------------------------------------

  preferences.begin(
    "counts",
    true
  );

  bioCount =
    preferences.getULong(
      "bio",
      0
    );

  nonbioCount =
    preferences.getULong(
      "nonbio",
      0
    );

  metalCount =
    preferences.getULong(
      "metal",
      0
    );

  preferences.end();

  // -----------------------------------------------
  // WIFI
  // -----------------------------------------------

  connectToSavedWiFi();

  // -----------------------------------------------
  // WEB
  // -----------------------------------------------

  server.on(
    "/",
    HTTP_GET,
    handleRoot
  );

  server.on(
    "/status",
    HTTP_GET,
    handleStatus
  );

  server.on(
    "/ai",
    HTTP_GET,
    handleAI
  );

  server.on(
    "/savewifi",
    HTTP_POST,
    handleSaveWiFi
  );

  server.on(
    "/resetwifi",
    HTTP_POST,
    handleResetWiFi
  );

  server.begin();

  // -----------------------------------------------
  // SENSOR
  // -----------------------------------------------

  updateBinLevels();

  updateSafety();

  if (!safetyAlarm) {

    digitalWrite(
      GREEN_LED,
      HIGH
    );

    showReadyOLED();
  }

  Serial.println(
    "=========================================="
  );

  if (wifiConnected) {

    Serial.print(
      "RECEIVER IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.println(
      "mDNS: smart-waste-receiver.local"
    );

  } else {

    Serial.println(
      "SETUP MODE: 192.168.4.1"
    );
  }

  Serial.println(
    "=========================================="
  );
}

// =====================================================
// LOOP
// =====================================================

void loop() {

  server.handleClient();

  checkWiFiResetButton();

  updateSafety();

  static unsigned long lastSensors = 0;
  static unsigned long lastMetalCheck = 0;

  // ---------------------------------------------------
  // FAST METAL MONITOR
  // This works even when Python reports NO_OBJECT.
  // ---------------------------------------------------

  if (millis() - lastMetalCheck >= 100) {

    lastMetalCheck = millis();

    bool metalNow =
      readMetalSensorStable();

    metalDetectedStable =
      metalNow;

    if (metalNow) {

      metalClearSince = 0;

      if (
        metalSortArmed &&
        !sorting &&
        !safetyAlarm
      ) {

        Serial.println(
          "[AUTO METAL] METAL DETECTED"
        );

        metalSortArmed = false;

        currentCategory = "METAL";

        sortWaste(
          "METAL",
          100.0
        );
      }

    } else {

      if (metalClearSince == 0) {
        metalClearSince = millis();
      }

      // Sensor must be clear for 700 ms before
      // another metal object can be accepted.
      if (
        millis() - metalClearSince >= 700
      ) {

        metalSortArmed = true;
      }
    }
  }

  // ---------------------------------------------------
  // BIN DISTANCES
  // ---------------------------------------------------

  if (
    millis() - lastSensors >= 1000
  ) {

    lastSensors = millis();

    if (!sorting) {

      updateBinLevels();

      Serial.print("[SENSOR] BIO=");
      Serial.print(bioDistance, 1);
      Serial.print("cm  NONBIO=");
      Serial.print(nonbioDistance, 1);
      Serial.print("cm  METALBIN=");
      Serial.print(metalDistance, 1);
      Serial.print("cm  INDUCTIVE=");

      Serial.println(
        metalDetectedStable
          ? "METAL DETECTED"
          : "NO METAL"
      );

      if (!safetyAlarm) {
        showReadyOLED();
      }
    }
  }

  // ---------------------------------------------------
  // THINGSPEAK
  // ---------------------------------------------------

  if (
    millis() - lastThingSpeak >=
    THINGSPEAK_INTERVAL
  ) {

    lastThingSpeak =
      millis();

    sendThingSpeak();
  }

  delay(2);
}
