/*
  ESP32-C3 — OTA + MAX30102 (raw + HR) + Local Web UI (no Serial)
  ----------------------------------------------------------------
  What it does:
    - Starts Wi‑Fi + ArduinoOTA FIRST, then enables I²C (strap‑safe for GPIO8/9).
    - Reads MAX30102 raw IR/RED and computes heart rate (checkForBeat + EWMA).
    - Serves a tiny web UI and JSON endpoint for live debugging (no Serial needed).
    - Keeps OTA responsive at all times.

  One-time first upload (serial):
    Because GPIO9 (often SCL) is pulled up on many boards, for the FIRST flash either
      • de‑power the I²C device / lift the pull‑up, OR
      • hold GPIO9 → GND THROUGH A 1–2.2 kΩ RESISTOR while you power‑cycle/tap EN, then click Upload.
    After this sketch is on the device, use Arduino IDE's Network Port for OTA forever.

  Arduino IDE:
    Board:            Your ESP32‑C3 (e.g., "ESP32C3 Dev Module")
    Partition Scheme: "Default 4MB with OTA"
    Upload Speed:     115200 (for the first serial upload only)
*/

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"

// ---------------------- Wi‑Fi (EDIT THESE) ----------------------
#define WIFI_SSID      "MiAX1500"
#define WIFI_PASSWORD  "Thisisthepassw0rd1"
#define OTA_PASSWORD   "derpterp"   // set a real password

// ---------------------- I²C pins (edit if needed) ---------------
// Default here matches common "GPIO9=SDA, GPIO8=SCL" layout.
// If your PCB uses GPIO8=SDA, GPIO9=SCL, just swap these two defines.
#define I2C_SDA   9
#define I2C_SCL   8

// ---------------------- Timing / Behavior -----------------------
static const uint32_t I2C_INIT_DELAY_MS = 500;    // wait before enabling I2C (strap-safe)
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
static const uint32_t LOOP_DELAY_MS = 10;         // base loop delay to keep CPU cool

// ---------------------- Sensor / HR -----------------------------
MAX30105 gSensor;
bool     gSensorOK = false;
long     gIR = 0;
long     gRED = 0;
float    gBpmInstant = 0;
float    gBpmAvg = 0;
bool     gBeat = false;

// HR calculation limits
static const float HR_MIN = 20.0f;
static const float HR_MAX = 220.0f;
static const float HR_EWMA_ALPHA = 0.15f; // smoothing (0..1). Larger = faster response.

// ---------------------- Web Server ------------------------------
WebServer server(80);

// Build a unique hostname like esp32c3-ABCDEF
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
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    ArduinoOTA.handle();
    delay(100);
  }

  // If STA fails, start AP for rescue (so you can still reach web UI + OTA)
  if (WiFi.status() != WL_CONNECTED) {
    String host = makeHostName();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(host.c_str());
  }
}

void startOTA() {
  String host = makeHostName();
  ArduinoOTA.setHostname(host.c_str());
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();

  // mDNS for nice URL http://<host>.local/
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("http", "tcp", 80);
  }
}

