#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>  // Install: Library Manager -> "DHT sensor library" by Adafruit + "Adafruit Unified Sensor"

// ===== Telegram alerts (Foster82_bot) =====
const String TG_TOKEN = "8609964590:AAGgHQO5DInZ71-xydWrGc9DiIgZoa2zqpw";
const String TG_CHAT = "8613422761";
const unsigned long TG_COOLDOWN = 300000; // 5 min between repeat alarm msgs
unsigned long lastTgAlarmMsg = 0;
int lastTgCode = 0;
String lastTgResp = "never sent";

// ===== MockAPI cloud config: single-row live state (PUT id=1, never fills 100) =====
String MOCKAPI_URL = "https://6465a133228bd07b354eb182.mockapi.io/readings";
const String CLOUD_FIXED_ID = "1"; // always overwrite row 1
const unsigned long CLOUD_INTERVAL = 30000; // auto PUT every 30s
unsigned long lastCloudPost = 0;
int lastCloudCode = 0;
String lastCloudResp = "never posted";

const int BUZZER_PIN = D5; // Rewired from D8 (GPIO15 boot issue)
const int LED_PIN = LED_BUILTIN;
const int DHT_PIN = D6;      // DHT11/DHT22 data -> D6
const int RELAY_PIN = D1;    // Relay IN -> D1 (safe pin)
const int GAS_PIN = A0;      // MQ-2/MQ-135 AO -> A0 (only analog pin)

#define DHTTYPE DHT11        // blue box = DHT11
DHT dht(DHT_PIN, DHTTYPE);

// Thresholds - tune to your room
const float TEMP_FAN_ON = 32.0;   // relay ON above this
const float TEMP_ALARM = 40.0;    // buzzer alarm above this
const int GAS_FAN_ON = 400;       // 0-1023, relay ON above this
const int GAS_ALARM = 600;        // buzzer alarm above this

float gTemp = NAN, gHum = NAN;
int gGas = 0;
bool gAlarm = false;
bool gAlarmMuted = false;
bool gAutoMode = true;
bool gRelayOn = false;
unsigned long lastSensorRead = 0;
unsigned long lastAlarmBeep = 0;
bool alarmBeepOn = false;
// Onboard LED manager: buzzer/alarm use it first, else WiFi status
bool ledAuto = true;          // true = LED shows WiFi, false = manual /led/on/off
bool melodyPlaying = false;   // true while blocking melody drives LED directly
bool sirenOn = false;         // continuous siren until /play/stop
int sirenFreq = 400;
unsigned long lastSirenStep = 0;
unsigned long lastLedToggle = 0;
bool ledState = false;        // false=OFF(HIGH), true=ON(LOW)

void stopSounds(const char* why) {
  sirenOn = false;
  melodyPlaying = false;
  noTone(BUZZER_PIN);
  digitalWrite(LED_PIN, HIGH);
  Serial.printf("[BUZZ] Stopped (%s)\n", why);
}

void updateLed(unsigned long now) {
  if (melodyPlaying || sirenOn) return; // siren/melody handlers drive LED directly
  if (gAlarm && !gAlarmMuted) {
    // Alarm: LED follows buzzer beep
    digitalWrite(LED_PIN, alarmBeepOn ? LOW : HIGH);
    return;
  }
  if (!ledAuto) return; // manual mode, leave as user set
  if (WiFi.status() != WL_CONNECTED) {
    // Trying to connect / reconnecting: fast blink 200ms
    if (now - lastLedToggle > 200) {
      lastLedToggle = now;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? LOW : HIGH);
    }
  } else {
    // Connected: mostly OFF, short heartbeat blink every 3s
    unsigned long phase = now % 3000;
    digitalWrite(LED_PIN, (phase < 120) ? LOW : HIGH);
  }
}

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

void setRelay(bool on, const char* why) {
  gRelayOn = on;
  digitalWrite(RELAY_PIN, on ? LOW : HIGH); // most relay boards are Active LOW
  Serial.printf("[RELAY] %s (%s)\n", on ? "ON" : "OFF", why);
}

