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
volatile bool doForward = false;

// --- Body Composition Data ---
struct BodyData {
    float weight;          // kg
    float bmi;             // kg/m²
    float bodyFatPct;      // %
    float musclePct;       // %
    float waterPct;        // %
    float boneMass;        // kg
    float bmr;             // kcal
    float proteinPct;      // %
    int metabolicAge;      // years
    int visceralFat;       // index 1-30
    float subcutFatPct;    // %
    float idealWeight;     // kg
    float weightControl;   // kg (negative = lose, positive = gain)
    float fatMass;         // kg
    float fatFreeWeight;   // kg
    float muscleMass;      // kg
    float proteinMass;     // kg
    char time[20];
};
BodyData bodyData = {};

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

// --- Multi-Profile Storage ---
// NVS namespace "prof", max 8 profiles
// Keys: "count", "active", "n0".."n7" (name), "g0".."g7" (gender), "a0".."a7" (age), "h0".."h7" (height)
#define MAX_PROFILES 8

String activeProfileName = "";

struct Profile {
    String name;
    uint16_t gender; // 0=male, 1=female
    uint16_t age;
    uint16_t height; // cm
};

int getProfileCount() { return getPrefInt("prof", "count", 0); }
int getActiveIndex() { return getPrefInt("prof", "active", 0); }

Profile getProfile(int idx) {
    Profile p;
    p.name = getPref("prof", ("n" + String(idx)).c_str());
    p.gender = getPrefInt("prof", ("g" + String(idx)).c_str(), 0);
    p.age = getPrefInt("prof", ("a" + String(idx)).c_str(), 0);
    p.height = getPrefInt("prof", ("h" + String(idx)).c_str(), 0);
    return p;
}

void setProfile(int idx, const Profile& p) {
    setPref("prof", ("n" + String(idx)).c_str(), p.name);
    setPrefInt("prof", ("g" + String(idx)).c_str(), p.gender);
    setPrefInt("prof", ("a" + String(idx)).c_str(), p.age);
    setPrefInt("prof", ("h" + String(idx)).c_str(), p.height);
}

void deleteProfile(int idx) {
    int count = getProfileCount();
    // Shift profiles after idx down by one
    for (int i = idx; i < count - 1; i++) {
        Profile p = getProfile(i + 1);
        setProfile(i, p);
    }
    setPrefInt("prof", "count", count - 1);
    int active = getActiveIndex();
    if (active >= count - 1) setPrefInt("prof", "active", max(0, count - 2));
    else if (active > idx) setPrefInt("prof", "active", active - 1);
}

Profile getActiveProfile() {
    int count = getProfileCount();
    if (count == 0) { Profile p; p.age = 0; p.height = 0; p.gender = 0; return p; }
    int active = getActiveIndex();
    if (active >= count) active = 0;
    return getProfile(active);
}

// --- Body Composition Calculation ---
// Uses BMI-based estimation formulas (no impedance data from scale)
// Sources: Deurenberg (body fat), Mifflin-St Jeor (BMR), Hume (water)