void handleRoot() {
  // Minimal, responsive web UI with auto-refreshing values
  String html;
  html.reserve(6000);
  html += F(
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-C3 MAX30102</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;background:#0e0f12;color:#e6e6e6;margin:0}"
    "header{background:#14161a;padding:12px 16px;position:sticky;top:0;box-shadow:0 2px 8px rgba(0,0,0,.35)}"
    ".wrap{padding:16px;max-width:900px;margin:0 auto}"
    ".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:12px}"
    ".card{background:#171a1f;border:1px solid #262a31;border-radius:14px;padding:14px;box-shadow:0 4px 16px rgba(0,0,0,.25)}"
    ".big{font-size:48px;font-weight:700;letter-spacing:.5px}"
    ".mono{font-family:ui-monospace, SFMono-Regular, Menlo, Consolas, monospace}"
    "canvas{width:100%;height:160px;background:#0b0c0e;border-radius:10px;border:1px solid #262a31}"
    "footer{color:#9aa0a6;padding:16px;text-align:center}"
    "button{background:#2a3140;color:#e6e6e6;border:1px solid #3a4152;border-radius:10px;padding:8px 12px;cursor:pointer}"
    "button:hover{filter:brightness(1.1)}"
    "</style></head><body>"
    "<header><div class='wrap'><b>ESP32‑C3 MAX30102</b> — OTA + Web UI</div></header>"
    "<div class='wrap'>"
    "<div class='cards'>"
      "<div class='card'><div>Heart Rate</div><div id='bpm' class='big'>--</div></div>"
      "<div class='card'><div>Beat?</div><div id='beat' class='big'>--</div></div>"
      "<div class='card'><div>IR / RED (raw)</div>"
        "<div class='mono'>IR: <span id='ir'>--</span></div>"
        "<div class='mono'>RED: <span id='red'>--</span></div>"
      "</div>"
    "</div>"
    "<div class='card' style='margin-top:12px'><canvas id='chart'></canvas></div>"
    "<div style='margin-top:10px'><button onclick='toggleStream()' id='btn'>Pause</button></div>"
    "</div>"
    "<footer>Refreshes ~5×/sec. Use Arduino IDE → Port → Network to OTA.</footer>"
    "<script>"
    "let running=true;"
    "const btn=document.getElementById('btn');"
    "function toggleStream(){running=!running;btn.textContent=running?'Pause':'Resume';}"
    "const bpmEl=document.getElementById('bpm');"
    "const beatEl=document.getElementById('beat');"
    "const irEl=document.getElementById('ir');"
    "const redEl=document.getElementById('red');"
    "const cv=document.getElementById('chart');"
    "const ctx=cv.getContext('2d');"
    "let w,h;function resize(){w=cv.clientWidth;h=cv.clientHeight;cv.width=w;cv.height=h;}"
    "resize();addEventListener('resize',resize);"
    "let pts=[];"
    "function draw(){ctx.clearRect(0,0,w,h);"
      "if(pts.length<2)return;"
      "let min=1e9,max=-1e9;"
      "for(const p of pts){if(p.ir<min)min=p.ir;if(p.ir>max)max=p.ir;}"
      "if(max==min){min-=1;max+=1;}"
      "ctx.strokeStyle='#6fb8ff';ctx.lineWidth=2;ctx.beginPath();"
      "for(let i=0;i<pts.length;i++){const x=i*(w/(pts.length-1));const y=h-(pts[i].ir-min)/(max-min)*h;"
        "if(i==0)ctx.moveTo(x,y);else ctx.lineTo(x,y);}"
      "ctx.stroke();"
      "ctx.strokeStyle='#ff6f91';ctx.beginPath();"
      "for(let i=0;i<pts.length;i++){const x=i*(w/(pts.length-1));const y=h-(pts[i].red-min)/(max-min)*h;"
        "if(i==0)ctx.moveTo(x,y);else ctx.lineTo(x,y);}"
      "ctx.stroke();"
    "}"
    "async function tick(){"
      "if(!running){setTimeout(tick,200);return;}"
      "try{const r=await fetch('/data.json');"
          "if(r.ok){const d=await r.json();"
            "bpmEl.textContent=d.bpm_avg>0?Math.round(d.bpm_avg):'--';"
            "beatEl.textContent=d.beat?'❤':'—';"
            "irEl.textContent=d.ir;"
            "redEl.textContent=d.red;"
            "pts.push({ir:d.ir,red:d.red});"
            "if(pts.length>120)pts.shift();"
            "draw();"
          "}"
      "}catch(e){}"
      "setTimeout(tick,200);"
    "}"
    "tick();"
    "</script></body></html>"
  );
  server.send(200, "text/html", html);
}

void handleData() {
  // JSON: raw + analyzed
  String out;
  out.reserve(256);
  out += F("{\"ir\":"); out += gIR;
  out += F(",\"red\":"); out += gRED;
  out += F(",\"bpm_instant\":"); out += (int)roundf(gBpmInstant);
  out += F(",\"bpm_avg\":"); out += (int)roundf(gBpmAvg);
  out += F(",\"beat\":"); out += (gBeat ? "true" : "false");
  out += F("}");
  server.send(200, "application/json", out);
}

void setupSensor() {
  if (!gSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    gSensorOK = false;
    return;
  }
  // Baseline configuration suitable for HR
  gSensor.setup();
  gSensor.setPulseAmplitudeIR(0x3F);    // increase if signal too low
  gSensor.setPulseAmplitudeRed(0x0A);   // low red (we're using IR for HR)
  gSensor.setPulseAmplitudeGreen(0x00); // off
  gSensorOK = true;
}

void setup() {
  // Keep strap pins harmless at boot
  pinMode(I2C_SDA, INPUT);
  pinMode(I2C_SCL, INPUT);

  // Wi‑Fi + OTA first
  startWiFi();
  startOTA();

  // Web server
  server.on("/", HTTP_GET, handleRoot);
  server.on("/data.json", HTTP_GET, handleData);
  server.begin();

  // Enable I2C after a short delay (strap-safe)
  delay(I2C_INIT_DELAY_MS);
  Wire.begin(I2C_SDA, I2C_SCL);

  // Sensor init (non-blocking)
  setupSensor();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  if (gSensorOK) {
    // Read raw values
    gIR  = gSensor.getIR();
    gRED = gSensor.getRed();

    // Beat detection uses IR
    gBeat = checkForBeat(gIR);
    static uint32_t lastBeatMs = 0;

    if (gBeat) {
      uint32_t now = millis();
      uint32_t delta = now - lastBeatMs;
      lastBeatMs = now;

      if (delta > 0) {
        float bpm = 60.0f / (delta / 1000.0f);
        if (bpm >= HR_MIN && bpm <= HR_MAX) {
          gBpmInstant = bpm;
          if (gBpmAvg <= 0.01f) gBpmAvg = gBpmInstant;
          gBpmAvg = (1.0f - HR_EWMA_ALPHA) * gBpmAvg + HR_EWMA_ALPHA * gBpmInstant;
        }
      }
    }
  } else {
    // retry sensor occasionally so OTA stays available if wiring/power changes
    static uint32_t lastRetry = 0;
    if (millis() - lastRetry > 3000) {
      lastRetry = millis();
      setupSensor();
    }
  }

  delay(LOOP_DELAY_MS);
}