// Single PUT to fixed id: overwrites row 1, row count stays 1, no DELETE needed.
void cloudUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[CLOUD] Skipped - WiFi not connected"));
    return;
  }
  char payload[160];
  snprintf(payload, sizeof(payload),
    "{\"temp\":%s,\"hum\":%s,\"gas\":%d,\"relay\":%s,\"alarm\":%s,\"rssi\":%d,\"uptime\":%lu}",
    isnan(gTemp) ? "null" : String(gTemp, 1).c_str(),
    isnan(gHum) ? "null" : String(gHum, 0).c_str(),
    gGas,
    gRelayOn ? "true" : "false",
    (gAlarm && !gAlarmMuted) ? "true" : "false",
    WiFi.RSSI(),
    millis() / 1000);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);
  HTTPClient http;
  String url = MOCKAPI_URL + "/" + CLOUD_FIXED_ID;
  Serial.printf("[CLOUD] PUT %s (heap %d)\n[CLOUD] Payload: %s\n", url.c_str(), ESP.getFreeHeap(), payload);
  if (!http.begin(client, url)) {
    Serial.println(F("[CLOUD] ERROR: http.begin failed"));
    lastCloudCode = -100;
    return;
  }
  http.addHeader("Content-Type", "application/json");
  lastCloudCode = http.PUT(payload);
  String raw = http.getString();
  if (lastCloudCode < 0) {
    Serial.printf("[CLOUD] PUT failed %d (%s), retrying once...\n", lastCloudCode, http.errorToString(lastCloudCode).c_str());
    delay(800);
    yield();
    lastCloudCode = http.PUT(payload);
    raw = http.getString();
  }
  Serial.printf("[CLOUD] PUT -> code %d (%s) %s\n", lastCloudCode, http.errorToString(lastCloudCode).c_str(), raw.substring(0, 120).c_str());
  if (lastCloudCode == 404) {
    Serial.println(F("[CLOUD] TIP: row id=1 missing - in MockAPI create one row (POST) or Reset, so PUT /1 has a target."));
    lastCloudResp = "404: create row id=1 first";
  } else {
    lastCloudResp = raw.substring(0, 120);
    lastCloudResp.replace("\"", "'");
    lastCloudResp.replace("\n", " ");
  }
  http.end();
  delay(300);
  yield();
}

// Kept name so existing callers/routes keep working
void postToCloud() { cloudUpdate(); }

void tgSend(const String& msg) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[TG] Skipped - WiFi not connected"));
    return;
  }
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + TG_TOKEN + "/sendMessage";
  Serial.printf("[TG] Sending (heap %d): %s\n", ESP.getFreeHeap(), msg.c_str());
  if (!http.begin(client, url)) {
    Serial.println(F("[TG] ERROR: http.begin failed"));
    lastTgCode = -100;
    return;
  }
  http.addHeader("Content-Type", "application/json");
  String body = "{\"chat_id\":\"" + TG_CHAT + "\",\"text\":\"" + msg + "\"}";
  lastTgCode = http.POST(body);
  lastTgResp = http.getString().substring(0, 120);
  lastTgResp.replace("\"", "'");
  lastTgResp.replace("\n", " ");
  lastTgResp.replace("\r", " ");
  Serial.printf("[TG] Code %d (%s) %s\n", lastTgCode, http.errorToString(lastTgCode).c_str(), lastTgResp.c_str());
  http.end();
  delay(300);
  yield();
}

void tgAlarm(const String& why) {
  unsigned long now = millis();
  if (now - lastTgAlarmMsg < TG_COOLDOWN) {
    Serial.println(F("[TG] Alarm cooldown - not resending"));
    return;
  }
  lastTgAlarmMsg = now;
  char m[200];
  snprintf(m, sizeof(m), "ALARM %s! Temp:%sC Hum:%s%% Gas:%d/1023 Relay:%s IP:%s",
    why.c_str(),
    isnan(gTemp) ? "--" : String(gTemp, 1).c_str(),
    isnan(gHum) ? "--" : String(gHum, 0).c_str(),
    gGas, gRelayOn ? "ON" : "OFF",
    WiFi.localIP().toString().c_str());
  tgSend(String(m));
}