void calculateBodyComposition(float weight, const char* time) {
    Profile prof = getActiveProfile();
    uint16_t heightCm = prof.height;
    uint16_t age = prof.age;
    uint16_t gender = prof.gender; // 0=male, 1=female
    activeProfileName = prof.name;

    bodyData.weight = weight;
    strncpy(bodyData.time, time, sizeof(bodyData.time) - 1);

    if (heightCm == 0 || age == 0) {
        // No profile data — only store weight
        bodyData.bmi = 0;
        Serial.println("No profile set — skipping body composition calc");
        return;
    }

    float heightM = heightCm / 100.0f;
    bool male = (gender == 0);

    // BMI
    bodyData.bmi = weight / (heightM * heightM);

    // Body Fat % — Deurenberg formula
    // BF% = 1.20 × BMI + 0.23 × age - 10.8 × sex(1=male,0=female) - 5.4
    bodyData.bodyFatPct = 1.20f * bodyData.bmi + 0.23f * age - (male ? 10.8f : 0.0f) - 5.4f;
    if (bodyData.bodyFatPct < 3.0f) bodyData.bodyFatPct = 3.0f;
    if (bodyData.bodyFatPct > 60.0f) bodyData.bodyFatPct = 60.0f;

    // Fat mass & fat-free weight
    bodyData.fatMass = weight * bodyData.bodyFatPct / 100.0f;
    bodyData.fatFreeWeight = weight - bodyData.fatMass;

    // Muscle mass (~90% of fat-free mass, protein is a component of muscle)
    bodyData.muscleMass = bodyData.fatFreeWeight * 0.90f;
    bodyData.musclePct = bodyData.muscleMass / weight * 100.0f;

    // Bone mass (~4.5% of fat-free mass, typically 2.5-4 kg)
    bodyData.boneMass = bodyData.fatFreeWeight * 0.045f;

    // Protein mass (~23% of fat-free mass, overlaps with muscle tissue)
    bodyData.proteinMass = bodyData.fatFreeWeight * 0.23f;
    bodyData.proteinPct = bodyData.proteinMass / weight * 100.0f;

    // Body water % — ~73% of fat-free mass
    bodyData.waterPct = bodyData.fatFreeWeight * 73.0f / weight;

    // Ideal weight (BMI 22)
    bodyData.idealWeight = 22.0f * heightM * heightM;

    // Weight control — how much to lose/gain
    bodyData.weightControl = weight - bodyData.idealWeight;

    // BMR — Mifflin-St Jeor
    if (male) {
        bodyData.bmr = 10.0f * weight + 6.25f * heightCm - 5.0f * age + 5;
    } else {
        bodyData.bmr = 10.0f * weight + 6.25f * heightCm - 5.0f * age - 161;
    }

    // Metabolic age — compare actual BMR to expected BMR at ideal weight
    // Difference in BMR due to excess/deficit weight maps to age offset
    float expectedBmr;
    if (male) {
        expectedBmr = 10.0f * bodyData.idealWeight + 6.25f * heightCm - 5.0f * age + 5;
    } else {
        expectedBmr = 10.0f * bodyData.idealWeight + 6.25f * heightCm - 5.0f * age - 161;
    }
    // Higher actual BMR (heavier) → older metabolic age
    // ~50 kcal BMR difference per metabolic year
    bodyData.metabolicAge = age + (int)((bodyData.bmr - expectedBmr) / 50.0f);
    if (bodyData.metabolicAge < 12) bodyData.metabolicAge = 12;
    if (bodyData.metabolicAge > 90) bodyData.metabolicAge = 90;

    // Visceral fat index — BMI/age based estimate, range 1-30
    if (male) {
        bodyData.visceralFat = (int)(bodyData.bmi * 0.68f - 8.0f);
    } else {
        bodyData.visceralFat = (int)(bodyData.bmi * 0.58f - 5.0f);
    }
    if (bodyData.visceralFat < 1) bodyData.visceralFat = 1;
    if (bodyData.visceralFat > 30) bodyData.visceralFat = 30;

    // Subcutaneous fat % — body fat minus visceral estimate
    bodyData.subcutFatPct = bodyData.bodyFatPct - bodyData.visceralFat * 0.4f;
    if (bodyData.subcutFatPct < 1.0f) bodyData.subcutFatPct = 1.0f;

    Serial.println("--- Body Composition ---");
    Serial.printf("  BMI:            %.1f\n", bodyData.bmi);
    Serial.printf("  Body Fat:       %.1f %%\n", bodyData.bodyFatPct);
    Serial.printf("  Muscle:         %.1f %% (%.1f kg)\n", bodyData.musclePct, bodyData.muscleMass);
    Serial.printf("  Water:          %.1f %%\n", bodyData.waterPct);
    Serial.printf("  Bone Mass:      %.1f kg\n", bodyData.boneMass);
    Serial.printf("  BMR:            %.0f kcal\n", bodyData.bmr);
    Serial.printf("  Protein:        %.1f %% (%.1f kg)\n", bodyData.proteinPct, bodyData.proteinMass);
    Serial.printf("  Metabolic Age:  %d\n", bodyData.metabolicAge);
    Serial.printf("  Visceral Fat:   %d\n", bodyData.visceralFat);
    Serial.printf("  Subcut. Fat:    %.1f %%\n", bodyData.subcutFatPct);
    Serial.printf("  Fat Mass:       %.1f kg\n", bodyData.fatMass);
    Serial.printf("  Fat-Free:       %.1f kg\n", bodyData.fatFreeWeight);
    Serial.printf("  Ideal Weight:   %.1f kg\n", bodyData.idealWeight);
    Serial.printf("  Weight Control: %.1f kg\n", bodyData.weightControl);
    Serial.println("------------------------");
}

// --- MQTT & HTTP forwarding ---

