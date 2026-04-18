#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiEnterprise.h>
#include <HTTPClient.h>
#include "Adafruit_TCS34725.h"
#include <Adafruit_BMP280.h>
#include <DHTesp.h>

#define WIFI_SSID     ""
#define WIFI_USERNAME ""
#define WIFI_PASSWORD ""
#define MAX_RETRY     5

#define INFLUXDB_URL  "http://--.--.--.---:8086"
#define INFLUXDB_DB   "espy"
#define DEVICE_NAME   "esp32_R_69"

#define DHT_PIN  2
#define PIR_PIN  34

DHTesp dht;
Adafruit_BMP280 bmp;
Adafruit_TCS34725 tcs = Adafruit_TCS34725(
    TCS34725_INTEGRATIONTIME_50MS,
    TCS34725_GAIN_4X
);

// ── WiFi ──────────────────────────────────────────────────────────────────────
void connectToWiFi() {
    int attempt = 0;
    while (attempt < MAX_RETRY) {
        attempt++;
        Serial.printf("[WiFi] Attempt %d / %d ...\n", attempt, MAX_RETRY);
        if (WiFiEnterprise.begin(WIFI_SSID, WIFI_USERNAME, WIFI_PASSWORD)) {
            Serial.printf("[WiFi] Connected! IP: %s\n",
                          WiFiEnterprise.localIP().toString().c_str());
            return;
        }
        Serial.println("[WiFi] Attempt failed, retrying...");
        delay(500);
    }
    Serial.println("[WiFi] All attempts failed. Rebooting...");
    delay(1000);
    esp_restart();
}

void reconnectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.println("[WiFi] Lost connection, reconnecting...");
    connectToWiFi();
}

// ── InfluxDB ──────────────────────────────────────────────────────────────────
int failCount = 0;

void sendToInfluxDB(const String& payload) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[InfluxDB] WiFi disconnected, skipping.");
        return;
    }

    HTTPClient http;
    String url = String(INFLUXDB_URL) + "/write?db=" + INFLUXDB_DB + "&precision=s";

    Serial.println("[InfluxDB] " + payload);

    http.begin(url);
    http.addHeader("Content-Type", "application/octet-stream");
    int httpCode = http.POST(payload);

    if (httpCode == 204) {
        Serial.println("[InfluxDB] Write OK.");
        failCount = 0;
    } else {
        Serial.printf("[InfluxDB] Failed. HTTP %d: %s\n",
                      httpCode, http.getString().c_str());
        if (++failCount >= 10) {
            Serial.println("[InfluxDB] Too many failures, rebooting...");
            esp_restart();
        }
    }
    http.end();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);

    connectToWiFi();

    // PIR — GPIO34 is input-only, no pull needed
    pinMode(PIR_PIN, INPUT);
    Serial.println("PIR initialized.");

    // DHT22
    dht.setup(DHT_PIN, DHTesp::DHT22);
    Serial.println("DHT22 initialized.");

    // BMP280
    Wire.begin();
    if (!bmp.begin(0x76)) {
        Serial.println("BMP280 not at 0x76, trying 0x77...");
        if (!bmp.begin(0x77)) {
            Serial.println("BMP280 not found!");
            while (1);
        }
    }
    bmp.setSampling(
        Adafruit_BMP280::MODE_NORMAL,
        Adafruit_BMP280::SAMPLING_X2,
        Adafruit_BMP280::SAMPLING_X4,
        Adafruit_BMP280::FILTER_X4,
        Adafruit_BMP280::STANDBY_MS_2000
    );
    Serial.println("BMP280 initialized.");

    // TCS34725
    if (!tcs.begin()) {
        Serial.println("TCS34725 not found!");
        while (1);
    }
    Serial.println("TCS34725 initialized.");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    reconnectWiFi();

    // ── PIR (motion) ────────────────────────────────────────────────
    bool motion = false;
    unsigned long pirStart = millis();
    while (millis() - pirStart < 1000) {
    if (digitalRead(PIR_PIN) == HIGH) {
        motion = true;
        break;
    }
    delay(10);
}

Serial.printf("[PIR]     Motion: %s\n", motion ? "DETECTED" : "clear");

{
    String payload = String("motionR69,device=") + DEVICE_NAME +
                     " motion=" + (motion ? 1 : 0) + "i";
    sendToInfluxDB(payload);
}
    // ── DHT22 (indoor) ──────────────────────────────────────────────
    TempAndHumidity dhtVal = dht.getTempAndHumidity();

    if (dht.getStatus() != 0) {
        Serial.println("[DHT22] Error: " + String(dht.getStatusString()));
    } else {
        float heatIndex    = dht.computeHeatIndex(dhtVal.temperature, dhtVal.humidity);
        float dewPoint     = dht.computeDewPoint(dhtVal.temperature, dhtVal.humidity);
        ComfortState cf;
        float comfortRatio = dht.getComfortRatio(cf, dhtVal.temperature, dhtVal.humidity);

        Serial.printf("[Indoor]  Temp: %.1f °C  Hum: %.1f %%  HeatIdx: %.1f  DewPt: %.1f\n",
                      dhtVal.temperature, dhtVal.humidity, heatIndex, dewPoint);

        String payload = String("indoorR69,device=") + DEVICE_NAME +
                         " temperature=" + String(dhtVal.temperature, 2) +
                         ",humidity="    + String(dhtVal.humidity, 2) +
                         ",heat_index="  + String(heatIndex, 2) +
                         ",dew_point="   + String(dewPoint, 2) +
                         ",comfort="     + String(comfortRatio, 2);
        sendToInfluxDB(payload);
    }

    // ── BMP280 (outdoor) ────────────────────────────────────────────
    float outdoorTemp = bmp.readTemperature();
    float pressure    = bmp.readPressure() / 100.0F;
    float altitude    = bmp.readAltitude(1013.25);

    Serial.printf("[Outdoor] Temp: %.1f °C  Pressure: %.1f hPa  Alt: %.1f m\n",
                  outdoorTemp, pressure, altitude);

    {
        String payload = String("outdoorR69,device=") + DEVICE_NAME +
                         " temperature=" + String(outdoorTemp, 2) +
                         ",pressure="    + String(pressure, 2) +
                         ",altitude="    + String(altitude, 2);
        sendToInfluxDB(payload);
    }

    // ── TCS34725 (outdoor light) ────────────────────────────────────
    uint16_t r, g, b, c;
    tcs.getRawData(&r, &g, &b, &c);
    float lux       = tcs.calculateLux(r, g, b);
    float colorTemp = tcs.calculateColorTemperature_dn40(r, g, b, c);

    if (c >= 65535) Serial.println("[TCS] WARNING: Saturated!");

    Serial.printf("[Light]   R:%4d G:%4d B:%4d C:%4d  Lux:%.1f  CCT:%.0f K\n",
                  r, g, b, c, lux, colorTemp);

    {
        String payload = String("color_sensor,device=") + DEVICE_NAME +
                         " r="     + r +
                         ",g="     + g +
                         ",b="     + b +
                         ",clear=" + c +
                         ",lux="   + String(lux, 2) +
                         ",cct="   + String(colorTemp, 2);
        sendToInfluxDB(payload);
    }

    Serial.println("----------------------");
    delay(2000);
}
