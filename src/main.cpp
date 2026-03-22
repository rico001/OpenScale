#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <HTTPClient.h>

// --- Config ---
#define SCALE_NAME      "FitTrack"
#define AP_SSID         "OpenTrackFit"
#define AP_PASSWORD     "12345678"
#define AP_TIMEOUT_MS   300000
#define WIFI_LOST_MS    60000

// --- BLE ---
static BLEUUID SVC_FFB0("0000ffb0-0000-1000-8000-00805f9b34fb");
static BLEUUID CHR_FFB2("0000ffb2-0000-1000-8000-00805f9b34fb");
static BLEUUID CHR_FFB3("0000ffb3-0000-1000-8000-00805f9b34fb");

#define PKT_LIVE   0xCE
#define PKT_STABLE 0xCA

// --- State ---
enum Mode { MODE_AP, MODE_STA, MODE_BLE_ONLY };

BLEScan* pBLEScan = nullptr;
BLEAdvertisedDevice* pScaleDevice = nullptr;
BLEClient* pClient = nullptr;
volatile bool connected = false;
bool doConnect = false;
bool scanning = false;

WebServer server(80);
Preferences prefs;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Mode currentMode = MODE_BLE_ONLY;
unsigned long apStartTime = 0;
unsigned long wifiLostTime = 0;

volatile float finalWeight = 0;
char finalWeightTime[20] = "";

// --- Preferences helpers ---

String getPref(const char* ns, const char* key) {
    prefs.begin(ns, true);
    String v = prefs.getString(key, "");
    prefs.end();
    return v;
}

void setPref(const char* ns, const char* key, const String& val) {
    prefs.begin(ns, false);
    prefs.putString(key, val);
    prefs.end();
}

uint16_t getPrefInt(const char* ns, const char* key, uint16_t def) {
    prefs.begin(ns, true);
    uint16_t v = prefs.getUShort(key, def);
    prefs.end();
    return v;
}

void setPrefInt(const char* ns, const char* key, uint16_t val) {
    prefs.begin(ns, false);
    prefs.putUShort(key, val);
    prefs.end();
}

// --- MQTT & HTTP forwarding ---

void forwardWeight(float weight, const char* time) {
    String json = "{\"weight\":" + String(weight, 1) + ",\"time\":\"" + String(time) + "\"}";

    // MQTT
    String broker = getPref("mqtt", "broker");
    if (!broker.isEmpty()) {
        String topic = getPref("mqtt", "topic");
        if (topic.isEmpty()) topic = "opentrackfit/weight";
        uint16_t port = getPrefInt("mqtt", "port", 1883);
        String user = getPref("mqtt", "user");
        String pass = getPref("mqtt", "pass");

        mqttClient.setServer(broker.c_str(), port);
        bool ok;
        if (!user.isEmpty()) {
            ok = mqttClient.connect("OpenTrackFit", user.c_str(), pass.c_str());
        } else {
            ok = mqttClient.connect("OpenTrackFit");
        }
        if (ok) {
            mqttClient.publish(topic.c_str(), json.c_str());
            Serial.printf("MQTT published to %s\n", topic.c_str());
            mqttClient.disconnect();
        } else {
            Serial.printf("MQTT connect failed (rc=%d)\n", mqttClient.state());
        }
    }

    // HTTP Webhook
    String webhook = getPref("http", "webhook");
    if (!webhook.isEmpty() && WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(webhook);
        http.addHeader("Content-Type", "application/json");
        int code = http.POST(json);
        Serial.printf("HTTP POST %s -> %d\n", webhook.c_str(), code);
        http.end();
    }
}

// --- HTML ---