String buildBodyJson() {
    String j = "{";
    j += "\"weight\":" + String(bodyData.weight, 1);
    j += ",\"time\":\"" + String(bodyData.time) + "\"";
    if (!activeProfileName.isEmpty()) {
        j += ",\"profile\":\"" + activeProfileName + "\"";
    }
    if (bodyData.bmi > 0) {
        j += ",\"bmi\":" + String(bodyData.bmi, 1);
        j += ",\"body_fat_pct\":" + String(bodyData.bodyFatPct, 1);
        j += ",\"muscle_pct\":" + String(bodyData.musclePct, 1);
        j += ",\"water_pct\":" + String(bodyData.waterPct, 1);
        j += ",\"bone_mass\":" + String(bodyData.boneMass, 1);
        j += ",\"bmr\":" + String((int)bodyData.bmr);
        j += ",\"protein_pct\":" + String(bodyData.proteinPct, 1);
        j += ",\"metabolic_age\":" + String(bodyData.metabolicAge);
        j += ",\"visceral_fat\":" + String(bodyData.visceralFat);
        j += ",\"subcutaneous_fat_pct\":" + String(bodyData.subcutFatPct, 1);
        j += ",\"ideal_weight\":" + String(bodyData.idealWeight, 1);
        j += ",\"weight_control\":" + String(bodyData.weightControl, 1);
        j += ",\"fat_mass\":" + String(bodyData.fatMass, 1);
        j += ",\"fat_free_weight\":" + String(bodyData.fatFreeWeight, 1);
        j += ",\"muscle_mass\":" + String(bodyData.muscleMass, 1);
        j += ",\"protein_mass\":" + String(bodyData.proteinMass, 1);
    }
    j += "}";
    return j;
}

