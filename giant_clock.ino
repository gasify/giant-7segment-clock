#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <FastLED.h>
#include <ESP8266WebServer.h>

// --- HARDCODED CONFIGURATION ---
const char* ssid     = "Exclusive";
const char* password = "acc@226010";

#define DATA_PIN    D5 // Pin connected to WS2812B DI
#define LED_TYPE    WS2811
#define COLOR_ORDER GRB
#define NUM_LEDS    119

CRGB leds[NUM_LEDS];

// --- TIME & TIMEZONE CONFIGURATION ---
long utcOffsetInSeconds = 19800; 
int ntpUpdateInterval = 2147483647; 

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds, ntpUpdateInterval);

// State tracking to prevent flickering
int lastMinute = -1;
bool forceRefresh = true;
int globalBrightness = 100;

// --- SEGMENT MAPPING ---
struct DigitMap {
  int segmentIndices[7]; // A, B, C, D, E, F, G
};

DigitMap digits[4] = {
  { -1, 114, 109, -1, -1, -1, -1 },      // Digit 0: Hours Tens
  { 99, 94, 74, 79, 84, 105, 89 },       // Digit 1: Hours Ones
  { 60, 55, 35, 40, 45, 65, 50 },       // Digit 2: Minutes Tens
  { 25, 20, 0, 5, 10, 30, 15 }          // Digit 3: Minutes Ones
};

const int colonIndices[] = {70, 71, 72, 73}; 

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

// --- WEB SERVER & STATE ---
ESP8266WebServer server(80);
CRGB currentColor = CRGB::Red;

String getHexColor() {
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", currentColor.r, currentColor.g, currentColor.b);
  return String(hex);
}

void handleRoot() {
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body{font-family:sans-serif; text-align:center; background:#f4f4f4; padding:20px;} ";
  html += ".card{background:white; padding:20px; border-radius:15px; box-shadow:0 4px 6px rgba(0,0,0,0.1); max-width:400px; margin:auto;} ";
  html += "input[type='color'], input[type='range']{width:100%; margin:10px 0; cursor:pointer;} ";
  html += "input[type='number']{padding:10px; width:80%; margin:10px 0;} ";
  html += "input[type='submit'], button{background:#333; color:white; border:none; padding:12px 24px; border-radius:5px; cursor:pointer; font-size:16px; margin:5px;} ";
  html += "label{display:block; margin-top:15px; font-weight:bold;}";
  html += "</style></head><body><div class='card'>";
  html += "<h1>Giant Clock</h1>";
  html += "<h2>" + timeClient.getFormattedTime() + "</h2>";
  
  html += "<form action='/config' method='GET'>";
  html += "<label>Set Color</label>";
  html += "<input type='color' name='hex' value='" + getHexColor() + "'>";
  
  html += "<label>Brightness (" + String(globalBrightness) + ")</label>";
  html += "<input type='range' name='brightness' min='10' max='255' value='" + String(globalBrightness) + "' onchange='this.form.submit()'>";
  
  html += "<label>Timezone Offset (Seconds)</label>";
  html += "<input type='number' name='offset' value='" + String(utcOffsetInSeconds) + "'>";
  html += "<br><input type='submit' value='Save Settings'>";
  html += "</form>";
  
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleConfig() {
  if (server.hasArg("hex")) {
    String hex = server.arg("hex");
    if (hex.startsWith("#")) hex = hex.substring(1);
    long number = strtol(hex.c_str(), NULL, 16);
    currentColor.r = (number >> 16) & 0xFF;
    currentColor.g = (number >> 8) & 0xFF;
    currentColor.b = number & 0xFF;
  }
  
  if (server.hasArg("brightness")) {
    globalBrightness = server.arg("brightness").toInt();
    FastLED.setBrightness(globalBrightness);
  }
  
  if (server.hasArg("offset")) {
    utcOffsetInSeconds = server.arg("offset").toInt();
    timeClient.setTimeOffset(utcOffsetInSeconds);
    timeClient.forceUpdate();
  }
  
  forceRefresh = true;
  server.sendHeader("Location", "/");
  server.send(303);
}

// --- CLOCK LOGIC ---

void displayDigit(int digitIndex, int value, bool isBlank = false) {
  if (isBlank) {
    for (int s = 0; s < 7; s++) {
      int startIdx = digits[digitIndex].segmentIndices[s];
      if (startIdx != -1) {
        for (int i = 0; i < 5; i++) leds[startIdx + i] = CRGB::Black;
      }
    }
    return;
  }

  byte pattern = segmentPatterns[value];
  for (int s = 0; s < 7; s++) {
    int startIdx = digits[digitIndex].segmentIndices[s];
    if (startIdx == -1) continue;
    
    bool isOn = (pattern >> s) & 0x01;
    for (int i = 0; i < 5; i++) {
      leds[startIdx + i] = isOn ? currentColor : CRGB::Black;
    }
  }
}

void updateClock() {
  int currentMin = timeClient.getMinutes();
  int currentHour24 = timeClient.getHours();

  bool needsShow = false;

  if (currentMin != lastMinute || forceRefresh) {
    lastMinute = currentMin;
    forceRefresh = false;

    int displayHours = currentHour24;
    if (displayHours == 0) displayHours = 12;
    else if (displayHours > 12) displayHours -= 12;

    if (displayHours >= 10) displayDigit(0, 1);
    else displayDigit(0, 0, true); 
    
    displayDigit(1, displayHours % 10);
    displayDigit(2, currentMin / 10);
    displayDigit(3, currentMin % 10);
    
    needsShow = true;
  }

  static unsigned long lastBlink = 0;
  static bool colonState = false;
  if (millis() - lastBlink > 500) {
    lastBlink = millis();
    colonState = !colonState;
    CRGB color = colonState ? currentColor : CRGB::Black;
    for(int i=0; i<4; i++) leds[colonIndices[i]] = color;
    needsShow = true;
  }

  if (needsShow) {
    noInterrupts();
    FastLED.show();
    interrupts();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000); 

  // FastLED.addLeds with bit-banging on ESP8266 can be sensitive to interrupts
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(globalBrightness);
  FastLED.clear(true);
  FastLED.show(); 
  delay(100);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  timeClient.begin();
  
  Serial.println("Fetching initial NTP time...");
  while(!timeClient.update()) {
    timeClient.forceUpdate();
    delay(1000);
  }
  Serial.println("Time synchronized.");

  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.begin();
}

void loop() {
  // Process server requests
  server.handleClient();
  
  // Update clock logic
  updateClock();
  
  // Give the ESP8266 background tasks some time
  yield();
}