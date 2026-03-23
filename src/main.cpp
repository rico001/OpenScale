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
volatile bool weightReady = false;   // set after CA (stable weight), cleared after forward
volatile uint16_t impedanceRaw = 0;  // BIA impedance from CB packets (ohms)

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
    uint16_t impedance;    // ohms (BIA), 0 = not available
    bool biaValid;         // true if impedance-based calc was used
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

void saveBodyData() {
    prefs.begin("body", false);
    prefs.putBytes("data", &bodyData, sizeof(bodyData));
    prefs.end();
}

void loadBodyData() {
    prefs.begin("body", true);
    if (prefs.getBytesLength("data") == sizeof(bodyData)) {
        prefs.getBytes("data", &bodyData, sizeof(bodyData));
        if (bodyData.weight > 0)
            Serial.printf("Restored last measurement: %.1f kg (%s)\n", bodyData.weight, bodyData.time);
    }
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
// Uses impedance-based formulas when BIA data available, falls back to BMI-based
// Sources: Kushner/Schoeller (FFM from BIA, adjusted for foot-to-foot), Owen (BMR), Broca (ideal weight)

void calculateBodyComposition(float weight, const char* time) {
    Profile prof = getActiveProfile();
    uint16_t heightCm = prof.height;
    uint16_t age = prof.age;
    uint16_t gender = prof.gender; // 0=male, 1=female
    activeProfileName = prof.name;
    uint16_t impedance = impedanceRaw;

    bodyData.weight = weight;
    bodyData.impedance = impedance;
    strncpy(bodyData.time, time, sizeof(bodyData.time) - 1);

    if (heightCm == 0 || age == 0) {
        bodyData.bmi = 0;
        Serial.println("No profile set — skipping body composition calc");
        return;
    }

    float heightM = heightCm / 100.0f;
    bool male = (gender == 0);

    // BMI = weight / height²
    bodyData.bmi = weight / (heightM * heightM);

    // Ideal weight — modified Broca: male (H-80)*0.7, female (H-70)*0.6
    if (male) {
        bodyData.idealWeight = (heightCm - 80) * 0.7f;
    } else {
        bodyData.idealWeight = (heightCm - 70) * 0.6f;
    }
    bodyData.weightControl = weight - bodyData.idealWeight;

    // BMR — Owen (1986): male 879+10.2*W, female 795+7.18*W
    if (male) {
        bodyData.bmr = 879.0f + 10.2f * weight;
    } else {
        bodyData.bmr = 795.0f + 7.18f * weight;
    }

    // Metabolic age — offset from actual age based on BMR vs expected BMR at ideal weight
    float expectedBmr;
    if (male) {
        expectedBmr = 879.0f + 10.2f * bodyData.idealWeight;
    } else {
        expectedBmr = 795.0f + 7.18f * bodyData.idealWeight;
    }
    bodyData.metabolicAge = age + (int)((bodyData.bmr - expectedBmr) / 85.0f);
    if (bodyData.metabolicAge < 12) bodyData.metabolicAge = 12;
    if (bodyData.metabolicAge > 90) bodyData.metabolicAge = 90;

    // Visceral fat index — empirical BMI-based regression, clamped 1-30
    if (male) {
        bodyData.visceralFat = (int)(bodyData.bmi * 0.68f - 7.2f);
    } else {
        bodyData.visceralFat = (int)(bodyData.bmi * 0.58f - 4.2f);
    }
    if (bodyData.visceralFat < 1) bodyData.visceralFat = 1;
    if (bodyData.visceralFat > 30) bodyData.visceralFat = 30;

    // --- BIA-dependent values (require valid impedance) ---
    if (impedance > 0) {
        bodyData.biaValid = true;
        // Fat-free mass — Kushner/Schoeller with adjusted H²/Z coefficient for foot-to-foot BIA
        // FFM = a * (H²/Z) + b * W + c, where H in cm, Z in ohms
        float h2z = (float)(heightCm * heightCm) / impedance;
        if (male) {
            bodyData.fatFreeWeight = 0.600f * h2z + 0.338f * weight + 5.32f;
        } else {
            bodyData.fatFreeWeight = 0.589f * h2z + 0.295f * weight + 5.49f;
        }
        if (bodyData.fatFreeWeight > weight) bodyData.fatFreeWeight = weight * 0.95f;
        bodyData.fatMass = weight - bodyData.fatFreeWeight;
        bodyData.bodyFatPct = bodyData.fatMass / weight * 100.0f;
        if (bodyData.bodyFatPct < 3.0f) bodyData.bodyFatPct = 3.0f;

        // Derived from fat-free mass using fixed tissue ratios
        bodyData.muscleMass = bodyData.fatFreeWeight * 0.90f;   // 90% of FFM
        bodyData.musclePct = bodyData.muscleMass / weight * 100.0f;
        bodyData.boneMass = bodyData.fatFreeWeight * 0.046f;    // 4.6% of FFM
        bodyData.proteinMass = bodyData.fatFreeWeight * 0.231f; // 23.1% of FFM
        bodyData.proteinPct = bodyData.proteinMass / weight * 100.0f;
        bodyData.waterPct = bodyData.fatFreeWeight * 72.4f / weight; // 72.4% of FFM
        bodyData.subcutFatPct = bodyData.bodyFatPct - bodyData.visceralFat * 0.21f;
        if (bodyData.subcutFatPct < 1.0f) bodyData.subcutFatPct = 1.0f;

        Serial.printf("  Using BIA impedance: %d ohms\n", impedance);
    } else {
        bodyData.biaValid = false;
        // Zero out BIA-dependent fields
        bodyData.bodyFatPct = 0;
        bodyData.fatMass = 0;
        bodyData.fatFreeWeight = 0;
        bodyData.muscleMass = 0;
        bodyData.musclePct = 0;
        bodyData.boneMass = 0;
        bodyData.proteinMass = 0;
        bodyData.proteinPct = 0;
        bodyData.waterPct = 0;
        bodyData.subcutFatPct = 0;
        Serial.println("  No impedance — BIA values not available");
    }

    Serial.println("--- Body Composition ---");
    if (bodyData.impedance > 0)
        Serial.printf("  Impedance:      %d ohms\n", bodyData.impedance);
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
    if (bodyData.impedance > 0) {
        j += ",\"impedance\":" + String(bodyData.impedance);
    }
    if (bodyData.bmi > 0) {
        j += ",\"body_analysis\":" + String(bodyData.biaValid ? "true" : "false");
        // Always available with profile
        j += ",\"bmi\":" + String(bodyData.bmi, 1);
        j += ",\"bmr\":" + String((int)bodyData.bmr);
        j += ",\"metabolic_age\":" + String(bodyData.metabolicAge);
        j += ",\"visceral_fat\":" + String(bodyData.visceralFat);
        j += ",\"ideal_weight\":" + String(bodyData.idealWeight, 1);
        j += ",\"weight_control\":" + String(bodyData.weightControl, 1);
        // BIA-dependent (null if no valid impedance)
        if (bodyData.biaValid) {
            j += ",\"body_fat_pct\":" + String(bodyData.bodyFatPct, 1);
            j += ",\"muscle_pct\":" + String(bodyData.musclePct, 1);
            j += ",\"water_pct\":" + String(bodyData.waterPct, 1);
            j += ",\"bone_mass\":" + String(bodyData.boneMass, 1);
            j += ",\"protein_pct\":" + String(bodyData.proteinPct, 1);
            j += ",\"subcutaneous_fat_pct\":" + String(bodyData.subcutFatPct, 1);
            j += ",\"fat_mass\":" + String(bodyData.fatMass, 1);
            j += ",\"fat_free_weight\":" + String(bodyData.fatFreeWeight, 1);
            j += ",\"muscle_mass\":" + String(bodyData.muscleMass, 1);
            j += ",\"protein_mass\":" + String(bodyData.proteinMass, 1);
        } else {
            j += ",\"body_fat_pct\":null";
            j += ",\"muscle_pct\":null";
            j += ",\"water_pct\":null";
            j += ",\"bone_mass\":null";
            j += ",\"protein_pct\":null";
            j += ",\"subcutaneous_fat_pct\":null";
            j += ",\"fat_mass\":null";
            j += ",\"fat_free_weight\":null";
            j += ",\"muscle_mass\":null";
            j += ",\"protein_mass\":null";
        }
    }
    j += "}";
    return j;
}

void forwardWeight(float weight, const char* time) {
    calculateBodyComposition(weight, time);
    saveBodyData();
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

#include "config_page.h"

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
  .section-label{color:#888;font-size:clamp(12px,1.5vw,14px);margin:18px 0 10px;text-align:center}
  .settings{margin-top:30px}
  .settings a{color:#888;font-size:clamp(13px,1.5vw,16px)}
  .no-profile{color:#888;font-size:14px;margin:20px 0}
  .toast{position:fixed;top:12px;left:12px;background:#1b3a1b;color:#4CAF50;padding:8px 14px;border-radius:8px;font-size:13px;opacity:0;transition:opacity 0.3s;pointer-events:none;z-index:99;border:1px solid #4CAF50}
  .toast.show{opacity:1}
</style>
</head><body>
<h1>OpenTrackFit</h1>
<div id="history-link" style="display:none;margin-top:4px"><a id="history-href" href="#" style="color:#888;font-size:clamp(13px,1.5vw,15px);text-decoration:none">zur History</a></div>
<div id="profile-name" style="color:#888;font-size:clamp(14px,2vw,18px);margin-top:6px"></div>
<div class="hero">
  <span class="val" id="weight">--.-</span> <span class="unit">kg</span>
</div>
<div class="meta" id="meta">Noch keine Messung</div>
<div id="tiles" style="display:none">
  <div class="grid">
    <div class="tile"><div class="label">BMI</div><div class="val" id="t_bmi">-</div></div>
    <div class="tile"><div class="label">Grundumsatz</div><div class="val" id="t_bmr">-</div><div class="unit">kcal</div></div>
    <div class="tile"><div class="label">Idealgewicht</div><div class="val" id="t_ideal">-</div><div class="unit">kg</div></div>
    <div class="tile"><div class="label">Differenz</div><div class="val" id="t_control">-</div><div class="unit">kg</div></div>
    <div class="tile"><div class="label">Stoffw.-Alter</div><div class="val" id="t_age">-</div></div>
    <div class="tile"><div class="label">Viszeralfett</div><div class="val" id="t_visceral">-</div></div>
  </div>
  <div class="section-label" id="bia-label">Koerperanalyse <span id="t_bia"></span></div>
  <div class="grid">
    <div class="tile"><div class="label">Koerperfett</div><div class="val" id="t_fat">-</div><div class="unit">%</div></div>
    <div class="tile"><div class="label">Muskelanteil</div><div class="val" id="t_muscle">-</div><div class="unit">%</div></div>
    <div class="tile"><div class="label">Koerperwasser</div><div class="val" id="t_water">-</div><div class="unit">%</div></div>
    <div class="tile"><div class="label">Knochenmasse</div><div class="val" id="t_bone">-</div><div class="unit">kg</div></div>
    <div class="tile"><div class="label">Protein</div><div class="val" id="t_protein">-</div><div class="unit">%</div></div>
    <div class="tile"><div class="label">Subkutan. Fett</div><div class="val" id="t_subcut">-</div><div class="unit">%</div></div>
    <div class="tile"><div class="label">Fettmasse</div><div class="val" id="t_fatmass">-</div><div class="unit">kg</div></div>
    <div class="tile"><div class="label">Fettfrei</div><div class="val" id="t_ffm">-</div><div class="unit">kg</div></div>
    <div class="tile"><div class="label">Muskelmasse</div><div class="val" id="t_musclekg">-</div><div class="unit">kg</div></div>
  </div>
</div>
<div id="no-profile" style="display:none;color:#888;font-size:14px;margin:20px 0;text-align:center">Profil in <a href="/setup" style="color:#4CAF50">Einstellungen</a> anlegen fuer Koerperanalyse</div>
<div class="settings"><a href="/setup">Einstellungen</a></div>
<div class="toast" id="toast"></div>
<script>
function f1(v){return v.toFixed(1)}
function v1(x){return x!==null?f1(x):'k.A.'}
var lastTime='';
function showToast(msg){
  var t=document.getElementById('toast');
  t.textContent=msg;t.classList.add('show');
  setTimeout(function(){t.classList.remove('show')},3000);
}
function load(){
  fetch('/api/last-weight-data').then(r=>r.json()).then(d=>{
    if(d.profile) document.getElementById('profile-name').textContent=d.profile;
    if(d.weight>0){
      document.getElementById('weight').textContent=f1(d.weight);
      document.getElementById('meta').textContent=d.time?'Letzte Messung: '+d.time:'Letzte Messung';
      if(d.time&&d.time!==lastTime){if(lastTime)showToast('Neue Messung: '+d.time);lastTime=d.time;}
      if(d.bmi){
        document.getElementById('tiles').style.display='block';
        document.getElementById('no-profile').style.display='none';
        document.getElementById('t_bmi').textContent=f1(d.bmi);
        document.getElementById('t_fat').textContent=v1(d.body_fat_pct);
        document.getElementById('t_muscle').textContent=v1(d.muscle_pct);
        document.getElementById('t_water').textContent=v1(d.water_pct);
        document.getElementById('t_bone').textContent=v1(d.bone_mass);
        document.getElementById('t_bmr').textContent=d.bmr;
        document.getElementById('t_protein').textContent=v1(d.protein_pct);
        document.getElementById('t_age').textContent=d.metabolic_age;
        document.getElementById('t_visceral').textContent=d.visceral_fat;
        document.getElementById('t_subcut').textContent=v1(d.subcutaneous_fat_pct);
        document.getElementById('t_ideal').textContent=f1(d.ideal_weight);
        document.getElementById('t_control').textContent=d.weight_control!==null?(d.weight_control>0?'+':'')+f1(d.weight_control):'k.A.';
        document.getElementById('t_fatmass').textContent=v1(d.fat_mass);
        document.getElementById('t_ffm').textContent=v1(d.fat_free_weight);
        document.getElementById('t_musclekg').textContent=v1(d.muscle_mass);
        var ba=document.getElementById('t_bia');
        var bl=document.getElementById('bia-label');
        if(d.body_analysis){ba.textContent='\u2714';ba.style.color='#4CAF50';bl.style.color='#4CAF50';}
        else{ba.textContent='\u2718';ba.style.color='#f44';bl.style.color='#888';}
      } else {
        document.getElementById('no-profile').style.display='block';
      }
    }
  }).catch(()=>{});
}
load();
setInterval(load,5000);
fetch('/api/settings').then(r=>r.json()).then(d=>{
  if(d.history_url){
    document.getElementById('history-href').href=d.history_url;
    document.getElementById('history-link').style.display='block';
  }
}).catch(()=>{});
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

void handleSaveHistory() {
    setPref("http", "history", server.arg("history_url"));
    Serial.println("History URL saved.");
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"History-App URL gespeichert.\"}");
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
        server.send(200, "application/json", "{\"weight\":0}");
    }
}

void handleApiSettings() {
    String json = "{";
    json += "\"mqtt_broker\":\"" + getPref("mqtt", "broker") + "\"";
    json += ",\"mqtt_port\":" + String(getPrefInt("mqtt", "port", 1883));
    json += ",\"mqtt_topic\":\"" + getPref("mqtt", "topic") + "\"";
    json += ",\"mqtt_user\":\"" + getPref("mqtt", "user") + "\"";
    json += ",\"http_webhook\":\"" + getPref("http", "webhook") + "\"";
    json += ",\"history_url\":\"" + getPref("http", "history") + "\"";
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
              "\"impedance\":\"BIA-Impedanz in Ohm (nur bei barfuss-Messung)\","
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
              "\"fat_free_weight\":\"Fettfreies Gewicht in kg\","
              "\"body_analysis\":\"true wenn BIA-Koerperanalyse verfuegbar, false wenn nicht (z.B. Socken)\""
            "},"
            "\"note\":\"Alle Felder ausser weight und time erfordern ein Profil. body_analysis zeigt ob BIA-Impedanzmessung erfolgreich war. BIA-abhaengige Felder (body_fat_pct, muscle_pct, water_pct, bone_mass, protein_pct, protein_mass, subcutaneous_fat_pct, fat_mass, fat_free_weight, muscle_mass) sind null wenn body_analysis=false. Immer verfuegbar mit Profil: bmi, bmr, metabolic_age, visceral_fat, ideal_weight, weight_control. impedance nur vorhanden bei barfuss-Messung.\""
          "},"
          "\"GET /api/settings\":{\"description\":\"Aktuelle Einstellungen und Profile\"},"
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
    server.on("/save/history-app", HTTP_POST, handleSaveHistory);
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
        if (weightReady) {
            weightReady = false;
            doForward = true;
        }
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
    // logRawData(source, data, length);  // uncomment for BLE packet hex dump

    if (length < 8 || data[0] != 0xAC || data[1] != 0x02) return;

    uint8_t type = data[6];

    uint8_t sum = 0;
    for (int i = 2; i <= 6; i++) sum += data[i];
    if ((uint8_t)sum != data[7]) return;

    if (type == PKT_LIVE || type == PKT_STABLE) {
        // Weight packet
        uint16_t raw = (data[2] << 8) | data[3];
        float weight = raw / 10.0f;

        if (type == PKT_STABLE) {
            finalWeight = weight;
            impedanceRaw = 0;  // reset for new measurement
            struct tm ti;
            if (getLocalTime(&ti, 0)) {
                strftime(finalWeightTime, sizeof(finalWeightTime), "%d.%m.%Y %H:%M:%S", &ti);
            } else {
                strcpy(finalWeightTime, "");
            }
            Serial.printf("\n>>> FINAL WEIGHT: %.1f kg <<<\n", weight);
            weightReady = true;
        } else {
            Serial.print(".");
        }
    } else if (type == 0xCB) {
        // Post-measurement CB packet
        if (data[2] == 0xFD && data[3] == 0x01) {
            // Impedance data packet — last CB packet for barefoot measurements
            uint16_t imp = (data[4] << 8) | data[5];
            if (imp > 0) {
                impedanceRaw = imp;
                Serial.printf(">>> IMPEDANCE: %d ohms <<<\n", imp);
            }
            // Forward now — FD 01 is the last relevant packet (barefoot)
            if (weightReady) {
                weightReady = false;
                doForward = true;
            }
        }
    } else if (type == 0xCC && weightReady) {
        // CC packet after CB sequence — all data collected, trigger forward
        weightReady = false;
        doForward = true;
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

    loadBodyData();
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

    // Forward weight (deferred from BLE disconnect to main loop)
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
