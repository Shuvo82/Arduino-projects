#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

const int BUZZER_PIN = D5; // NOTE: moved from D8 (GPIO15 must be LOW at boot or WiFi/AP fails). Rewire buzzer to D5.
const int LED_PIN = LED_BUILTIN;

// Home router WiFi (2.4GHz ONLY - ESP8266 cannot use 5GHz)
// Connect laptop/phone to same router (Green or Green_5G both OK, same LAN)
const char* ssid = "Green";
const char* password = "bappy_4322";

ESP8266WebServer server(80);

// Basic note frequencies (Hz)
#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_E5  659
#define NOTE_G5  784

void logRequest(const char* action) {
  Serial.printf("[%lu ms] %s | Client: %s | URL: %s\n",
                millis(),
                action,
                server.client().remoteIP().toString().c_str(),
                server.uri().c_str());
}

void handleRoot() {
  logRequest("PAGE  GET /");
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;text-align:center;background:#1e1e2e;color:#fff;padding:20px;}";
  html += ".btn{display:inline-block;padding:15px 25px;margin:10px;font-size:18px;color:#fff;background:#74c7ec;border:none;border-radius:8px;cursor:pointer;text-decoration:none;}";
  html += ".btn-led{background:#f9e2af;color:#111;} .btn-mario{background:#a6e3a1;color:#111;}</style></head><body>";
  html += "<h1>NodeMCU Sound & Light Studio</h1>";
  html += "<p>Control LED and Buzzer over Wi-Fi</p>";
  
  html += "<h3>Toggle LED</h3>";
  html += "<a href='/led/on' class='btn btn-led'>LED ON</a>";
  html += "<a href='/led/off' class='btn btn-led'>LED OFF</a>";
  
  html += "<h3>Play Melodies</h3>";
  html += "<a href='/play/mario' class='btn btn-mario'>Play Mario Theme</a>";
  html += "<a href='/play/siren' class='btn'>Play Police Siren</a>";
  
  html += "<h3>Play Single Notes</h3>";
  html += "<a href='/note?freq=262' class='btn'>C4</a>";
  html += "<a href='/note?freq=330' class='btn'>E4</a>";
  html += "<a href='/note?freq=392' class='btn'>G4</a>";
  html += "<a href='/note?freq=523' class='btn'>C5</a>";
  
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println(F("======================================"));
  Serial.println(F(" NodeMCU Sound & Light Studio"));
  Serial.println(F(" Booting..."));
  Serial.println(F("======================================"));

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn off LED (Active LOW)
  digitalWrite(BUZZER_PIN, LOW);

  // --- Connect to home router (STA mode, 2.4GHz only) ---
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(1000);
  WiFi.mode(WIFI_STA);
  delay(100);

  Serial.printf("Connecting to WiFi... SSID: %s (2.4GHz)\n", ssid);
  Serial.println(F("[WiFi] ESP8266 cannot join 5GHz (Green_5G). It MUST join Green 2.4GHz."));
  WiFi.begin(ssid, password);

  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 60) { // 30 sec timeout
    delay(500);
    Serial.print(".");
    timeout++;
    // Blink LED while connecting
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  Serial.println();
  digitalWrite(LED_PIN, HIGH); // LED off

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("[WiFi] CONNECTED successfully!"));
    Serial.print(F("[WiFi] ESP IP address: "));
    Serial.println(WiFi.localIP());
    Serial.print(F("[WiFi] Signal (RSSI): "));
    Serial.print(WiFi.RSSI());
    Serial.println(F(" dBm"));
    Serial.print(F("[WiFi] Router gateway: "));
    Serial.println(WiFi.gatewayIP());
    Serial.printf("[WiFi] From laptop (Green_5G is OK, same router) open: http://%s\n",
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.print(F("[WiFi] FAILED to connect! Status code: "));
    Serial.println(WiFi.status());
    Serial.println(F("[WiFi] WL_IDLE_STATUS=0, NO_SSID_AVAIL=1, SCAN_COMPLETED=2, CONNECTED=3, CONNECT_FAILED=4, CONNECTION_LOST=5, DISCONNECTED=6"));
    Serial.println(F("[WiFi] CHECK: 1) SSID is 2.4GHz Green (not Green_5G) 2) password correct 3) router close by 4) MAC filter off"));
    Serial.println(F("[WiFi] Retrying in loop..."));
  }

  WiFi.setAutoReconnect(true);

  // Log WiFi disconnect/reconnect events
  WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& evt) {
    Serial.printf("[WiFi] DISCONNECTED from router | SSID: %s | Reason: %d\n", evt.ssid.c_str(), evt.reason);
  });
  WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& evt) {
    Serial.printf("[WiFi] Got IP: %s\n", evt.ip.toString().c_str());
  });

  // Web routes
  server.on("/", handleRoot);
  
  server.on("/led/on", []() {
    logRequest("LED   LED ON");
    digitalWrite(LED_PIN, LOW);
    Serial.println("[LED] LED turned ON");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/led/off", []() {
    logRequest("LED   LED OFF");
    digitalWrite(LED_PIN, HIGH);
    Serial.println("[LED] LED turned OFF");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/note", []() {
    logRequest("BUZZ  Play single note");
    if (server.hasArg("freq")) {
      int freq = server.arg("freq").toInt();
      Serial.printf("[BUZZ] Playing note: %d Hz\n", freq);
      digitalWrite(LED_PIN, LOW);
      tone(BUZZER_PIN, freq, 300);
      delay(300);
      digitalWrite(LED_PIN, HIGH);
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/play/siren", []() {
    logRequest("BUZZ  Play siren");
    Serial.println("[BUZZ] Playing police siren...");
    for (int i = 0; i < 3; i++) {
      for (int freq = 400; freq <= 1200; freq += 20) {
        digitalWrite(LED_PIN, (freq % 40 == 0) ? LOW : HIGH);
        tone(BUZZER_PIN, freq, 10);
        delay(10);
      }
    }
    digitalWrite(LED_PIN, HIGH);
    noTone(BUZZER_PIN);
    Serial.println("[BUZZ] Siren finished");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/play/mario", []() {
    logRequest("BUZZ  Play mario");
    Serial.println("[BUZZ] Playing Mario theme...");
    int notes[] = {NOTE_E5, NOTE_E5, 0, NOTE_E5, 0, NOTE_C5, NOTE_E5, 0, NOTE_G5};
    int delays[] = {150, 150, 150, 150, 150, 150, 150, 150, 300};
    
    for (int i = 0; i < 9; i++) {
      if (notes[i] != 0) {
        digitalWrite(LED_PIN, LOW);
        tone(BUZZER_PIN, notes[i], delays[i]);
      } else {
        digitalWrite(LED_PIN, HIGH);
      }
      delay(delays[i] * 1.2);
      digitalWrite(LED_PIN, HIGH);
    }
    Serial.println("[BUZZ] Mario theme finished");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.onNotFound([]() {
    logRequest("404   Not Found");
    Serial.printf("[HTTP] 404 - %s not found\n", server.uri().c_str());
    server.send(404, "text/plain", "404: Not Found");
  });

  server.begin();
  Serial.println(F("[HTTP] Web server started on port 80"));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Ready! Open http://%s in browser (laptop on Green or Green_5G)\n\n",
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.println(F("Ready, but WiFi NOT connected yet - fix WiFi, then open ESP IP\n"));
  }
}

unsigned long lastStatusPrint = 0;

void loop() {
  server.handleClient();

  // Auto-reconnect + heartbeat every 10s
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastRetry = 0;
    if (now - lastRetry > 10000) {
      lastRetry = now;
      Serial.println(F("[WiFi] Lost connection, retrying WiFi.begin()..."));
      WiFi.disconnect();
      delay(500);
      WiFi.begin(ssid, password);
    }
  } else if (now - lastStatusPrint > 10000) {
    lastStatusPrint = now;
    Serial.printf("[WiFi] Connected | SSID: %s | IP: %s | RSSI: %d dBm | Uptime: %lus\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(),
                  now / 1000);
  }
}
