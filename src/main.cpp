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
#include <Update.h>
#include "build_info.h"

// --- Config ---
#define SCALE_NAME      "FitTrack"
#define AP_SSID         "OpenScale"
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
unsigned long lastScanEnd = 0;

volatile float finalWeight = 0;
char finalWeightTime[20] = "";
volatile bool doForward = false;
volatile bool weightReady = false;   // set after CA (stable weight), cleared after forward
volatile uint16_t impedanceRaw = 0;  // BIA impedance from CB packets (ohms)

// --- Sync Status ---
char lastMqttSync[20] = "";
bool lastMqttOk = false;
char lastHttpSync[20] = "";
bool lastHttpOk = false;

// --- Buzzer ---
int buzzerPin = -1;

String activeProfileName = "";

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
    char profile[20];
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

void setupBuzzer() {
    int pin = getPrefInt("hw", "buzzer", 0);
    if (pin > 0) {
        buzzerPin = pin;
        pinMode(buzzerPin, OUTPUT);
        digitalWrite(buzzerPin, LOW);
        Serial.printf("Buzzer on GPIO %d\n", buzzerPin);
    } else {
        buzzerPin = -1;
    }
}

void beep() {
    if (buzzerPin < 0) return;
    digitalWrite(buzzerPin, HIGH);
    delay(300);
    digitalWrite(buzzerPin, LOW);
}

void saveBodyData() {
    prefs.begin("body", false);
    prefs.putBytes("data", &bodyData, sizeof(bodyData));
    prefs.end();
}

void loadBodyData() {
    prefs.begin("body", true);
    size_t stored = prefs.getBytesLength("data");
    if (stored == sizeof(bodyData)) {
        prefs.getBytes("data", &bodyData, sizeof(bodyData));
        if (bodyData.weight > 0) {
            activeProfileName = bodyData.profile;
            Serial.printf("Restored last measurement: %.1f kg (%s)\n", bodyData.weight, bodyData.time);
        }
    } else if (stored > 0) {
        // Struct size changed (e.g. profile field added) — clear stale data
        prefs.end();
        prefs.begin("body", false);
        prefs.clear();
    }
    prefs.end();
}

// --- Multi-Profile Storage ---
// NVS namespace "prof", max 8 profiles
// Keys: "count", "active", "n0".."n7" (name), "g0".."g7" (gender), "a0".."a7" (age), "h0".."h7" (height)
#define MAX_PROFILES 8

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

// --- Auto-Profile Selection ---
// Selects profile by weight range before body composition calculation