void handleRoot() {
  logRequest("PAGE  GET /");
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;text-align:center;background:#1e1e2e;color:#fff;padding:20px;}";
  html += ".card{background:#313244;padding:15px;border-radius:12px;margin:10px auto;max-width:400px;overflow:hidden;}";
  html += ".stat{font-size:14px;word-break:break-word;overflow-wrap:anywhere;white-space:pre-wrap;line-height:1.5;text-align:center;}";
  html += ".big{font-size:32px;font-weight:bold;} .danger{color:#f38ba8;} .ok{color:#a6e3a1;}";
  html += ".btn{display:inline-block;padding:12px 20px;margin:6px;font-size:16px;color:#fff;background:#74c7ec;border:none;border-radius:8px;cursor:pointer;text-decoration:none;}";
  html += ".btn-led{background:#f9e2af;color:#111;} .btn-mario{background:#a6e3a1;color:#111;} .btn-red{background:#f38ba8;color:#111;}</style></head><body>";
  html += "<h1>Smart Room Guard <span style='font-size:14px;color:#a6e3a1'>LIVE</span></h1>";
  html += "<div class='card'><div>Temperature</div><div class='big'><span id='temp'>--</span> C</div>";
  html += "<div>Humidity: <span id='hum'>--</span> %</div></div>";
  html += "<div class='card'><div>Gas (MQ) raw</div><div class='big'><span id='gas'>--</span> / 1023</div>";
  html += "<div>Air: <span id='air'>--</span></div></div>";
  html += "<div class='card'><div>Relay (Fan): <span id='relay'>--</span> | Mode: <span id='mode'>--</span></div>";
  html += "<div>Alarm: <span id='alarm'>--</span></div><div>Board LED: <span id='led'>--</span></div><div style='font-size:12px'>Updated: <span id='upd'>--</span></div></div>";
  html += "<button class='btn' onclick=\"cmd('/relay/on')\">Relay ON</button>";
  html += "<button class='btn' onclick=\"cmd('/relay/off')\">Relay OFF</button>";
  html += "<button class='btn' onclick=\"cmd('/relay/auto')\">AUTO</button>";
  html += "<button class='btn btn-red' onclick=\"cmd('/alarm/mute')\">Mute/Unmute</button><br>";
  html += "<h3>Board LED (auto = WiFi indicator)</h3>";
  html += "<button class='btn btn-led' onclick=\"cmd('/led/on')\">LED ON</button>";
  html += "<button class='btn btn-led' onclick=\"cmd('/led/off')\">LED OFF</button>";
  html += "<button class='btn btn-led' onclick=\"cmd('/led/auto')\">LED AUTO</button>";
  html += "<h3>Sounds</h3><div>Siren: <span id='siren'>--</span></div>";
  html += "<button class='btn btn-mario' onclick=\"cmd('/play/mario')\">Mario</button>";
  html += "<button class='btn' onclick=\"cmd('/play/siren')\">Siren Start</button>";
  html += "<button class='btn btn-red' onclick=\"cmd('/play/stop')\">Stop Sound</button><br>";
  html += "<button class='btn' onclick=\"cmd('/play/doorbell')\">Doorbell</button>";
  html += "<button class='btn' onclick=\"cmd('/play/ambulance')\">Ambulance</button>";
  html += "<button class='btn' onclick=\"cmd('/play/success')\">Success</button>";
  html += "<button class='btn' onclick=\"cmd('/note?freq=262')\">C4</button>";
  html += "<h3>Cloud (MockAPI PUT id=1)</h3><div class='card'><div>Last PUT:</div><div class='stat'><span id='cloud'>--</span></div><div style='font-size:12px'>Auto every 30s, single row, never fills</div></div>";
  html += "<button class='btn' onclick=\"cmd('/cloud/test')\">Update Now</button>";
  html += "<h3>Telegram (@Foster82_bot)</h3><div class='card'><div>Last TG:</div><div class='stat'><span id='tg'>--</span></div></div>";
  html += "<button class='btn' onclick=\"cmd('/tg/test')\">Send Test</button>";
  html += "<p><a href='/api' style='color:#89b4fa'>JSON API: /api</a></p>";
  html += "<script>";
  html += "function cmd(u){fetch(u).then(()=>update());}";
  html += "async function update(){try{let r=await fetch('/api');let d=await r.json();";
  html += "document.getElementById('temp').textContent=d.temp??'--';";
  html += "document.getElementById('hum').textContent=d.hum??'--';";
  html += "document.getElementById('gas').textContent=d.gas;";
  html += "let air='Good';if(d.gas>600)air='DANGER - GAS LEAK!';else if(d.gas>400)air='Poor - ventilating';";
  html += "document.getElementById('air').textContent=air;";
  html += "document.getElementById('relay').textContent=d.relay?'ON':'OFF';";
  html += "document.getElementById('mode').textContent=d.auto?'AUTO':'MANUAL';";
  html += "document.getElementById('alarm').textContent=d.alarm?'ACTIVE!':(d.muted?'MUTED':'off');";
  html += "document.getElementById('led').textContent=d.ledAuto?('AUTO ('+(d.alarm?'ALARM blink':(d.wifi?'heartbeat':'fast blink'))+')'):(d.ledOn?'MANUAL ON':'MANUAL OFF');";
  html += "document.getElementById('cloud').textContent='code '+d.cloudCode+' | '+(d.cloudResp||'');";
  html += "document.getElementById('tg').textContent='code '+d.tgCode+' | '+(d.tgResp||'');";
  html += "document.getElementById('siren').textContent=d.siren?'RUNNING (press Stop)':'off';";
  html += "document.getElementById('upd').textContent=new Date().toLocaleTimeString();";
  html += "}catch(e){}} setInterval(update,2000);update();";
  html += "</script></body></html>";

  server.send(200, "text/html", html);
}

