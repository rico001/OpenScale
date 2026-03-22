#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include "config.h"

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

BLEScan* pBLEScan = nullptr;
bool deviceFound = false;
BLEAddress* pScaleAddress = nullptr;
BLEClient* pClient = nullptr;

// --- WiFi ---

void setupWiFi() {
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 40) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\nWiFi connection failed!");
    }
}

// --- MQTT ---

void setupMQTT() {
    if (strlen(MQTT_BROKER) == 0) return;
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    Serial.printf("MQTT broker set: %s:%d\n", MQTT_BROKER, MQTT_PORT);
}

void reconnectMQTT() {
    if (strlen(MQTT_BROKER) == 0) return;
    if (mqttClient.connected()) return;

    Serial.println("Connecting to MQTT...");
    const char* clientId = "OpenTrackFit-ESP32";
    bool connected;
    if (strlen(MQTT_USER) > 0) {
        connected = mqttClient.connect(clientId, MQTT_USER, MQTT_PASSWORD);
    } else {
        connected = mqttClient.connect(clientId);
    }
    if (connected) {
        Serial.println("MQTT connected.");
    } else {
        Serial.printf("MQTT connection failed, rc=%d\n", mqttClient.state());
    }
}

// --- Data forwarding ---

void forwardData(const String& payload) {
    Serial.printf("Scale data: %s\n", payload.c_str());

    // MQTT
    if (strlen(MQTT_BROKER) > 0 && mqttClient.connected()) {
        mqttClient.publish(MQTT_TOPIC, payload.c_str());
        Serial.println("Published to MQTT.");
    }

    // HTTP POST
    if (strlen(HTTP_CALLBACK_URL) > 0 && WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(HTTP_CALLBACK_URL);
        http.addHeader("Content-Type", "application/json");
        int httpCode = http.POST(payload);
        Serial.printf("HTTP POST response: %d\n", httpCode);
        http.end();
    }
}

// --- BLE ---

class ScaleAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        Serial.printf("BLE Device found: %s\n", advertisedDevice.toString().c_str());
        if (advertisedDevice.getName() == SCALE_BLE_NAME) {
            Serial.println("Scale found!");
            pScaleAddress = new BLEAddress(advertisedDevice.getAddress());
            deviceFound = true;
            pBLEScan->stop();
        }
    }
};

void setupBLE() {
    BLEDevice::init("OpenTrackFit");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new ScaleAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
}

void scanForScale() {
    if (deviceFound) return;
    Serial.println("Scanning for BLE scale...");
    pBLEScan->start(10, false);
    pBLEScan->clearResults();
}

// --- Main ---

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== OpenTrackFit ESP32 ===");

    setupWiFi();
    setupMQTT();
    setupBLE();
}

void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        reconnectMQTT();
        mqttClient.loop();
    }

    if (!deviceFound) {
        scanForScale();
    }

    // TODO: Connect to scale, subscribe to notifications,
    //       parse weight data, call forwardData()

    delay(1000);
}