void autoSelectProfile(float weight) {
    if (getPrefInt("aprof", "enabled", 0) != 1) return;
    int ruleCount = getPrefInt("aprof", "count", 0);
    int profileCount = getProfileCount();
    if (profileCount == 0) return;

    String matchName = "";
    for (int i = 0; i < ruleCount; i++) {
        float mn = getPrefInt("aprof", ("l" + String(i)).c_str(), 0) / 10.0f;
        float mx = getPrefInt("aprof", ("h" + String(i)).c_str(), 0) / 10.0f;
        String prof = getPref("aprof", ("p" + String(i)).c_str());
        if (prof.isEmpty()) continue;
        if (mn > 0 && weight < mn) continue;
        if (mx > 0 && weight > mx) continue;
        matchName = prof;
        break;
    }

    if (matchName.isEmpty()) return; // keep currently active profile as default

    // Find profile index by name (case-insensitive) and activate it
    matchName.toLowerCase();
    for (int i = 0; i < profileCount; i++) {
        String profName = getPref("prof", ("n" + String(i)).c_str());
        profName.toLowerCase();
        if (profName == matchName) {
            if (i != getActiveIndex()) {
                setPrefInt("prof", "active", i);
                Serial.printf("Auto-profile: selected '%s' for %.1f kg\n", matchName.c_str(), weight);
            }
            return;
        }
    }
    Serial.printf("Auto-profile: '%s' not found\n", matchName.c_str());
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
    strncpy(bodyData.profile, prof.name.c_str(), sizeof(bodyData.profile) - 1);
    bodyData.profile[sizeof(bodyData.profile) - 1] = '\0';

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
    beep();
    autoSelectProfile(weight);
    calculateBodyComposition(weight, time);
    saveBodyData();
    String json = buildBodyJson();

    // MQTT
    String broker = getPref("mqtt", "broker");
    if (!broker.isEmpty()) {
        String topic = getPref("mqtt", "topic");
        if (topic.isEmpty()) topic = "openscale/weight";
        uint16_t port = getPrefInt("mqtt", "port", 1883);
        String user = getPref("mqtt", "user");
        String pass = getPref("mqtt", "pass");

        bool retain = getPrefInt("mqtt", "retain", 0) == 1;
        mqttClient.setServer(broker.c_str(), port);
        mqttClient.setBufferSize(1024);
        lastMqttOk = false;
        for (int attempt = 1; attempt <= 3; attempt++) {
            bool ok;
            if (!user.isEmpty()) {
                ok = mqttClient.connect("OpenScale", user.c_str(), pass.c_str());
            } else {
                ok = mqttClient.connect("OpenScale");
            }
            if (ok) {
                if (mqttClient.publish(topic.c_str(), json.c_str(), retain)) {
                    Serial.printf("MQTT published to %s (%d bytes)\n", topic.c_str(), json.length());
                    lastMqttOk = true;
                } else {
                    Serial.printf("MQTT publish failed (payload %d bytes)\n", json.length());
                }
                mqttClient.disconnect();
                break;
            }
            Serial.printf("MQTT connect failed (rc=%d), attempt %d/3\n", mqttClient.state(), attempt);
            if (attempt < 3) delay(500);
        }
        struct tm ti;
        if (getLocalTime(&ti)) strftime(lastMqttSync, sizeof(lastMqttSync), "%d.%m.%Y %H:%M:%S", &ti);
        else strcpy(lastMqttSync, time);
    }

    // HTTP Webhook
    String webhook = getPref("http", "webhook");
    if (!webhook.isEmpty() && WiFi.status() == WL_CONNECTED) {
        lastHttpOk = false;
        for (int attempt = 1; attempt <= 3; attempt++) {
            HTTPClient http;
            http.begin(webhook);
            http.addHeader("Content-Type", "application/json");
            int code = http.POST(json);
            Serial.printf("HTTP POST %s -> %d, attempt %d/3\n", webhook.c_str(), code, attempt);
            http.end();
            if (code >= 200 && code < 300) {
                lastHttpOk = true;
                break;
            }
            if (attempt < 3) delay(500);
        }
        struct tm ti;
        if (getLocalTime(&ti)) strftime(lastHttpSync, sizeof(lastHttpSync), "%d.%m.%Y %H:%M:%S", &ti);
        else strcpy(lastHttpSync, time);
    }
}

// --- HTML ---

#include "config_page.h"

