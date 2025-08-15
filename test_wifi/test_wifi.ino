#include <WiFi.h>
#include <WebServer.h>

const char* AP_SSID     = "ashrafycard";
const char* AP_PASSWORD = "";  // 8+ chars. Use "" for an open network.

WebServer server(80);

void handleRoot() {
  server.send(200, "text/html",
              "<!DOCTYPE html><html><head><meta charset='utf-8'><title>ESP32</title></head>"
              "<body style='font-family:system-ui;margin:2rem;'>"
              "<h1>Hello World!</h1>"
              "<p>Served by ESP32 😎</p>"
              "</body></html>");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // Start Access Point
  bool ok = (strlen(AP_PASSWORD) >= 8) ?
            WiFi.softAP(AP_SSID, AP_PASSWORD) :
            WiFi.softAP(AP_SSID);           // open AP if password is ""
  IPAddress ip = WiFi.softAPIP();

  Serial.printf("\nAP %s\n", ok ? "started" : "FAILED");
  Serial.printf("SSID: %s\n", AP_SSID);
  if (strlen(AP_PASSWORD) >= 8) Serial.printf("PASS: %s\n", AP_PASSWORD);
  Serial.printf("AP IP: %s\n", ip.toString().c_str());

  // Routes
  server.on("/", handleRoot);
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
}