void handleApi() {
  String j = "{";
  j += "\"temp\":" + String(isnan(gTemp) ? "null" : String(gTemp, 1)) + ",";
  j += "\"hum\":" + String(isnan(gHum) ? "null" : String(gHum, 0)) + ",";
  j += "\"gas\":" + String(gGas) + ",";
  j += "\"relay\":" + String(gRelayOn ? "true" : "false") + ",";
  j += "\"auto\":" + String(gAutoMode ? "true" : "false") + ",";
  j += "\"muted\":" + String(gAlarmMuted ? "true" : "false") + ",";
  j += "\"wifi\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  j += "\"ledAuto\":" + String(ledAuto ? "true" : "false") + ",";
  j += "\"ledOn\":" + String(digitalRead(LED_PIN) == LOW ? "true" : "false") + ",";
  j += "\"cloudCode\":" + String(lastCloudCode) + ",";
  j += "\"cloudResp\":\"" + lastCloudResp + "\",";
  j += "\"tgCode\":" + String(lastTgCode) + ",";
  j += "\"tgResp\":\"" + lastTgResp + "\",";
  j += "\"siren\":" + String(sirenOn ? "true" : "false") + ",";
  j += "\"alarm\":" + String(gAlarm && !gAlarmMuted ? "true" : "false");
  j += "}";
  server.send(200, "application/json", j);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println(F("======================================"));
  Serial.println(F(" Smart Room Guard"));
  Serial.println(F(" Booting..."));
  Serial.println(F("======================================"));
  Serial.println(F("[WIRING] DHT data->D6, MQ AO->A0, Relay IN->D1, Buzzer->D5"));

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn off LED (Active LOW)
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RELAY_PIN, HIGH); // relay OFF (Active LOW)
  dht.begin();
  Serial.println(F("[SENS] DHT + MQ + Relay init done"));

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
  server.on("/api", []() {
    logRequest("API   GET /api");
    handleApi();
  });

  server.on("/relay/on", []() {
    logRequest("RELAY ON manual");
    gAutoMode = false;
    noTone(BUZZER_PIN);
    setRelay(true, "web manual");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/relay/off", []() {
    logRequest("RELAY OFF manual");
    gAutoMode = false;
    setRelay(false, "web manual");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/relay/auto", []() {
    logRequest("RELAY AUTO mode");
    gAutoMode = true;
    Serial.println("[RELAY] Mode -> AUTO");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/alarm/mute", []() {
    logRequest("ALARM mute toggle");
    gAlarmMuted = !gAlarmMuted;
    if (gAlarmMuted) noTone(BUZZER_PIN);
    Serial.printf("[ALARM] Muted: %s\n", gAlarmMuted ? "YES" : "NO");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/cloud/test", []() {
    logRequest("CLOUD manual PUT");
    postToCloud();
    lastCloudPost = millis(); // reset timer
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/tg/test", []() {
    logRequest("TG manual test");
    tgSend("Test from Smart Room Guard. Temp:" + String(isnan(gTemp) ? "--" : String(gTemp, 1)) + "C Gas:" + String(gGas));
    server.sendHeader("Location", "/");
    server.send(303);
  });
  
  server.on("/led/on", []() {
    logRequest("LED   LED ON manual");
    ledAuto = false;
    stopSounds("LED manual");
    digitalWrite(LED_PIN, LOW);
    Serial.println("[LED] Manual ON (siren stopped, auto WiFi-indicator OFF, use /led/auto to restore)");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/led/off", []() {
    logRequest("LED   LED OFF manual");
    ledAuto = false;
    stopSounds("LED manual");
    digitalWrite(LED_PIN, HIGH);
    Serial.println("[LED] Manual OFF (siren stopped, auto WiFi-indicator OFF, use /led/auto to restore)");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/led/auto", []() {
    logRequest("LED   LED AUTO");
    ledAuto = true;
    Serial.println("[LED] Mode -> AUTO (WiFi indicator: fast blink=connecting, heartbeat=connected, alarm blink=alarm)");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/note", []() {
    logRequest("BUZZ  Play single note");
    if (server.hasArg("freq")) {
      int freq = server.arg("freq").toInt();
      Serial.printf("[BUZZ] Playing note: %d Hz (LED follows buzzer)\n", freq);
      melodyPlaying = true;
      digitalWrite(LED_PIN, LOW);
      tone(BUZZER_PIN, freq, 300);
      delay(300);
      digitalWrite(LED_PIN, HIGH);
      melodyPlaying = false;
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/play/siren", []() {
    logRequest("BUZZ  Siren START (continuous)");
    stopSounds("siren restart");
    sirenOn = true;
    sirenFreq = 400;
    lastSirenStep = millis();
    Serial.println("[BUZZ] Siren STARTED continuous - use /play/stop to stop (LED follows siren)");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/play/stop", []() {
    logRequest("BUZZ  Stop all sounds");
    stopSounds("web button");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/play/doorbell", []() {
    logRequest("BUZZ  Doorbell ding-dong");
    stopSounds("doorbell");
    melodyPlaying = true;
    Serial.println("[BUZZ] Doorbell...");
    digitalWrite(LED_PIN, LOW);
    tone(BUZZER_PIN, NOTE_E5, 300); delay(350);
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW);
    tone(BUZZER_PIN, NOTE_C5, 500); delay(550);
    digitalWrite(LED_PIN, HIGH);
    noTone(BUZZER_PIN);
    melodyPlaying = false;
    Serial.println("[BUZZ] Doorbell finished");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/play/ambulance", []() {
    logRequest("BUZZ  Ambulance two-tone");
    stopSounds("ambulance");
    melodyPlaying = true;
    Serial.println("[BUZZ] Ambulance (5x two-tone)...");
    for (int i = 0; i < 5; i++) {
      digitalWrite(LED_PIN, LOW);
      tone(BUZZER_PIN, 700, 400); delay(450);
      digitalWrite(LED_PIN, HIGH);
      tone(BUZZER_PIN, 950, 400); delay(450);
    }
    digitalWrite(LED_PIN, HIGH);
    noTone(BUZZER_PIN);
    melodyPlaying = false;
    Serial.println("[BUZZ] Ambulance finished");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/play/success", []() {
    logRequest("BUZZ  Success jingle");
    stopSounds("success");
    melodyPlaying = true;
    Serial.println("[BUZZ] Success jingle...");
    int seq[] = {NOTE_C4, NOTE_E4, NOTE_G4, NOTE_C5};
    for (int i = 0; i < 4; i++) {
      digitalWrite(LED_PIN, LOW);
      tone(BUZZER_PIN, seq[i], 150); delay(180);
    }
    digitalWrite(LED_PIN, HIGH);
    noTone(BUZZER_PIN);
    melodyPlaying = false;
    Serial.println("[BUZZ] Success finished");
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/play/mario", []() {
    logRequest("BUZZ  Play mario");
    Serial.println("[BUZZ] Playing Mario theme... (LED follows buzzer)");
    melodyPlaying = true;
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
    melodyPlaying = false;
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
  Serial.println(F("[LED] AUTO: fast blink=connecting, heartbeat blink=connected, beep-blink=alarm, solid=melody. /led/on/off=manual, /led/auto=restore"));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Ready! Open http://%s in browser (laptop on Green or Green_5G)\n\n",
                  WiFi.localIP().toString().c_str());
    tgSend("Smart Room Guard online. IP http://" + WiFi.localIP().toString());
  } else {
    Serial.println(F("Ready, but WiFi NOT connected yet - fix WiFi, then open ESP IP\n"));
  }
}