const char WEIGHT_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenScale</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 75.22 75.22' fill='%234CAF50'><path d='M67.104,0H8.117C3.635,0,0,3.634,0,8.117v58.987c0,4.481,3.635,8.116,8.117,8.116h58.987c4.482,0,8.116-3.635,8.116-8.116V8.117C75.22,3.633,71.587,0,67.104,0z M53.922,8.454l-6.035,8.955c-3.585-2.416-7.059-3.207-10.085-3.199l-4.335-7.348l-1.068,0.579l3.419,6.89c-4.787,0.561-8.05,2.904-8.105,2.945l-6.412-8.688C21.939,8.115,37.156-2.845,53.922,8.454z M66.089,58.496c0,4.482-3.634,8.117-8.117,8.117H17.114c-4.483,0-8.117-3.635-8.117-8.117V29.831c0-4.483,3.634-8.117,8.117-8.117h40.857c4.483,0,8.117,3.634,8.117,8.117L66.089,58.496z'/></svg>">
<style>
  *{box-sizing:border-box}
  body{font-family:sans-serif;max-width:600px;margin:0 auto;padding:0 16px 30px;background:#111;color:#eee;text-align:center}
  .status-bar{display:flex;align-items:center;justify-content:center;gap:10px;padding:6px 16px;font-size:11px;color:#555;border-bottom:1px solid #222;margin:0 -16px 0;position:relative}
  .gear{position:absolute;right:16px;color:#555;text-decoration:none;font-size:25px;line-height:1}
  .gear:hover{color:#888}
  .status-bar .ble{display:flex;align-items:center;gap:4px}
  .status-bar .dot{width:6px;height:6px;border-radius:50%;flex-shrink:0;transition:background 0.3s}
  .status-bar .dot.connected{background:#4CAF50}
  .status-bar .dot.scanning{background:#FFC107}
  .status-bar .dot.disconnected{background:#f44336}
  .status-bar .sep{color:#333}
  .status-bar .sync{color:#555}
  .status-bar .sync .ok{color:#4CAF50}
  .status-bar .sync .fail{color:#f44336}
  .status-bar.warn{background:#2a2211;border-bottom-color:#FFC107}
  .status-bar.offline{background:#2a1111;border-bottom-color:#f44336}
  h1{color:#4CAF50;margin-bottom:0;font-size:20px;margin-top:20px}
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
  .no-profile{color:#888;font-size:14px;margin:20px 0}
  .toast{position:fixed;top:12px;left:12px;background:#1b3a1b;color:#4CAF50;padding:8px 14px;border-radius:8px;font-size:13px;opacity:0;transition:opacity 0.3s;pointer-events:none;z-index:99;border:1px solid #4CAF50}
  .toast.show{opacity:1}
</style>
</head><body>
<div class="status-bar" id="status-bar"><span class="ble"><span class="dot disconnected" id="ble-dot"></span><span id="ble-text">Waage</span></span><span id="sync-info"></span><a href="/setup" class="gear">&#9881;</a></div>
<h1><svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 75.22 75.22" fill="#4CAF50" style="width:20px;height:20px;vertical-align:-2px;margin-right:6px;transform:rotate(340deg)"><path d="M67.104,0H8.117C3.635,0,0,3.634,0,8.117v58.987c0,4.481,3.635,8.116,8.117,8.116h58.987c4.482,0,8.116-3.635,8.116-8.116V8.117C75.22,3.633,71.587,0,67.104,0z M53.922,8.454l-6.035,8.955c-3.585-2.416-7.059-3.207-10.085-3.199l-4.335-7.348l-1.068,0.579l3.419,6.89c-4.787,0.561-8.05,2.904-8.105,2.945l-6.412-8.688C21.939,8.115,37.156-2.845,53.922,8.454z M66.089,58.496c0,4.482-3.634,8.117-8.117,8.117H17.114c-4.483,0-8.117-3.635-8.117-8.117V29.831c0-4.483,3.634-8.117,8.117-8.117h40.857c4.483,0,8.117,3.634,8.117,8.117L66.089,58.496z"/></svg>OpenScale</h1>
<div id="history-link" style="display:none;margin-top:4px"><a id="history-href" href="#" style="color:#888;font-size:clamp(13px,1.5vw,15px);text-decoration:none">zur History</a></div>
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
  <div class="section-label" id="bia-label">Körperanalyse <span id="t_bia"></span></div>
  <div class="grid">
    <div class="tile"><div class="label">Körperfett</div><div class="val" id="t_fat">-</div><div class="unit">%</div></div>
    <div class="tile"><div class="label">Muskelanteil</div><div class="val" id="t_muscle">-</div><div class="unit">%</div></div>
    <div class="tile"><div class="label">Körperwasser</div><div class="val" id="t_water">-</div><div class="unit">%</div></div>
    <div class="tile"><div class="label">Knochenmasse</div><div class="val" id="t_bone">-</div><div class="unit">kg</div></div>
    <div class="tile"><div class="label">Protein</div><div class="val" id="t_protein">-</div><div class="unit">%</div></div>
    <div class="tile"><div class="label">Subkutan. Fett</div><div class="val" id="t_subcut">-</div><div class="unit">%</div></div>
    <div class="tile"><div class="label">Fettmasse</div><div class="val" id="t_fatmass">-</div><div class="unit">kg</div></div>
    <div class="tile"><div class="label">Fettfrei</div><div class="val" id="t_ffm">-</div><div class="unit">kg</div></div>
    <div class="tile"><div class="label">Muskelmasse</div><div class="val" id="t_musclekg">-</div><div class="unit">kg</div></div>
  </div>
</div>
<div id="no-profile" style="display:none;color:#888;font-size:14px;margin:20px 0;text-align:center">Profil in <a href="/setup" style="color:#4CAF50">Einstellungen</a> anlegen für Körperanalyse</div>
<div class="toast" id="toast"></div>
<p style="text-align:center;margin-top:20px;color:#555;font-size:11px" id="build-info"></p>
<script>
function fetchT(url){var c=new AbortController();setTimeout(function(){c.abort()},3000);return fetch(url,{signal:c.signal})}
function f1(v){return v.toFixed(1)}
function v1(x){return x!==null?f1(x):'k.A.'}
var lastTime='';
function showToast(msg){
  var t=document.getElementById('toast');
  t.textContent=msg;t.classList.add('show');
  setTimeout(function(){t.classList.remove('show')},3000);
}
function load(){
  fetchT('/api/last-weight-data').then(r=>r.json()).then(d=>{
    if(d.weight>0){
      document.getElementById('weight').textContent=f1(d.weight);
      document.getElementById('meta').textContent=d.time?'Letzte Messung: '+(d.profile?d.profile+', ':'')+d.time:'Letzte Messung';
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
var failCount=0;
function setOffline(){
  failCount++;
  var bar=document.getElementById('status-bar');
  var dot=document.getElementById('ble-dot');
  var txt=document.getElementById('ble-text');
  if(failCount>6){bar.className='status-bar offline';dot.className='dot disconnected';txt.textContent='Offline';}
  else if(failCount>3){bar.className='status-bar warn';dot.className='dot scanning';txt.textContent='Verbindungsproblem';}
  else{bar.className='status-bar';return;}
  document.getElementById('sync-info').innerHTML='';
}
function loadStatus(){
  fetchT('/api/status').then(function(r){if(!r.ok)throw 0;return r.json()}).then(d=>{
    failCount=0;
    var bar=document.getElementById('status-bar');
    bar.className='status-bar';
    var dot=document.getElementById('ble-dot');
    var txt=document.getElementById('ble-text');
    dot.className='dot '+(d.ble_connected?'connected':d.ble_scanning?'scanning':'disconnected');
    txt.textContent=d.ble_connected?'Waage verbunden':d.ble_scanning?'Suche Waage...':'Waage getrennt';
    var si=document.getElementById('sync-info');
    var parts=[];
    if(d.mqtt_configured){
      if(d.mqtt_last_sync){var c=d.mqtt_last_ok?'ok':'fail';parts.push('<span class="sync"><span class="'+c+'">\u2022</span> MQTT '+d.mqtt_last_sync.slice(11,16)+'</span>');}
      else parts.push('<span class="sync">\u2022 MQTT</span>');
    }
    if(d.http_configured){
      if(d.http_last_sync){var c=d.http_last_ok?'ok':'fail';parts.push('<span class="sync"><span class="'+c+'">\u2022</span> HTTP '+d.http_last_sync.slice(11,16)+'</span>');}
      else parts.push('<span class="sync">\u2022 HTTP</span>');
    }
    si.innerHTML=parts.length?'<span class="sep">|</span> '+parts.join(' '):'';
  }).catch(setOffline);
}
load();loadStatus();
setInterval(load,5000);setInterval(loadStatus,3000);
fetchT('/api/settings').then(r=>r.json()).then(d=>{
  if(d.history_url){
    document.getElementById('history-href').href=d.history_url;
    document.getElementById('history-link').style.display='block';
  }
  if(d.build_time) document.getElementById('build-info').textContent='Build: '+d.build_time;
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
            "<p>mDNS: <strong>http://openscale.local</strong></p>"
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
            "<p>Bitte SSID und Passwort prüfen.</p>"
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
    setPrefInt("mqtt", "retain", server.hasArg("retain") ? 1 : 0);
    Serial.println("MQTT settings saved.");
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"MQTT Einstellungen gespeichert.\"}");
}

void handleSaveHttp() {
    setPref("http", "webhook", server.arg("webhook"));
    Serial.println("HTTP webhook saved.");
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"Webhook gespeichert.\"}");
}

void handleSaveBuzzer() {
    String pin = server.arg("buzzer_pin");
    uint16_t val = pin.isEmpty() ? 0 : pin.toInt();
    setPrefInt("hw", "buzzer", val);
    setupBuzzer();
    if (buzzerPin > 0) beep();
    Serial.printf("Buzzer pin saved: %d\n", val);
    server.send(200, "application/json", val > 0
        ? "{\"ok\":true,\"message\":\"Buzzer gespeichert. Testton ausgegeben.\"}"
        : "{\"ok\":true,\"message\":\"Buzzer deaktiviert.\"}");
}

void handleSaveHistory() {
    setPref("http", "history", server.arg("history_url"));
    Serial.println("History URL saved.");
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"History-App URL gespeichert.\"}");
}

void handleSaveAutoProfile() {
    setPrefInt("aprof", "enabled", server.hasArg("enabled") ? 1 : 0);

    int count = 0;
    for (int i = 0; i < 8; i++) {
        String prof = server.arg("r" + String(i) + "_prof");
        String minS = server.arg("r" + String(i) + "_min");
        String maxS = server.arg("r" + String(i) + "_max");
        if (prof.isEmpty()) continue;
        uint16_t mn = minS.isEmpty() ? 0 : (uint16_t)(minS.toFloat() * 10);
        uint16_t mx = maxS.isEmpty() ? 0 : (uint16_t)(maxS.toFloat() * 10);
        setPref("aprof", ("p" + String(count)).c_str(), prof);
        setPrefInt("aprof", ("l" + String(count)).c_str(), mn);
        setPrefInt("aprof", ("h" + String(count)).c_str(), mx);
        count++;
    }
    // Clear old slots beyond new count
    int oldCount = getPrefInt("aprof", "count", 0);
    for (int i = count; i < oldCount; i++) {
        setPref("aprof", ("p" + String(i)).c_str(), "");
        setPrefInt("aprof", ("l" + String(i)).c_str(), 0);
        setPrefInt("aprof", ("h" + String(i)).c_str(), 0);
    }
    setPrefInt("aprof", "count", count);
    Serial.printf("Auto-profile saved: %d rules, enabled=%d\n", count, server.hasArg("enabled") ? 1 : 0);
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"Auto-Profil gespeichert.\"}");
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
            server.send(200, "application/json", "{\"ok\":true,\"message\":\"Profil gelöscht.\"}");
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
    json += ",\"mqtt_retain\":";
    json += getPrefInt("mqtt", "retain", 0) ? "true" : "false";
    json += ",\"http_webhook\":\"" + getPref("http", "webhook") + "\"";
    json += ",\"history_url\":\"" + getPref("http", "history") + "\"";
    json += ",\"buzzer_pin\":" + String(getPrefInt("hw", "buzzer", 0));
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
    // Auto-profile
    json += ",\"auto_profile_enabled\":";
    json += getPrefInt("aprof", "enabled", 0) ? "true" : "false";
    int apCount = getPrefInt("aprof", "count", 0);
    json += ",\"auto_profile_rules\":[";
    for (int i = 0; i < apCount; i++) {
        if (i > 0) json += ",";
        float mn = getPrefInt("aprof", ("l" + String(i)).c_str(), 0) / 10.0f;
        float mx = getPrefInt("aprof", ("h" + String(i)).c_str(), 0) / 10.0f;
        String prof = getPref("aprof", ("p" + String(i)).c_str());
        json += "{\"min\":" + String(mn, 1) + ",\"max\":" + String(mx, 1) + ",\"profile\":\"" + prof + "\"}";
    }
    json += "]";
    json += ",\"build_time\":\"" + String(BUILD_TIME) + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void handleApiStatus() {
    String json = "{\"ble_connected\":";
    json += connected ? "true" : "false";
    json += ",\"ble_scanning\":";
    json += scanning ? "true" : "false";
    // MQTT sync
    json += ",\"mqtt_configured\":";
    json += !getPref("mqtt", "broker").isEmpty() ? "true" : "false";
    if (lastMqttSync[0]) {
        json += ",\"mqtt_last_sync\":\"" + String(lastMqttSync) + "\"";
        json += ",\"mqtt_last_ok\":";
        json += lastMqttOk ? "true" : "false";
    }
    // HTTP sync
    json += ",\"http_configured\":";
    json += !getPref("http", "webhook").isEmpty() ? "true" : "false";
    if (lastHttpSync[0]) {
        json += ",\"http_last_sync\":\"" + String(lastHttpSync) + "\"";
        json += ",\"http_last_ok\":";
        json += lastHttpOk ? "true" : "false";
    }
    json += "}";
    server.send(200, "application/json", json);
}

void handleApiDocs() {
    server.send(200, "application/json",
        "{"
        "\"name\":\"OpenScale API\","
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
              "\"body_fat_pct\":\"Körperfettanteil in %\","
              "\"muscle_pct\":\"Muskelanteil in %\","
              "\"muscle_mass\":\"Muskelmasse in kg\","
              "\"water_pct\":\"Körperwasseranteil in %\","
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
          "\"GET /api/status\":{\"description\":\"System-Status (BLE, Sync)\",\"fields\":{\"ble_connected\":\"true wenn mit Waage verbunden\",\"ble_scanning\":\"true wenn nach Waage gesucht wird\",\"mqtt_configured\":\"true wenn MQTT konfiguriert\",\"mqtt_last_sync\":\"Zeitpunkt letzter MQTT-Sync\",\"mqtt_last_ok\":\"true wenn letzter MQTT-Sync erfolgreich\",\"http_configured\":\"true wenn HTTP-Webhook konfiguriert\",\"http_last_sync\":\"Zeitpunkt letzter HTTP-Sync\",\"http_last_ok\":\"true wenn letzter HTTP-Sync erfolgreich\"}},"
          "\"GET /api/settings\":{\"description\":\"Aktuelle Einstellungen und Profile\"},"
          "\"GET /api/docs\":{\"description\":\"Diese API-Dokumentation\"}"
        "}"
        "}");
}

// --- OTA Update ---
void handleOtaUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("OTA Update: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("OTA Update erfolgreich: %u bytes\n", upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

void handleOtaResult() {
    if (Update.hasError()) {
        server.send(200, "application/json", "{\"ok\":false,\"message\":\"Update fehlgeschlagen!\"}");
    } else {
        server.send(200, "application/json", "{\"ok\":true,\"message\":\"Update erfolgreich! Neustart...\"}");
        delay(1000);
        ESP.restart();
    }
}

void setupWebServer() {
    server.on("/", handleRoot);
    server.on("/setup", handleSetup);
    server.on("/save/wifi", HTTP_POST, handleSaveWifi);
    server.on("/save/mqtt", HTTP_POST, handleSaveMqtt);
    server.on("/save/http", HTTP_POST, handleSaveHttp);
    server.on("/save/buzzer", HTTP_POST, handleSaveBuzzer);
    server.on("/save/history-app", HTTP_POST, handleSaveHistory);
    server.on("/save/profile", HTTP_POST, handleSaveProfile);
    server.on("/save/auto-profile", HTTP_POST, handleSaveAutoProfile);
    server.on("/delete/profile", HTTP_POST, handleDeleteProfile);
    server.on("/api/set-profile", HTTP_POST, handleSetActiveProfile);
    server.on("/api/scan", handleApiScan);
    server.on("/api/last-weight-data", handleApiWeight);
    server.on("/api/settings", handleApiSettings);
    server.on("/api/status", handleApiStatus);
    server.on("/api/docs", handleApiDocs);
    server.on("/ota", HTTP_POST, handleOtaResult, handleOtaUpload);
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
    BLEDevice::init("OpenScale");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), false);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(200);
    pBLEScan->setWindow(80);
}

// --- Main ---

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== OpenScale ===");

    loadBodyData();
    setupBuzzer();
    setupBLE();

    if (connectWiFi()) {
        currentMode = MODE_STA;
        MDNS.begin("openscale");
        setupWebServer();
        Serial.println("-----------------------------");
        Serial.println("Mode:  LAN (Station)");
        Serial.printf("IP:    %s\n", WiFi.localIP().toString().c_str());
        Serial.println("mDNS:  http://openscale.local");
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
            MDNS.begin("openscale");
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

    // BLE connect (retry up to 3 times before falling back to scan)
    if (doConnect && !connected) {
        bool ok = false;
        for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
            if (attempt > 1) {
                Serial.printf("Retry %d/3...\n", attempt);
                delay(200);
            }
            ok = connectToScale();
        }
        doConnect = false;
    }

    // BLE non-blocking scan (2s pause between scans for WiFi stability)
    if (!connected && !doConnect && !scanning && millis() - lastScanEnd > 2000) {
        pBLEScan->clearResults();
        pBLEScan->start(5, [](BLEScanResults) { scanning = false; lastScanEnd = millis(); }, false);
        scanning = true;
    }

    delay(10);
}