const char CONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenTrackFit Setup</title>
<style>
  body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px;background:#111;color:#eee}
  h1{color:#4CAF50}
  h2{color:#ccc;font-size:16px;margin-top:30px;border-bottom:1px solid #333;padding-bottom:8px}
  input,select{width:100%;padding:12px;margin:8px 0;box-sizing:border-box;border-radius:6px;border:1px solid #555;background:#222;color:#eee;font-size:16px}
  button{width:100%;padding:14px;margin-top:16px;background:#4CAF50;color:#fff;border:none;border-radius:6px;font-size:18px;cursor:pointer}
  button:hover{background:#45a049}
  .info{color:#888;font-size:13px;margin-top:20px}
  label{display:block;margin-top:10px;color:#aaa;font-size:14px}
</style>
</head><body>
<h1>OpenTrackFit</h1>

<form action="/save" method="POST">
<h2>WLAN</h2>
  <label>SSID</label>
  <select name="ssid" id="ssid" style="display:none"></select>
  <input name="ssid_manual" id="ssid_manual" placeholder="WLAN Name">
  <div id="scan-status" class="info">Scanne WLANs...</div>
  <label>Passwort</label>
  <input name="pass" type="password" placeholder="WLAN Passwort">

<h2>MQTT</h2>
  <label>Broker</label>
  <input name="mqtt_broker" id="mqtt_broker" placeholder="z.B. 192.168.178.50">
  <label>Port</label>
  <input name="mqtt_port" id="mqtt_port" placeholder="1883" type="number">
  <label>Topic</label>
  <input name="mqtt_topic" id="mqtt_topic" placeholder="opentrackfit/weight">
  <label>Benutzer (optional)</label>
  <input name="mqtt_user" id="mqtt_user" placeholder="Benutzername">
  <label>Passwort (optional)</label>
  <input name="mqtt_pass" id="mqtt_pass" type="password" placeholder="Passwort">

<h2>HTTP Webhook</h2>
  <label>POST URL</label>
  <input name="http_webhook" id="http_webhook" placeholder="https://example.com/webhook">

  <button type="submit">Speichern & Verbinden</button>
</form>

<p class="info">AP schaltet sich nach 5 Minuten automatisch ab.</p>
<script>
fetch('/api/scan').then(r=>r.json()).then(d=>{
  var sel=document.getElementById('ssid');
  var inp=document.getElementById('ssid_manual');
  var st=document.getElementById('scan-status');
  if(d.length>0){
    sel.style.display='';inp.style.display='none';inp.name='';sel.name='ssid';
    d.forEach(function(n){
      var o=document.createElement('option');
      o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+'dBm)';
      sel.appendChild(o);
    });
    var o=document.createElement('option');
    o.value='__manual__';o.textContent='Manuell eingeben...';
    sel.appendChild(o);
    sel.onchange=function(){
      if(sel.value==='__manual__'){
        inp.style.display='';inp.name='ssid';inp.required=true;sel.name='';
      }else{
        inp.style.display='none';inp.name='';inp.required=false;sel.name='ssid';
      }
    };
    st.textContent=d.length+' Netzwerke gefunden';
  }else{
    st.textContent='Keine Netzwerke gefunden';inp.required=true;
  }
}).catch(()=>{
  document.getElementById('scan-status').textContent='Scan fehlgeschlagen';
  document.getElementById('ssid_manual').required=true;
});
fetch('/api/settings').then(r=>r.json()).then(d=>{
  if(d.mqtt_broker) document.getElementById('mqtt_broker').value=d.mqtt_broker;
  if(d.mqtt_port) document.getElementById('mqtt_port').value=d.mqtt_port;
  if(d.mqtt_topic) document.getElementById('mqtt_topic').value=d.mqtt_topic;
  if(d.mqtt_user) document.getElementById('mqtt_user').value=d.mqtt_user;
  if(d.http_webhook) document.getElementById('http_webhook').value=d.http_webhook;
}).catch(()=>{});
</script>
</body></html>
)rawliteral";

const char WEIGHT_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenTrackFit</title>
<style>
  body{font-family:sans-serif;max-width:500px;margin:40px auto;padding:0 20px;background:#111;color:#eee;text-align:center}
  h1{color:#4CAF50;margin-bottom:0}
  .weight{font-size:72px;font-weight:bold;margin:40px 0 10px;color:#4CAF50}
  .unit{font-size:28px;color:#888}
  .status{font-size:16px;color:#888;margin-top:20px}
  .settings{margin-top:40px}
  .settings a{color:#888;font-size:13px}
</style>
</head><body>
<h1>OpenTrackFit</h1>
<div class="weight" id="weight">--.-</div>
<div class="unit">kg</div>
<div class="status" id="status">Noch keine Messung</div>
<div class="status" id="time"></div>
<div class="settings"><a href="/setup">Netzwerkeinstellungen</a></div>
<script>
function load(){
  fetch('/api/weight').then(r=>r.json()).then(d=>{
    if(d.weight>0){
      document.getElementById('weight').textContent=d.weight.toFixed(1);
      document.getElementById('status').textContent='Letzte Messung';
      if(d.time) document.getElementById('time').textContent=d.time;
    }
  }).catch(()=>{});
}
load();
setInterval(load,5000);
</script>
</body></html>
)rawliteral";

// --- WiFi ---

bool connectWiFi() {
    String ssid = getPref("wifi", "ssid");
    String pass = getPref("wifi", "pass");
    if (ssid.isEmpty()) return false;

    Serial.printf("Connecting to WiFi: %s\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org");
        return true;
    }
    return false;
}

void startAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    currentMode = MODE_AP;
    apStartTime = millis();
}

void stopAP() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    currentMode = MODE_BLE_ONLY;
}

// --- Web Handlers ---

void handleRoot() {
    server.send(200, "text/html", currentMode == MODE_AP ? CONFIG_PAGE : WEIGHT_PAGE);
}

void handleSetup() {
    server.send(200, "text/html", CONFIG_PAGE);
}

void handleSave() {
    String ssid = server.arg("ssid");
    if (ssid == "__manual__" || ssid.isEmpty()) ssid = server.arg("ssid_manual");
    String pass = server.arg("pass");

    // Save MQTT settings
    setPref("mqtt", "broker", server.arg("mqtt_broker"));
    String port = server.arg("mqtt_port");
    setPrefInt("mqtt", "port", port.isEmpty() ? 1883 : port.toInt());
    setPref("mqtt", "topic", server.arg("mqtt_topic"));
    setPref("mqtt", "user", server.arg("mqtt_user"));
    setPref("mqtt", "pass", server.arg("mqtt_pass"));

    // Save HTTP webhook
    setPref("http", "webhook", server.arg("http_webhook"));

    Serial.println("Settings saved.");

    if (ssid.isEmpty()) {
        server.send(200, "text/html",
            "<html><body style='font-family:sans-serif;text-align:center;background:#111;color:#eee;padding:40px'>"
            "<h2 style='color:#4CAF50'>Einstellungen gespeichert!</h2>"
            "<p>MQTT und Webhook Einstellungen wurden aktualisiert.</p>"
            "<a href='/' style='color:#4CAF50;font-size:18px'>Zurueck</a></body></html>");
        return;
    }

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("Testing WiFi: %s\n", ssid.c_str());

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        setPref("wifi", "ssid", ssid);
        setPref("wifi", "pass", pass);
        Serial.printf("WiFi OK. IP: %s\n", ip.c_str());
        server.send(200, "text/html",
            "<html><body style='font-family:sans-serif;text-align:center;background:#111;color:#eee;padding:40px'>"
            "<h2 style='color:#4CAF50'>Verbunden!</h2>"
            "<p>WLAN: " + ssid + "</p>"
            "<p>IP: <strong>" + ip + "</strong></p>"
            "<p>mDNS: <strong>http://opentrackfit.local</strong></p>"
            "<p style='color:#888;margin-top:20px'>Neustart in 3s...</p></body></html>");
        delay(3000);
        ESP.restart();
    } else {
        WiFi.disconnect();
        WiFi.mode(WIFI_AP);
        Serial.println("WiFi test failed.");
        server.send(200, "text/html",
            "<html><body style='font-family:sans-serif;text-align:center;background:#111;color:#eee;padding:40px'>"
            "<h2 style='color:#f44336'>Verbindung fehlgeschlagen</h2>"
            "<p>WLAN: " + ssid + "</p>"
            "<p>Bitte SSID und Passwort pruefen.</p>"
            "<p style='color:#4CAF50;margin-top:10px'>MQTT/Webhook Einstellungen wurden trotzdem gespeichert.</p>"
            "<a href='/' style='color:#4CAF50;font-size:18px'>Erneut versuchen</a></body></html>");
    }
}

void handleApiScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    server.send(200, "application/json", json);
}

void handleApiWeight() {
    server.send(200, "application/json",
        "{\"weight\":" + String(finalWeight, 1) + ",\"time\":\"" + String(finalWeightTime) + "\"}");
}

void handleApiSettings() {
    String json = "{";
    json += "\"mqtt_broker\":\"" + getPref("mqtt", "broker") + "\"";
    json += ",\"mqtt_port\":" + String(getPrefInt("mqtt", "port", 1883));
    json += ",\"mqtt_topic\":\"" + getPref("mqtt", "topic") + "\"";
    json += ",\"mqtt_user\":\"" + getPref("mqtt", "user") + "\"";
    json += ",\"http_webhook\":\"" + getPref("http", "webhook") + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void setupWebServer() {
    server.on("/", handleRoot);
    server.on("/setup", handleSetup);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/api/scan", handleApiScan);
    server.on("/api/weight", handleApiWeight);
    server.on("/api/settings", handleApiSettings);
    server.begin();
}

// --- BLE ---

class ClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient* client) override {}
    void onDisconnect(BLEClient* client) override {
        connected = false;
        Serial.println("\n>> Scale disconnected. Rescanning...");
    }
};

void parseScaleData(uint8_t* data, size_t length) {
    if (length < 8 || data[0] != 0xAC || data[1] != 0x02) return;

    uint8_t type = data[6];

    uint8_t sum = 0;
    for (int i = 2; i <= 6; i++) sum += data[i];
    if (sum != data[7]) return;

    if (type != PKT_LIVE && type != PKT_STABLE) return;

    uint16_t raw = (data[2] << 8) | data[3];
    float weight = raw / 10.0f;

    if (type == PKT_STABLE) {
        finalWeight = weight;
        struct tm ti;
        if (getLocalTime(&ti, 0)) {
            strftime(finalWeightTime, sizeof(finalWeightTime), "%d.%m.%Y %H:%M", &ti);
        } else {
            strcpy(finalWeightTime, "");
        }
        Serial.printf(">>> FINAL WEIGHT: %.1f kg <<<\n", weight);
        forwardWeight(weight, finalWeightTime);
    } else {
        Serial.printf("  Measuring: %.1f kg\n", weight);
    }
}

void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    parseScaleData(pData, length);
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice device) override {
        if (device.haveName() && device.getName() == SCALE_NAME) {
            Serial.println("FitTrack found!");
            if (pScaleDevice) delete pScaleDevice;
            pScaleDevice = new BLEAdvertisedDevice(device);
            doConnect = true;
            pBLEScan->stop();
        }
    }
};

bool connectToScale() {
    Serial.printf("Connecting to %s...\n", pScaleDevice->getAddress().toString().c_str());

    if (pClient) { delete pClient; pClient = nullptr; }
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new ClientCallbacks());

    if (!pClient->connect(pScaleDevice)) {
        Serial.println("Connection failed!");
        return false;
    }

    BLERemoteService* pService = pClient->getService(SVC_FFB0);
    if (!pService) {
        Serial.println("Service 0xFFB0 not found!");
        pClient->disconnect();
        return false;
    }

    BLERemoteCharacteristic* pFFB2 = pService->getCharacteristic(CHR_FFB2);
    if (pFFB2 && pFFB2->canNotify()) pFFB2->registerForNotify(notifyCallback);

    BLERemoteCharacteristic* pFFB3 = pService->getCharacteristic(CHR_FFB3);
    if (pFFB3 && pFFB3->canNotify()) pFFB3->registerForNotify(notifyCallback);

    Serial.println("Connected. Waiting for measurement...");
    connected = true;
    return true;
}

void setupBLE() {
    BLEDevice::init("OpenTrackFit");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), false);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
}

