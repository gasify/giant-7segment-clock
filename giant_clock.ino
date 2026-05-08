#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <FastLED.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <RTClib.h>
#include <Wire.h>
#include <EEPROM.h>

#define DATA_PIN D5  // Pin connected to WS2812B DI
#define LED_TYPE WS2811
#define COLOR_ORDER GRB
#define NUM_LEDS 119

// I2C Pins for RTC
#define SDA_PIN D2 
#define SCL_PIN D1 

CRGB leds[NUM_LEDS];
CRGB currentColor = CRGB::Red;

// --- RTC & TIME CONFIGURATION ---
RTC_DS1307 rtc;
bool rtcFound = false;
long utcOffsetInSeconds = 19800; // Default IST
int ntpUpdateInterval = 3600000; // Sync every hour

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds, ntpUpdateInterval);

// Timezone & Mode State
char tzSign = '+';
int tzHours = 5;
int tzMinutes = 30;
int clockMode = 0; // 0 = Static Color, 1 = Rainbow
int globalBrightness = 100;
int rainbowSpeed = 5; // Default speed for rainbow animation
uint8_t hueOffset = 0; // GLOBAL hue for synchronized animation

// State tracking
int lastMinute = -1;
bool forceRefresh = true;
unsigned long lastRTCSync = 0;
const unsigned long RTC_SYNC_INTERVAL = 86400000; // Daily sync

// --- EEPROM SETTINGS ---
void saveSettings() {
  Serial.println("[EEPROM] Saving settings...");
  EEPROM.begin(512);
  EEPROM.write(0, (uint8_t)tzSign);
  EEPROM.write(1, (uint8_t)tzHours);
  EEPROM.write(2, (uint8_t)tzMinutes);
  EEPROM.write(3, (uint8_t)globalBrightness);
  EEPROM.write(4, currentColor.r);
  EEPROM.write(5, currentColor.g);
  EEPROM.write(6, currentColor.b);
  EEPROM.write(7, (uint8_t)clockMode);
  EEPROM.write(8, (uint8_t)rainbowSpeed);
  EEPROM.commit();
  Serial.printf("[EEPROM] Saved: TZ:%c%02d:%02d, Bri:%d, Mode:%d, Spd:%d, Color:R%d G%d B%d\n", 
                tzSign, tzHours, tzMinutes, globalBrightness, clockMode, rainbowSpeed, currentColor.r, currentColor.g, currentColor.b);
}

void loadSettings() {
  Serial.println("[EEPROM] Loading settings...");
  EEPROM.begin(512);
  char sign = (char)EEPROM.read(0);
  if (sign == '+' || sign == '-') {
    tzSign = sign;
    tzHours = EEPROM.read(1);
    tzMinutes = EEPROM.read(2);
    globalBrightness = EEPROM.read(3);
    currentColor.r = EEPROM.read(4);
    currentColor.g = EEPROM.read(5);
    currentColor.b = EEPROM.read(6);
    clockMode = EEPROM.read(7);
    rainbowSpeed = EEPROM.read(8);
    if (rainbowSpeed == 0 || rainbowSpeed > 50) rainbowSpeed = 5; 
    
    utcOffsetInSeconds = (tzHours * 3600) + (tzMinutes * 60);
    if (tzSign == '-') utcOffsetInSeconds = -utcOffsetInSeconds;
    
    timeClient.setTimeOffset(utcOffsetInSeconds);
    FastLED.setBrightness(globalBrightness);
    Serial.printf("[EEPROM] Loaded: TZ:%c%02d:%02d, Bri:%d, Mode:%d, Spd:%d, Color:R%d G%d B%d\n", 
                  tzSign, tzHours, tzMinutes, globalBrightness, clockMode, rainbowSpeed, currentColor.r, currentColor.g, currentColor.b);
  } else {
    Serial.println("[EEPROM] No valid data found. Using defaults.");
    FastLED.setBrightness(globalBrightness);
  }
}

void syncRTCWithNTP() {
  if (rtcFound && WiFi.status() == WL_CONNECTED) {
    Serial.println("[NTP] Attempting to sync RTC with NTP...");
    if (timeClient.update()) {
      unsigned long epochTime = timeClient.getEpochTime();
      rtc.adjust(DateTime(epochTime));
      lastRTCSync = millis();
      Serial.println("[RTC] Sync successful.");
    } else {
      Serial.println("[NTP] Update failed.");
    }
  }
}

// --- SEGMENT MAPPING ---
struct DigitMap {
  int segmentIndices[7]; 
};