void forwardWeight(float weight, const char* time) {
    calculateBodyComposition(weight, time);
    String json = buildBodyJson();

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
  *{box-sizing:border-box}
  body{font-family:sans-serif;max-width:560px;margin:0 auto;padding:30px 20px;background:#111;color:#eee}
  h1{color:#4CAF50;margin-bottom:5px;font-size:clamp(22px,3.5vw,32px)}
  .home{color:#888;font-size:clamp(13px,1.5vw,15px);text-decoration:none;display:block;margin-bottom:20px}
  .card{background:#1a1a1a;border-radius:10px;padding:clamp(16px,3vw,28px);margin-bottom:18px}
  .card h2{color:#ccc;font-size:clamp(14px,2vw,17px);margin:0 0 14px 0;padding-bottom:8px;border-bottom:1px solid #333}
  input,select{width:100%;padding:clamp(10px,1.5vw,14px);margin:6px 0;border-radius:6px;border:1px solid #444;background:#222;color:#eee;font-size:clamp(14px,1.5vw,16px)}
  label{display:block;margin-top:10px;color:#999;font-size:clamp(12px,1.5vw,14px)}
  button{width:100%;padding:clamp(11px,1.5vw,15px);margin-top:14px;background:#4CAF50;color:#fff;border:none;border-radius:6px;font-size:clamp(15px,1.5vw,18px);cursor:pointer}
  button:hover{background:#45a049}
  .info{color:#888;font-size:clamp(11px,1.2vw,14px);margin-top:6px}
  .msg{padding:10px;border-radius:6px;margin-top:10px;font-size:clamp(13px,1.5vw,15px);display:none}
  .msg.ok{display:block;background:#1b3a1b;color:#4CAF50}
  .msg.err{display:block;background:#3a1b1b;color:#f44336}
  .spinner-wrap{display:flex;justify-content:center;align-items:center;padding:80px 0}
  .spinner{width:40px;height:40px;border:4px solid #333;border-top:4px solid #4CAF50;border-radius:50%;animation:spin .8s linear infinite}
  @keyframes spin{to{transform:rotate(360deg)}}
  .content{display:none}
</style>
</head><body>
<h1>OpenTrackFit</h1>
<a class="home" href="/">Zurueck zur Startseite</a>
<div class="spinner-wrap" id="loader"><div class="spinner"></div></div>
<div class="content" id="content">

<div class="card">
  <h2>Profile</h2>
  <div id="profile-list"></div>
  <div style="margin-top:14px;padding-top:14px;border-top:1px solid #333">
    <div style="color:#999;font-size:13px;margin-bottom:8px">Neues Profil / Bearbeiten</div>
    <form id="profile-form" action="/save/profile" method="POST">
      <label>Name</label>
      <input name="name" id="pf_name" placeholder="z.B. Rico" required>
      <label>Geschlecht</label>
      <select name="gender" id="pf_gender">
        <option value="0">Maennlich</option>
        <option value="1">Weiblich</option>
      </select>
      <label>Alter (Jahre)</label>
      <input name="age" id="pf_age" type="number" min="10" max="99" placeholder="z.B. 35">
      <label>Groesse (cm)</label>
      <input name="height" id="pf_height" type="number" min="100" max="250" placeholder="z.B. 180">
      <button type="submit">Profil speichern</button>
      <div class="msg" id="profile-msg"></div>
    </form>
  </div>
</div>

<div class="card">
  <h2>WLAN</h2>
  <form action="/save/wifi" method="POST">
    <label>SSID</label>
    <select name="ssid" id="ssid" style="display:none"></select>
    <input name="ssid_manual" id="ssid_manual" placeholder="WLAN Name">
    <div class="info" id="scan-status">Scanne WLANs...</div>
    <label>Passwort</label>
    <input name="pass" type="password" placeholder="WLAN Passwort">
    <button type="submit">WLAN speichern & verbinden</button>
  </form>
</div>

<div class="card">
  <h2>MQTT</h2>
  <form action="/save/mqtt" method="POST">
    <label>Broker</label>
    <input name="broker" id="mqtt_broker" placeholder="z.B. 192.168.178.50">
    <label>Port</label>
    <input name="port" id="mqtt_port" placeholder="1883" type="number">
    <label>Topic</label>
    <input name="topic" id="mqtt_topic" placeholder="opentrackfit/weight">
    <label>Benutzer (optional)</label>
    <input name="user" id="mqtt_user" placeholder="">
    <label>Passwort (optional)</label>
    <input name="pass" id="mqtt_pass" type="password" placeholder="">
    <button type="submit">MQTT speichern</button>
    <div class="msg" id="mqtt-msg"></div>
  </form>
</div>

<div class="card">
  <h2>HTTP Webhook</h2>
  <form action="/save/http" method="POST">
    <label>POST URL</label>
    <input name="webhook" id="http_webhook" placeholder="https://example.com/webhook">
    <button type="submit">Webhook speichern</button>
    <div class="msg" id="http-msg"></div>
  </form>
</div>

<div class="card">
  <h2>REST API</h2>
  <p style="color:#999;font-size:14px;margin:0">Messdaten per API abrufen:</p>
  <a href="/api/last-weight-data" target="_blank" style="display:block;margin-top:10px;color:#4CAF50;font-size:15px;word-break:break-all">/api/last-weight-data</a>
  <p style="color:#666;font-size:12px;margin:6px 0 0">Dokumentation: <a href="/api/docs" target="_blank" style="color:#888">/api/docs</a></p>
</div>

<p class="info" style="text-align:center;margin-top:20px">AP schaltet sich nach 5 Min. automatisch ab.</p>
</div>
<script>
var loader=document.getElementById('loader');
var content=document.getElementById('content');
var pending=0;
var initialized=false;
function busy(){
  pending++;
  loader.style.display='flex';
  loader.style.padding=initialized?'12px 0':'80px 0';
}
function done(){
  pending--;
  if(pending<=0){
    pending=0;
    loader.style.display='none';
    content.style.display='block';
    initialized=true;
  }
}
function api(url,opts){
  busy();
  return fetch(url,opts).then(r=>r.json()).finally(done);
}
// Initial load: scan + settings in parallel
api('/api/scan').then(d=>{
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
function loadSettings(){
  api('/api/settings').then(d=>{
    if(d.mqtt_broker) document.getElementById('mqtt_broker').value=d.mqtt_broker;
    if(d.mqtt_port) document.getElementById('mqtt_port').value=d.mqtt_port;
    if(d.mqtt_topic) document.getElementById('mqtt_topic').value=d.mqtt_topic;
    if(d.mqtt_user) document.getElementById('mqtt_user').value=d.mqtt_user;
    if(d.http_webhook) document.getElementById('http_webhook').value=d.http_webhook;
    renderProfiles(d.profiles||[]);
  }).catch(()=>{});
}
function renderProfiles(profiles){
  var el=document.getElementById('profile-list');
  if(!profiles.length){el.innerHTML='<div style="color:#666;font-size:13px">Keine Profile vorhanden</div>';return;}
  var h='';
  profiles.forEach(function(p){
    var active=p.active;
    h+='<div style="display:flex;align-items:center;gap:10px;padding:10px;margin-bottom:6px;background:'+(active?'#1b3a1b':'#222')+';border-radius:8px;border:1px solid '+(active?'#4CAF50':'#333')+'">';
    h+='<div style="flex:1"><strong style="color:'+(active?'#4CAF50':'#eee')+'">'+p.name+'</strong>';
    h+='<div style="color:#888;font-size:12px">'+(p.gender==0?'M':'W')+', '+p.age+' J., '+p.height+' cm</div></div>';
    if(!active) h+='<button onclick="setActive(\''+p.name+'\')" style="width:auto;padding:6px 12px;font-size:13px;margin:0;background:#333;color:#eee">Aktivieren</button>';
    h+='<button onclick="editProfile(\''+p.name+'\','+p.gender+','+p.age+','+p.height+')" style="width:auto;padding:6px 12px;font-size:13px;margin:0;background:#333;color:#eee">Bearbeiten</button>';
    h+='<button onclick="delProfile(\''+p.name+'\')" style="width:auto;padding:6px 10px;font-size:13px;margin:0;background:#3a1b1b;color:#f44">X</button>';
    h+='</div>';
  });
  el.innerHTML=h;
}
function setActive(name){
  api('/api/set-profile',{method:'POST',body:'name='+encodeURIComponent(name),headers:{'Content-Type':'application/x-www-form-urlencoded'}})
    .then(()=>loadSettings()).catch(()=>{});
}
function delProfile(name){
  if(!confirm(name+' loeschen?'))return;
  api('/delete/profile',{method:'POST',body:'name='+encodeURIComponent(name),headers:{'Content-Type':'application/x-www-form-urlencoded'}})
    .then(()=>loadSettings()).catch(()=>{});
}
function editProfile(name,g,a,h){
  document.getElementById('pf_name').value=name;
  document.getElementById('pf_gender').value=g;
  document.getElementById('pf_age').value=a;
  document.getElementById('pf_height').value=h;
}
loadSettings();
document.querySelectorAll('form[action="/save/mqtt"],form[action="/save/http"]').forEach(function(f){
  f.onsubmit=function(e){
    e.preventDefault();
    var fd=new FormData(f);
    var id=f.action.includes('mqtt')?'mqtt-msg':'http-msg';
    api(f.action,{method:'POST',body:new URLSearchParams(fd)})
      .then(d=>{
        var m=document.getElementById(id);
        m.textContent=d.message;
        m.className='msg '+(d.ok?'ok':'err');
      }).catch(()=>{});
  };
});
document.getElementById('profile-form').onsubmit=function(e){
  e.preventDefault();
  var fd=new FormData(this);
  api('/save/profile',{method:'POST',body:new URLSearchParams(fd)})
    .then(d=>{
      var m=document.getElementById('profile-msg');
      m.textContent=d.message;m.className='msg '+(d.ok?'ok':'err');
      if(d.ok){document.getElementById('profile-form').reset();loadSettings();}
    }).catch(()=>{});
};
</script>
</body></html>
)rawliteral";

const char WEIGHT_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenTrackFit</title>
<style>
  *{box-sizing:border-box}
  body{font-family:sans-serif;max-width:600px;margin:0 auto;padding:30px 16px;background:#111;color:#eee;text-align:center}
  h1{color:#4CAF50;margin-bottom:0;font-size:clamp(22px,4vw,32px)}
  .hero{margin:40px 0 10px}
  .hero .val{font-size:clamp(72px,15vw,130px);font-weight:bold;color:#4CAF50;line-height:1}
  .hero .unit{font-size:clamp(24px,4vw,36px);color:#888}
  .meta{font-size:clamp(13px,1.8vw,16px);color:#888;margin-bottom:30px}
  .grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-bottom:24px}
  .tile{background:#1a1a1a;border-radius:10px;padding:14px 8px}
  .tile .label{font-size:clamp(10px,1.3vw,12px);color:#888;margin-bottom:6px}
  .tile .val{font-size:clamp(20px,3.5vw,28px);font-weight:bold;color:#4CAF50}
  .tile .unit{font-size:clamp(10px,1.3vw,13px);color:#666}
  .settings{margin-top:30px}
  .settings a{color:#888;font-size:clamp(13px,1.5vw,16px)}
  .no-profile{color:#888;font-size:14px;margin:20px 0}
</style>
</head><body>
<h1>OpenTrackFit</h1>
<div id="profile-name" style="color:#888;font-size:clamp(14px,2vw,18px);margin-top:6px"></div>
<div class="hero">
  <span class="val" id="weight">--.-</span> <span class="unit">kg</span>
</div>
<div class="meta" id="meta">Noch keine Messung</div>
<div class="grid" id="tiles" style="display:none">
  <div class="tile"><div class="label">BMI</div><div class="val" id="t_bmi">-</div></div>
  <div class="tile"><div class="label">Koerperfett</div><div class="val" id="t_fat">-</div><div class="unit">%</div></div>
  <div class="tile"><div class="label">Muskelanteil</div><div class="val" id="t_muscle">-</div><div class="unit">%</div></div>
  <div class="tile"><div class="label">Koerperwasser</div><div class="val" id="t_water">-</div><div class="unit">%</div></div>
  <div class="tile"><div class="label">Knochenmasse</div><div class="val" id="t_bone">-</div><div class="unit">kg</div></div>
  <div class="tile"><div class="label">Grundumsatz</div><div class="val" id="t_bmr">-</div><div class="unit">kcal</div></div>
  <div class="tile"><div class="label">Protein</div><div class="val" id="t_protein">-</div><div class="unit">%</div></div>
  <div class="tile"><div class="label">Stoffw.-Alter</div><div class="val" id="t_age">-</div></div>
  <div class="tile"><div class="label">Viszeralfett</div><div class="val" id="t_visceral">-</div></div>
  <div class="tile"><div class="label">Subkutan. Fett</div><div class="val" id="t_subcut">-</div><div class="unit">%</div></div>
  <div class="tile"><div class="label">Idealgewicht</div><div class="val" id="t_ideal">-</div><div class="unit">kg</div></div>
  <div class="tile"><div class="label">Differenz</div><div class="val" id="t_control">-</div><div class="unit">kg</div></div>
  <div class="tile"><div class="label">Fettmasse</div><div class="val" id="t_fatmass">-</div><div class="unit">kg</div></div>
  <div class="tile"><div class="label">Fettfrei</div><div class="val" id="t_ffm">-</div><div class="unit">kg</div></div>
  <div class="tile"><div class="label">Muskelmasse</div><div class="val" id="t_musclekg">-</div><div class="unit">kg</div></div>
</div>
<div id="no-profile" style="display:none;color:#888;font-size:14px;margin:20px 0;text-align:center">Profil in <a href="/setup" style="color:#4CAF50">Einstellungen</a> anlegen fuer Koerperanalyse</div>
<div class="settings"><a href="/setup">Einstellungen</a></div>
<script>
function f1(v){return v.toFixed(1)}
function load(){
  fetch('/api/last-weight-data').then(r=>r.json()).then(d=>{
    if(d.profile) document.getElementById('profile-name').textContent=d.profile;
    if(d.weight>0){
      document.getElementById('weight').textContent=f1(d.weight);
      document.getElementById('meta').textContent=d.time?'Letzte Messung: '+d.time:'Letzte Messung';
      if(d.bmi){
        document.getElementById('tiles').style.display='grid';
        document.getElementById('no-profile').style.display='none';
        document.getElementById('t_bmi').textContent=f1(d.bmi);
        document.getElementById('t_fat').textContent=f1(d.body_fat_pct);
        document.getElementById('t_muscle').textContent=f1(d.muscle_pct);
        document.getElementById('t_water').textContent=f1(d.water_pct);
        document.getElementById('t_bone').textContent=f1(d.bone_mass);
        document.getElementById('t_bmr').textContent=d.bmr;
        document.getElementById('t_protein').textContent=f1(d.protein_pct);
        document.getElementById('t_age').textContent=d.metabolic_age;
        document.getElementById('t_visceral').textContent=d.visceral_fat;
        document.getElementById('t_subcut').textContent=f1(d.subcutaneous_fat_pct);
        document.getElementById('t_ideal').textContent=f1(d.ideal_weight);
        document.getElementById('t_control').textContent=(d.weight_control>0?'+':'')+f1(d.weight_control);
        document.getElementById('t_fatmass').textContent=f1(d.fat_mass);
        document.getElementById('t_ffm').textContent=f1(d.fat_free_weight);
        document.getElementById('t_musclekg').textContent=f1(d.muscle_mass);
      } else {
        document.getElementById('no-profile').style.display='block';
      }
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

void handleSaveWifi() {
    String ssid = server.arg("ssid");
    if (ssid == "__manual__" || ssid.isEmpty()) ssid = server.arg("ssid_manual");
    String pass = server.arg("pass");

    if (ssid.isEmpty()) {
        server.send(200, "text/html",
            "<html><head><meta http-equiv='refresh' content='3;url=/setup'></head>"
            "<body style='font-family:sans-serif;text-align:center;background:#111;color:#eee;padding:40px'>"
            "<h2 style='color:#f44336'>SSID fehlt</h2>"
            "<p style='color:#888'>Weiterleitung in 3s...</p></body></html>");
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
            "<html><head><meta http-equiv='refresh' content='5;url=http://" + ip + "/'></head>"
            "<body style='font-family:sans-serif;text-align:center;background:#111;color:#eee;padding:40px'>"
            "<h2 style='color:#4CAF50'>Verbunden!</h2>"
            "<p>WLAN: " + ssid + "</p>"
            "<p>IP: <strong>" + ip + "</strong></p>"
            "<p>mDNS: <strong>http://opentrackfit.local</strong></p>"
            "<p style='color:#888;margin-top:20px'>Neustart in 5s...</p></body></html>");
        delay(5000);
        ESP.restart();
    } else {
        WiFi.disconnect();
        WiFi.mode(WIFI_AP);
        Serial.println("WiFi test failed.");
        server.send(200, "text/html",
            "<html><head><meta http-equiv='refresh' content='5;url=/setup'></head>"
            "<body style='font-family:sans-serif;text-align:center;background:#111;color:#eee;padding:40px'>"
            "<h2 style='color:#f44336'>WLAN Verbindung fehlgeschlagen</h2>"
            "<p>WLAN: " + ssid + "</p>"
            "<p>Bitte SSID und Passwort pruefen.</p>"
            "<p style='color:#888;margin-top:20px'>Weiterleitung in 5s...</p></body></html>");
    }
}

void handleSaveMqtt() {
    setPref("mqtt", "broker", server.arg("broker"));
    String port = server.arg("port");
    setPrefInt("mqtt", "port", port.isEmpty() ? 1883 : port.toInt());
    setPref("mqtt", "topic", server.arg("topic"));
    setPref("mqtt", "user", server.arg("user"));
    setPref("mqtt", "pass", server.arg("pass"));
    Serial.println("MQTT settings saved.");
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"MQTT Einstellungen gespeichert.\"}");
}

void handleSaveHttp() {
    setPref("http", "webhook", server.arg("webhook"));
    Serial.println("HTTP webhook saved.");
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"Webhook gespeichert.\"}");
}

void handleSaveProfile() {
    String name = server.arg("name");
    String age = server.arg("age");
    String height = server.arg("height");
    String gender = server.arg("gender");

    if (name.isEmpty()) {
        server.send(200, "application/json", "{\"ok\":false,\"message\":\"Name fehlt.\"}");
        return;
    }

    int count = getProfileCount();

    // Check if profile with this name exists (update it)
    int idx = -1;
    for (int i = 0; i < count; i++) {
        if (getPref("prof", ("n" + String(i)).c_str()) == name) { idx = i; break; }
    }

    if (idx < 0) {
        // New profile
        if (count >= MAX_PROFILES) {
            server.send(200, "application/json", "{\"ok\":false,\"message\":\"Max. 8 Profile erreicht.\"}");
            return;
        }
        idx = count;
        setPrefInt("prof", "count", count + 1);
    }

    Profile p;
    p.name = name;
    p.gender = gender.isEmpty() ? 0 : gender.toInt();
    p.age = age.isEmpty() ? 0 : age.toInt();
    p.height = height.isEmpty() ? 0 : height.toInt();
    setProfile(idx, p);

    // Set as active
    setPrefInt("prof", "active", idx);

    Serial.printf("Profile saved [%d]: %s age=%d h=%d g=%d\n", idx, name.c_str(), p.age, p.height, p.gender);
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"Profil gespeichert & aktiviert.\"}");
}

void handleDeleteProfile() {
    String name = server.arg("name");
    int count = getProfileCount();
    for (int i = 0; i < count; i++) {
        if (getPref("prof", ("n" + String(i)).c_str()) == name) {
            deleteProfile(i);
            server.send(200, "application/json", "{\"ok\":true,\"message\":\"Profil geloescht.\"}");
            return;
        }
    }
    server.send(200, "application/json", "{\"ok\":false,\"message\":\"Profil nicht gefunden.\"}");
}

void handleSetActiveProfile() {
    String name = server.arg("name");
    int count = getProfileCount();
    for (int i = 0; i < count; i++) {
        if (getPref("prof", ("n" + String(i)).c_str()) == name) {
            setPrefInt("prof", "active", i);
            server.send(200, "application/json", "{\"ok\":true,\"message\":\"Profil aktiviert.\"}");
            return;
        }
    }
    server.send(200, "application/json", "{\"ok\":false,\"message\":\"Profil nicht gefunden.\"}");
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
    if (bodyData.weight > 0) {
        server.send(200, "application/json", buildBodyJson());
    } else {
        server.send(200, "application/json",
            "{\"weight\":" + String(finalWeight, 1) + ",\"time\":\"" + String(finalWeightTime) + "\"}");
    }
}

void handleApiSettings() {
    String json = "{";
    json += "\"mqtt_broker\":\"" + getPref("mqtt", "broker") + "\"";
    json += ",\"mqtt_port\":" + String(getPrefInt("mqtt", "port", 1883));
    json += ",\"mqtt_topic\":\"" + getPref("mqtt", "topic") + "\"";
    json += ",\"mqtt_user\":\"" + getPref("mqtt", "user") + "\"";
    json += ",\"http_webhook\":\"" + getPref("http", "webhook") + "\"";
    // Profiles
    int count = getProfileCount();
    int active = getActiveIndex();
    json += ",\"profiles\":[";
    for (int i = 0; i < count; i++) {
        Profile p = getProfile(i);
        if (i > 0) json += ",";
        json += "{\"name\":\"" + p.name + "\",\"gender\":" + String(p.gender);
        json += ",\"age\":" + String(p.age) + ",\"height\":" + String(p.height);
        json += ",\"active\":" + String(i == active ? "true" : "false") + "}";
    }
    json += "]";
    json += "}";
    server.send(200, "application/json", json);
}

void handleApiDocs() {
    server.send(200, "application/json",
        "{"
        "\"name\":\"OpenTrackFit API\","
        "\"version\":\"2.0\","
        "\"endpoints\":{"
          "\"GET /api/last-weight-data\":{"
            "\"description\":\"Letztes Messergebnis mit Koerperanalyse\","
            "\"fields\":{"
              "\"weight\":\"Gewicht in kg\","
              "\"time\":\"Zeitpunkt (dd.mm.yyyy HH:MM)\","
              "\"profile\":\"Name des aktiven Profils\","
              "\"bmi\":\"Body Mass Index\","
              "\"body_fat_pct\":\"Koerperfettanteil in %\","
              "\"muscle_pct\":\"Muskelanteil in %\","
              "\"muscle_mass\":\"Muskelmasse in kg\","
              "\"water_pct\":\"Koerperwasseranteil in %\","
              "\"bone_mass\":\"Knochenmasse in kg\","
              "\"bmr\":\"Grundumsatz in kcal\","
              "\"protein_pct\":\"Proteinanteil in %\","
              "\"protein_mass\":\"Proteinmasse in kg\","
              "\"metabolic_age\":\"Stoffwechselalter\","
              "\"visceral_fat\":\"Viszeralfettindex (1-30)\","
              "\"subcutaneous_fat_pct\":\"Subkutanes Fett in %\","
              "\"ideal_weight\":\"Standardgewicht in kg\","
              "\"weight_control\":\"Gewichtskontrolle in kg\","
              "\"fat_mass\":\"Fettmasse in kg\","
              "\"fat_free_weight\":\"Fettfreies Gewicht in kg\""
            "},"
            "\"note\":\"Koerperanalyse-Felder nur verfuegbar wenn Profil (Alter, Groesse, Geschlecht) gesetzt ist\""
          "},"
          "\"GET /api/docs\":{\"description\":\"Diese API-Dokumentation\"}"
        "}"
        "}");
}

void setupWebServer() {
    server.on("/", handleRoot);
    server.on("/setup", handleSetup);
    server.on("/save/wifi", HTTP_POST, handleSaveWifi);
    server.on("/save/mqtt", HTTP_POST, handleSaveMqtt);
    server.on("/save/http", HTTP_POST, handleSaveHttp);
    server.on("/save/profile", HTTP_POST, handleSaveProfile);
    server.on("/delete/profile", HTTP_POST, handleDeleteProfile);
    server.on("/api/set-profile", HTTP_POST, handleSetActiveProfile);
    server.on("/api/scan", handleApiScan);
    server.on("/api/last-weight-data", handleApiWeight);
    server.on("/api/settings", handleApiSettings);
    server.on("/api/docs", handleApiDocs);
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

void logRawData(const char* source, uint8_t* data, size_t length) {
    Serial.printf("[%s] %d bytes: ", source, length);
    for (size_t i = 0; i < length; i++) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.println();
}

void parseScaleData(uint8_t* data, size_t length, const char* source) {
    logRawData(source, data, length);

    // Known weight packet: AC 02, 8 bytes
    if (length >= 8 && data[0] == 0xAC && data[1] == 0x02) {
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
            doForward = true;
        } else {
            Serial.printf("  Measuring: %.1f kg\n", weight);
        }
    }
}

void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    const char* source = "UNK";
    if (pChar->getUUID().equals(CHR_FFB2)) source = "FFB2";
    else if (pChar->getUUID().equals(CHR_FFB3)) source = "FFB3";
    parseScaleData(pData, length, source);
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

    // Enumerate all characteristics of the service for discovery
    auto* chars = pService->getCharacteristics();
    if (chars) {
        Serial.println("--- FFB0 Service Characteristics ---");
        for (auto& entry : *chars) {
            BLERemoteCharacteristic* c = entry.second;
            Serial.printf("  UUID: %s  canNotify:%d  canRead:%d  canWrite:%d  canIndicate:%d\n",
                c->getUUID().toString().c_str(),
                c->canNotify(), c->canRead(), c->canWrite(), c->canIndicate());
            // Subscribe to ALL characteristics that can notify
            if (c->canNotify()) {
                c->registerForNotify(notifyCallback);
                Serial.printf("  -> Subscribed to %s notifications\n", c->getUUID().toString().c_str());
            }
            // Try reading any readable characteristics
            if (c->canRead()) {
                std::string val = c->readValue();
                Serial.printf("  -> Read %s (%d bytes): ", c->getUUID().toString().c_str(), val.length());
                for (size_t i = 0; i < val.length(); i++) {
                    Serial.printf("%02X ", (uint8_t)val[i]);
                }
                Serial.println();
            }
        }
        Serial.println("------------------------------------");
    }

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

    // Forward weight (deferred from BLE callback to main loop)
    if (doForward) {
        doForward = false;
        forwardWeight(finalWeight, finalWeightTime);
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