// --- Main ---

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== OpenTrackFit ===");

    setupBLE();

    if (connectWiFi()) {
        currentMode = MODE_STA;
        MDNS.begin("opentrackfit");
        setupWebServer();
        Serial.println("-----------------------------");
        Serial.println("Mode:  LAN (Station)");
        Serial.printf("IP:    %s\n", WiFi.localIP().toString().c_str());
        Serial.println("mDNS:  http://opentrackfit.local");
        Serial.println("-----------------------------");
    } else {
        startAP();
        setupWebServer();
        Serial.println("-----------------------------");
        Serial.println("Mode:  AP (Access Point)");
        Serial.printf("SSID:  %s\n", AP_SSID);
        Serial.printf("Pass:  %s\n", AP_PASSWORD);
        Serial.printf("IP:    %s\n", WiFi.softAPIP().toString().c_str());
        Serial.println("-----------------------------");
    }
}

void loop() {
    server.handleClient();

    // AP timeout
    if (currentMode == MODE_AP && (millis() - apStartTime > AP_TIMEOUT_MS)) {
        stopAP();
        if (connectWiFi()) {
            currentMode = MODE_STA;
            MDNS.begin("opentrackfit");
            Serial.printf("WiFi reconnected. IP: %s\n", WiFi.localIP().toString().c_str());
        }
    }

    // WiFi watchdog
    if (currentMode == MODE_STA) {
        if (WiFi.status() != WL_CONNECTED) {
            if (wifiLostTime == 0) {
                wifiLostTime = millis();
                Serial.println("WiFi lost. Waiting 60s...");
            } else if (millis() - wifiLostTime > WIFI_LOST_MS) {
                Serial.println("WiFi lost for 60s. AP fallback.");
                wifiLostTime = 0;
                startAP();
            }
        } else {
            wifiLostTime = 0;
        }
    }

    // BLE connect
    if (doConnect && !connected) {
        connectToScale();
        doConnect = false;
    }

    // BLE non-blocking scan
    if (!connected && !doConnect && !scanning) {
        pBLEScan->start(5, [](BLEScanResults) { scanning = false; }, false);
        scanning = true;
    }

    delay(10);
}