DigitMap digits[4] = {
  { -1, 114, 109, -1, -1, -1, -1 },      // Digit 0 (Hours Tens)
  { 99, 94, 74, 79, 84, 104, 89 },       // Digit 1 (Hours Ones)
  { 60, 55, 35, 40, 45, 65, 50 },       // Digit 2 (Minutes Tens)
  { 25, 20, 0, 5, 10, 30, 15 }          // Digit 3 (Minutes Ones)
};

const int colonIndices[] = { 70, 71, 72, 73 };

const byte segmentPatterns[10] = {
  0b00111111, // 0
  0b00000110, // 1
  0b01011011, // 2
  0b01001111, // 3
  0b01100110, // 4
  0b01101101, // 5
  0b01111101, // 6
  0b00000111, // 7
  0b01111111, // 8
  0b01101111  // 9
};

// --- WEB SERVER ---
ESP8266WebServer server(80);

String getHexColor() {
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", currentColor.r, currentColor.g, currentColor.b);
  return String(hex);
}

void handleRoot() {
  Serial.println("[Web] Root page accessed");
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body{font-family:sans-serif; text-align:center; background:#f4f4f4; padding:20px;} ";
  html += ".card{background:white; padding:20px; border-radius:15px; box-shadow:0 4px 6px rgba(0,0,0,0.1); max-width:400px; margin:auto;} ";
  html += "select, input{width:100%; margin:8px 0; padding:10px; border-radius:5px; border:1px solid #ddd; box-sizing:border-box;} ";
  html += "input[type='color']{height:50px; cursor:pointer; background:none;} ";
  html += "input[type='submit'], button{background:#333; color:white; border:none; padding:12px; border-radius:5px; cursor:pointer; font-size:16px; font-weight:bold;} ";
  html += "button.sync{background:#007bff; margin-top:10px;} label{display:block; margin-top:10px; font-weight:bold; text-align:left;}";
  html += ".status{font-size:12px; padding:10px; border-radius:5px; margin:10px 0;}";
  html += "</style></head><body><div class='card'>";
  html += "<h1>Giant Clock</h1>";
  
  int h = 0, m = 0;
  String source = "None";
  if (rtcFound) {
    DateTime now = rtc.now();
    if (now.year() != 2165) { h = now.hour(); m = now.minute(); source = "RTC"; }
  } 
  if (source == "None" && WiFi.status() == WL_CONNECTED && timeClient.isTimeSet()) {
    h = timeClient.getHours(); m = timeClient.getMinutes(); source = "NTP";
  }
  char timeStr[20];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d", h, m);
  html += "<h2>" + String(timeStr) + "</h2>";

  bool wifiOn = (WiFi.status() == WL_CONNECTED);
  html += "<div class='status' style='background:" + String(wifiOn ? "#d4edda" : "#f8d7da") + "'>";
  html += "Time Source: " + source + " | WiFi: " + (wifiOn ? "Connected" : "Offline");
  html += "</div>";

  html += "<form action='/config' method='GET'>";
  html += "<label>Display Mode</label>";
  html += "<select name='mode' onchange='this.form.submit()'>";
  html += "<option value='0' " + String(clockMode == 0 ? "selected" : "") + ">Static Color</option>";
  html += "<option value='1' " + String(clockMode == 1 ? "selected" : "") + ">Rainbow Animation</option>";
  html += "</select>";
  html += "<label>Static Color</label>";
  html += "<input type='color' name='hex' value='" + getHexColor() + "' onchange='this.form.submit()'>";
  html += "<label>Brightness (" + String(globalBrightness) + ")</label>";
  html += "<input type='range' name='brightness' min='10' max='255' value='" + String(globalBrightness) + "' onchange='this.form.submit()'>";
  html += "<label>Rainbow Speed (" + String(rainbowSpeed) + ")</label>";
  html += "<input type='range' name='speed' min='1' max='50' value='" + String(rainbowSpeed) + "' onchange='this.form.submit()'>";
  html += "<label>Timezone Offset</label><div style='display:flex; gap:5px;'>";
  html += "<select name='sign' style='width:30%'><option value='+' "+String(tzSign=='+'?"selected":"")+">+</option><option value='-' "+String(tzSign=='-'?"selected":"")+">-</option></select>";
  html += "<select name='hours' style='width:35%'>";
  for(int i=0; i<=13; i++) html += "<option value='"+String(i)+"' "+String(tzHours==i?"selected":"")+">"+String(i)+"h</option>";
  html += "</select>";
  html += "<select name='mins' style='width:35%'><option value='0' "+String(tzMinutes==0?"selected":"")+">0m</option><option value='30' "+String(tzMinutes==30?"selected":"")+">30m</option></select>";
  html += "</div><input type='submit' value='Save All Settings'></form>";
  html += "<hr><button class='sync' onclick='syncTime()'>Sync with Browser Time</button>";
  html += "<script>function syncTime(){var ts = Math.floor(new Date().getTime()/1000); fetch('/sync_browser?epoch=' + ts).then(()=>window.location.reload());}</script>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleConfig() {
  Serial.println("[Web] Processing configuration update...");
  if (server.hasArg("hex")) {
    String hex = server.arg("hex");
    if (hex.startsWith("#")) hex = hex.substring(1);
    long number = strtol(hex.c_str(), NULL, 16);
    currentColor = CRGB((number >> 16) & 0xFF, (number >> 8) & 0xFF, number & 0xFF);
    Serial.printf("[Web] New Color: R%d G%d B%d\n", currentColor.r, currentColor.g, currentColor.b);
  }
  if (server.hasArg("brightness")) {
    globalBrightness = server.arg("brightness").toInt();
    FastLED.setBrightness(globalBrightness);
    Serial.printf("[Web] New Brightness: %d\n", globalBrightness);
  }
  if (server.hasArg("speed")) {
    rainbowSpeed = server.arg("speed").toInt();
    Serial.printf("[Web] New Rainbow Speed: %d\n", rainbowSpeed);
  }
  if (server.hasArg("mode")) {
    clockMode = server.arg("mode").toInt();
    Serial.printf("[Web] New Mode: %d\n", clockMode);
  }
  if (server.hasArg("sign")) {
    tzSign = server.arg("sign")[0];
    tzHours = server.arg("hours").toInt();
    tzMinutes = server.arg("mins").toInt();
    utcOffsetInSeconds = (tzHours * 3600) + (tzMinutes * 60);
    if (tzSign == '-') utcOffsetInSeconds = -utcOffsetInSeconds;
    timeClient.setTimeOffset(utcOffsetInSeconds);
    Serial.printf("[Web] New Timezone Offset: %ld seconds\n", utcOffsetInSeconds);
  }
  saveSettings();
  forceRefresh = true;
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSyncBrowser() {
  if (server.hasArg("epoch")) {
    long epoch = server.arg("epoch").toInt();
    Serial.printf("[Web] Browser Sync requested. Epoch: %ld\n", epoch);
    if (rtcFound) {
      rtc.adjust(DateTime(epoch));
      Serial.println("[Web] RTC adjusted.");
    } else {
      Serial.println("[Web] RTC not found, browser sync ignored by hardware.");
    }
    forceRefresh = true;
  }
  server.send(200, "text/plain", "OK");
}

void displayDigit(int digitIndex, int value, bool isBlank = false) {
  if (isBlank) {
    for (int s = 0; s < 7; s++) {
      int startIdx = digits[digitIndex].segmentIndices[s];
      if (startIdx != -1) for (int i = 0; i < 5; i++) leds[startIdx + i] = CRGB::Black;
    }
    return;
  }
  byte pattern = segmentPatterns[value];
  for (int s = 0; s < 7; s++) {
    int startIdx = digits[digitIndex].segmentIndices[s];
    if (startIdx == -1) continue;
    bool isOn = (pattern >> s) & 0x01;
    
    CRGB color;
    if (clockMode == 1) {
      // Use global hueOffset for synced rainbow
      color = CHSV(hueOffset + (digitIndex * 32), 255, 255);
    } else {
      color = currentColor;
    }
    
    for (int i = 0; i < 5; i++) {
      leds[startIdx + i] = isOn ? color : CRGB::Black;
    }
  }
  
  if (clockMode == 1) {
    static unsigned long lastHueUpdate = 0;
    // Animation speed control - updates the GLOBAL hueOffset
    if (millis() - lastHueUpdate > (51 - rainbowSpeed)) { 
      hueOffset++; 
      lastHueUpdate = millis(); 
    }
  }
}

void updateClock() {
  int h = -1, m = -1;
  bool validTime = false;
  
  if (rtcFound) {
    DateTime now = rtc.now();
    if (now.year() != 2165) { 
      h = now.hour(); m = now.minute(); validTime = true; 
    } else {
      static unsigned long lastErr = 0;
      if (millis() - lastErr > 10000) {
        Serial.println("[RTC] I2C Error 165 detected. RTC data invalid.");
        lastErr = millis();
      }
    }
  } 
  
  if (!validTime && WiFi.status() == WL_CONNECTED) {
    timeClient.update();
    if (timeClient.isTimeSet()) { 
      h = timeClient.getHours(); m = timeClient.getMinutes(); validTime = true; 
    }
  }

  // Monthly/Daily background sync
  if (rtcFound && WiFi.status() == WL_CONNECTED && (millis() - lastRTCSync > RTC_SYNC_INTERVAL)) {
    syncRTCWithNTP();
  }

  static unsigned long lastBlink = 0;
  static bool colonState = false;
  bool blinkTriggered = false;

  // Blink logic: Only occurs if NOT in rainbow mode
  if (clockMode == 0) {
    if (millis() - lastBlink > 500) {
      lastBlink = millis();
      colonState = !colonState;
      blinkTriggered = true;
    }
  } else {
    // In rainbow mode, dots are always on
    colonState = true;
  }

  // Frame rate limiter for animation to prevent flickering
  static unsigned long lastFrame = 0;
  unsigned long frameTime = (clockMode == 1) ? 25 : 100; // ~40 FPS for rainbow, slower for static

  // Logic: Refresh if time changed, if settings forced it, if we're in rainbow mode, OR if colon needs to blink
  if (validTime && (millis() - lastFrame > frameTime)) {
    if (m != lastMinute || forceRefresh || clockMode == 1 || blinkTriggered) {
      if (m != lastMinute) Serial.printf("[Clock] Time Update: %02d:%02d\n", h, m);
      if (forceRefresh) Serial.println("[Clock] Applying new settings to display.");
      
      lastMinute = m; 
      forceRefresh = false;
      lastFrame = millis();

      int dh = h;
      if (dh == 0) dh = 12; else if (dh > 12) dh -= 12;

      FastLED.clear();
      if (dh >= 10) displayDigit(0, 1); else displayDigit(0, 0, true);
      displayDigit(1, dh % 10);
      displayDigit(2, m / 10);
      displayDigit(3, m % 10);

      // FIX: Use shared hueOffset instead of millis calculation for synchronization
      CRGB colColor = colonState ? (clockMode == 1 ? CHSV(hueOffset, 255, 255) : currentColor) : CRGB::Black;
      for (int i = 0; i < 4; i++) leds[colonIndices[i]] = colColor;

      yield(); 
      noInterrupts(); 
      FastLED.show(); 
      interrupts();
    }
  } else if (!validTime && forceRefresh) {
    Serial.println("[Clock] Force refresh ignored: No valid time source available.");
    forceRefresh = false;
  }
}

void setup() {
  Serial.begin(115200);
  
  // SIGNIFICANT DELAY: Wait 5 seconds for power supply to stabilize before I2C/LED init.
  // Giant clocks often have large capacitors in the PSU that take time to charge.
  delay(5000); 
  
  Serial.println("\n\n--- Giant 7-Segment Clock Booting ---");

  // Initialize I2C first, before LEDs are even configured.
  Serial.printf("[System] Initializing I2C on SDA:%d SCL:%d\n", SDA_PIN, SCL_PIN);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000); // Standard speed is more reliable than high speed for DS1307
  delay(1000); // Stabilization delay for I2C bus

  Serial.println("[System] Initializing RTC...");
  if (!rtc.begin()) {
    Serial.println("[RTC] ERROR: Not found. Checking if module is powered and SDA/SCL are correct.");
    rtcFound = false;
  } else {
    rtcFound = true;
    Serial.println("[RTC] Module detected.");
    if (!rtc.isrunning()) {
      Serial.println("[RTC] WARNING: RTC was not running! Adjusting to compile time.");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    DateTime now = rtc.now();
    Serial.printf("[RTC] Current time read: %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());
  }
  
  // Load settings from EEPROM
  loadSettings();
  
  // Initialize LEDs LAST to avoid current spikes during sensitive RTC detection
  Serial.println("[System] Initializing FastLED...");
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  
  // Apply saved brightness
  FastLED.setBrightness(globalBrightness);
  FastLED.clear(true); 
  FastLED.show();
  delay(500); // Short delay to let current stabilize after clearing LEDs

  Serial.println("[System] Initializing WiFiManager...");
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180); 
  
  // WiFiManager blocking call
  if (wifiManager.autoConnect("GiantClock-AP")) {
    Serial.println("[WiFi] Connected successfully.");
    timeClient.begin();
    syncRTCWithNTP();
  } else {
    Serial.println("[WiFi] AutoConnect failed or timed out. Operating in RTC mode.");
  }

  Serial.println("[System] Starting Web Server...");
  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/sync_browser", handleSyncBrowser);
  server.begin();

  // Ensure digits are drawn immediately if time is available
  forceRefresh = true; 
  Serial.println("[System] Setup complete. System entering main loop.");
}

void loop() {
  server.handleClient();
  updateClock();
  yield();
}