unsigned long lastStatusPrint = 0;

void loop() {
  server.handleClient();
  unsigned long now = millis();

  // --- Read sensors every 2s ---
  if (now - lastSensorRead > 2000) {
    lastSensorRead = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    int gas = analogRead(GAS_PIN);
    if (!isnan(t)) gTemp = t;
    if (!isnan(h)) gHum = h;
    gGas = gas;

    if (isnan(t) || isnan(h)) {
      Serial.println(F("[SENS] DHT read FAILED (check wiring D6, type DHT11 vs DHT22)"));
    } else {
      Serial.printf("[SENS] Temp: %.1f C | Hum: %.0f %% | Gas: %d/1023\n", gTemp, gHum, gGas);
    }

    // Alarm + auto relay logic
    bool shouldAlarm = (!isnan(gTemp) && gTemp > TEMP_ALARM) || (gGas > GAS_ALARM);
    if (shouldAlarm && !gAlarm) {
      Serial.println(F("[ALARM] TRIGGERED! (high temp or gas leak)"));
      String why = (!isnan(gTemp) && gTemp > TEMP_ALARM) ? "HIGH TEMP" : "GAS LEAK";
      tgAlarm(why);
    } else if (!shouldAlarm && gAlarm) {
      Serial.println(F("[ALARM] Cleared"));
      noTone(BUZZER_PIN);
      alarmBeepOn = false;
      tgSend("Alarm cleared. Temp:" + String(isnan(gTemp) ? "--" : String(gTemp, 1)) + "C Gas:" + String(gGas));
    }
    gAlarm = shouldAlarm;
    if (gAlarmMuted && !gAlarm) gAlarmMuted = false; // auto unmute when safe

    if (gAutoMode) {
      bool wantRelay = (!isnan(gTemp) && gTemp > TEMP_FAN_ON) || (gGas > GAS_FAN_ON);
      if (wantRelay != gRelayOn) setRelay(wantRelay, "auto temp/gas");
    }
  }

  // --- Non-blocking alarm beep (2kHz, 300ms on/off), wins over siren ---
  bool alarmActive = (gAlarm && !gAlarmMuted);
  if (alarmActive) {
    if (now - lastAlarmBeep > 300) {
      lastAlarmBeep = now;
      alarmBeepOn = !alarmBeepOn;
      if (alarmBeepOn) tone(BUZZER_PIN, 2000);
      else noTone(BUZZER_PIN);
    }
  }
  // --- Continuous siren sweep (paused while safety alarm beeps) ---
  if (sirenOn && !alarmActive && !melodyPlaying) {
    if (now - lastSirenStep > 15) {
      lastSirenStep = now;
      sirenFreq += 20;
      if (sirenFreq > 1200) sirenFreq = 400;
      tone(BUZZER_PIN, sirenFreq);
      digitalWrite(LED_PIN, (sirenFreq % 40 == 0) ? LOW : HIGH);
    }
  }
  // --- Board LED: buzzer/alarm first, else WiFi status ---
  updateLed(now);
  // --- Cloud PUT to MockAPI every CLOUD_INTERVAL (single row id=1) ---
  if (now - lastCloudPost > CLOUD_INTERVAL) {
    lastCloudPost = now;
    postToCloud();
  }
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
