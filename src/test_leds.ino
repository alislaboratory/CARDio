/*
  ESP32-C3 + ArduinoOTA + MAX30102 + NeoPixel (No Serial needed)
  --------------------------------------------------------------
  - OTA works from Arduino IDE Network Port.
  - Keeps your pinout: I2C_SDA = GPIO9, I2C_SCL = GPIO8, NeoPixel on GPIO3.
  - Wi-Fi + OTA start BEFORE I2C to avoid strap issues (GPIO9).
  - Replaces blocking delays with OTA-friendly waits so updates stay responsive.
  - If the MAX30102 is missing, app continues so OTA still works.

  First-time upload (serial, once):
    Because GPIO9 has a pull-up, either depower the I2C device / lift the pull-up,
    OR pull GPIO9 -> GND THROUGH A 1–2.2k RESISTOR while you power-cycle/tap EN and upload.
    After this sketch is on the device, use OTA for all future uploads.

  Arduino IDE:
    Board:            Your ESP32-C3 (e.g., "ESP32C3 Dev Module")
    Partition Scheme: "Default 4MB with OTA"
    Upload Speed:     115200 (for that first serial upload only)
*/

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include "MAX30105.h"
#include <Adafruit_NeoPixel.h>
#include "heartRate.h"

// ---------- Wi-Fi (EDIT THESE) ----------
#define WIFI_SSID      "MiAX1500"
#define WIFI_PASSWORD  "Thisisthepassw0rd1"
#define OTA_PASSWORD   "derpterp"   // set a real password

// ---------- Pins / LEDs ----------
#define I2C_SDA   9
#define I2C_SCL   8
#define NEO_PIN   3
#define NUM_LEDS  13
#define BRIGHT    40

#define IR_THRESHOLD 5000   // tune if needed

// timing (ms)
const int STEP_DELAY   = 50;   // between LEDs in a sweep
const int PAUSE_SMALL  = 120;  // rest between small & medium
const int PAUSE_BIG    = 400;  // rest after big sweep
const int FADE_DELAY   = 30;   // ms between fade steps
const float FADE_MULT  = 0.85; // 0..1, lower = faster fade

// bottom → top path you gave
int ledOrder[7] = {5, 4, 3, 6, 2, 1, 0};

// ---------- Globals ----------
MAX30105 sensor;
Adafruit_NeoPixel strip(NUM_LEDS, NEO_PIN, NEO_GRB + NEO_KHZ800);
bool sensorOK = false;

// ---------- Helpers ----------

// OTA-friendly wait (keeps networking alive)
void waitWithOTA(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    ArduinoOTA.handle();
    delay(2);
  }
}

// scale a 0x00RRGGBB color by a float (0..1)
uint32_t scaleColor(uint32_t c, float m) {
  uint8_t r = (c >> 16) & 0xFF;
  uint8_t g = (c >>  8) & 0xFF;
  uint8_t b = (c      ) & 0xFF;
  r = (uint8_t)(r * m);
  g = (uint8_t)(g * m);
  b = (uint8_t)(b * m);
  return (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

// unique hostname like esp32c3-ABCDEF
String makeHostName() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[32];
  snprintf(buf, sizeof(buf), "esp32c3-%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(buf);
}

void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // don't block forever; OTA will still start if Wi-Fi takes longer
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    ArduinoOTA.handle();
    delay(100);
  }
}

void startOTA() {
  String host = makeHostName();
  ArduinoOTA.setHostname(host.c_str());
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
  // (Optional) you can add onStart/onProgress callbacks here if you want
}

void initSensor() {
  if (!sensor.begin(Wire, I2C_SPEED_STANDARD)) {
    // Keep running so OTA remains available
    sensorOK = false;
    return;
  }
  sensor.setup();                 // default config (Red+IR on)
  sensor.setPulseAmplitudeRed(0x0A);
  sensor.setPulseAmplitudeGreen(0);
  sensorOK = true;
}

void setup() {
  // Avoid Serial dependency; OTA is our primary link
  // Keep strap pins harmless at boot:
  pinMode(I2C_SDA, INPUT);
  pinMode(I2C_SCL, INPUT);

  // NeoPixel init
  strip.begin();
  strip.setBrightness(BRIGHT);
  strip.clear();
  strip.show();

  // Wi-Fi + OTA first so updates always possible
  startWiFi();
  startOTA();

  // Enable I2C after a short delay (strap-safe)
  waitWithOTA(500);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // Sensor init (non-blocking on failure)
  initSensor();
}

void loop() {
  ArduinoOTA.handle();

  long irValue = 0;
  if (sensorOK) {
    irValue = sensor.getIR();
  }

  if (sensorOK && irValue > IR_THRESHOLD) {
    // ------- Small spike sweep (green) -------
    for (int idx = 0; idx < 3; idx++) {
      strip.setPixelColor(ledOrder[idx], strip.Color(0, 180, 0));
      strip.show();
      waitWithOTA(STEP_DELAY);
    }

    // ------- Medium spike sweep (white) ------
    for (int idx = 0; idx < 4; idx++) {
      strip.setPixelColor(ledOrder[idx], strip.Color(255, 255, 255));
      strip.show();
      waitWithOTA(STEP_DELAY);
    }
    waitWithOTA(PAUSE_SMALL);

    // ------- Big spike sweep (red) -----------
    for (int idx = 0; idx < 7; idx++) {
      strip.setPixelColor(ledOrder[idx], strip.Color(255, 0, 0));
      strip.show();
      waitWithOTA(STEP_DELAY);
    }
    waitWithOTA(PAUSE_BIG);

    // ------- Fade out everything --------------
    bool anyLit = true;
    while (anyLit) {
      anyLit = false;
      for (int p = 0; p < NUM_LEDS; p++) {
        uint32_t c = strip.getPixelColor(p);
        if (c) {
          c = scaleColor(c, FADE_MULT);
          strip.setPixelColor(p, c);
          if (c) anyLit = true;
        }
      }
      strip.show();
      waitWithOTA(FADE_DELAY);
    }

  } else {
    // idle: keep off
    strip.clear();
    strip.show();
    waitWithOTA(5);
  }

  // retry sensor if it failed earlier
  static uint32_t lastRetry = 0;
  if (!sensorOK && millis() - lastRetry > 3000) {
    lastRetry = millis();
    initSensor();
  }
}
