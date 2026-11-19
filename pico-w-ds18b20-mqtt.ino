/*
 * Pico W DS18B20 Temperature Node
 * --------------------------------
 * Reads one or more DS18B20 1-Wire temperature sensors from a Raspberry Pi
 * Pico W and publishes the readings to an MQTT broker every 30 seconds as a
 * small JSON payload. Uses the Arduino-Pico core (earlephilhower).
 *
 * Why this sketch: the DS18B20 is the cheapest waterproof-capable digital
 * thermometer you can buy (PHP 40-80 for the probe version), and the Pico W is
 * the cheapest WiFi board with a real Raspberry Pi ecosystem. Together they
 * make a PHP 400 temperature logger for aquaculture tanks, fridges, server
 * closets, compost piles, or any thesis rig that needs more than one probe.
 *
 * What it demonstrates:
 *   - 1-Wire on RP2040 with the OneWire + DallasTemperature libraries
 *   - NON-BLOCKING conversions: we kick off a conversion, keep servicing
 *     mqtt.loop(), and only read the result ~750 ms later. The DS18B20 takes
 *     up to 750 ms at 12-bit resolution; the naive requestTemperatures() call
 *     blocks for all of it and makes MQTT keepalives flaky.
 *   - Multiple sensors on one data pin, each identified by its 64-bit ROM
 *     address, so you can tell "tank inlet" from "tank outlet".
 *   - Reconnecting WiFi + MQTT without blocking forever, with a retained
 *     online/offline status topic (MQTT Last Will) so a dashboard notices
 *     when the node dies.
 *
 * Required libraries (install via Library Manager):
 *   - "OneWire" by Jim Studt / Paul Stoffregen
 *   - "DallasTemperature" by Miles Burton
 *   - "PubSubClient" by Nick O'Leary
 *
 * Board setup:
 *   1. In Arduino IDE, File -> Preferences -> Additional Boards Manager URLs,
 *      add: https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
 *   2. Boards Manager -> search "Raspberry Pi Pico/RP2040" (earlephilhower)
 *      and install.
 *   3. Tools -> Board -> Raspberry Pi Pico W.
 *
 * Wiring:
 *   DS18B20 VDD (red)    -> Pico 3V3 (pin 36)
 *   DS18B20 GND (black)  -> Pico GND
 *   DS18B20 DATA (yellow)-> Pico GP22 (pin 29)
 *   4.7 kOhm resistor between DATA and 3V3 (one resistor for the whole bus)
 *
 * Setup:
 *   1. Copy config.h.example to config.h
 *   2. Fill in WiFi + MQTT broker details.
 *   3. Flash. Open Serial Monitor at 115200.
 *
 * Published to MQTT_TOPIC every PUBLISH_INTERVAL_MS:
 *   {"temp_c":27.31,"sensor":"28FF641E8D1C0123","sensors":2,"uptime_s":120}
 * With more than one probe, each one is also published to MQTT_TOPIC/<index>:
 *   {"temp_c":27.31,"sensor":"28FF641E8D1C0123","index":0}
 *
 * (c) 2026 Fluidwire. Released under the MIT License.
 * https://github.com/fluidwire
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "config.h"

// --- Pins & timing ----------------------------------------------------------
const uint8_t  ONE_WIRE_PIN        = 22;              // GP22, physical pin 29
const uint8_t  DS18B20_RESOLUTION  = 12;              // 9..12 bits (12 = 0.0625 C, 750 ms)
const uint32_t PUBLISH_INTERVAL_MS = 30 * 1000UL;
const uint32_t WIFI_RETRY_MS       = 10 * 1000UL;
const uint32_t MQTT_RETRY_MS       = 5 * 1000UL;
const uint8_t  MAX_SENSORS         = 4;

// --- Objects ----------------------------------------------------------------
OneWire           oneWire(ONE_WIRE_PIN);
DallasTemperature sensors(&oneWire);
WiFiClient        wifiClient;
PubSubClient      mqtt(wifiClient);

// --- State ------------------------------------------------------------------
DeviceAddress sensorAddr[MAX_SENSORS];
uint8_t  sensorCount         = 0;
uint16_t conversionMs        = 750;
bool     conversionPending   = false;
uint32_t conversionStartedAt = 0;
uint32_t lastPublish         = 0;
uint32_t lastWifiAttempt     = 0;
uint32_t lastMqttAttempt     = 0;
uint32_t publishCount        = 0;

// --- Helpers ----------------------------------------------------------------
void addressToHex(const DeviceAddress addr, char *out /* >= 17 bytes */) {
  static const char hex[] = "0123456789ABCDEF";
  for (uint8_t i = 0; i < 8; i++) {
    out[i * 2]     = hex[addr[i] >> 4];
    out[i * 2 + 1] = hex[addr[i] & 0x0F];
  }
  out[16] = '\0';
}

void scanSensors() {
  sensors.begin();
  sensors.setWaitForConversion(false);          // the whole point of this sketch
  sensors.setResolution(DS18B20_RESOLUTION);
  conversionMs = sensors.millisToWaitForConversion(DS18B20_RESOLUTION);

  uint8_t found = sensors.getDeviceCount();
  sensorCount = 0;
  Serial.print(F("[1wire] devices on bus: "));
  Serial.println(found);

  for (uint8_t i = 0; i < found && sensorCount < MAX_SENSORS; i++) {
    if (sensors.getAddress(sensorAddr[sensorCount], i)) {
      char hex[17];
      addressToHex(sensorAddr[sensorCount], hex);
      Serial.print(F("[1wire]   #"));
      Serial.print(sensorCount);
      Serial.print(F(" = "));
      Serial.println(hex);
      sensorCount++;
    }
  }
  if (sensorCount == 0) {
    Serial.println(F("[1wire] no DS18B20 found - check wiring and the 4.7k pull-up"));
  }
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  uint32_t now = millis();
  if (lastWifiAttempt != 0 && now - lastWifiAttempt < WIFI_RETRY_MS) return;
  lastWifiAttempt = now;

  Serial.print(F("[wifi] connecting to "));
  Serial.print(WIFI_SSID);
  Serial.print(F(" ... "));
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // arduino-pico's begin() may already block briefly; give it a few seconds,
  // then let loop() carry on and retry later instead of hanging forever.
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("OK, ip="));
    Serial.println(WiFi.localIP());
  } else {
    Serial.print(F("not yet (status="));
    Serial.print(WiFi.status());
    Serial.println(F("), will retry"));
  }
}

void ensureMQTT() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  uint32_t now = millis();
  if (lastMqttAttempt != 0 && now - lastMqttAttempt < MQTT_RETRY_MS) return;
  lastMqttAttempt = now;

  Serial.print(F("[mqtt] connecting to "));
  Serial.print(MQTT_HOST);
  Serial.print(F(":"));
  Serial.print(MQTT_PORT);
  Serial.print(F(" as "));
  Serial.print(MQTT_CLIENT_ID);
  Serial.print(F(" ... "));

  const char *user = strlen(MQTT_USER) > 0 ? MQTT_USER : nullptr;
  const char *pass = strlen(MQTT_USER) > 0 ? MQTT_PASSWORD : nullptr;

  // Last Will: broker publishes "offline" (retained) if we drop off.
  bool ok = mqtt.connect(MQTT_CLIENT_ID, user, pass,
                         MQTT_STATUS_TOPIC, 0, true, "offline");
  if (ok) {
    Serial.println(F("OK"));
    mqtt.publish(MQTT_STATUS_TOPIC, "online", true);
  } else {
    Serial.print(F("FAIL rc="));
    Serial.println(mqtt.state());
  }
}

void startConversion() {
  sensors.requestTemperatures();          // returns immediately (non-blocking)
  conversionStartedAt = millis();
  conversionPending   = true;
}

void publishReadings() {
  uint32_t uptimeS = millis() / 1000UL;
  bool primarySent = false;

  for (uint8_t i = 0; i < sensorCount; i++) {
    float tempC = sensors.getTempC(sensorAddr[i]);
    char hex[17];
    addressToHex(sensorAddr[i], hex);

    if (tempC == DEVICE_DISCONNECTED_C || tempC < -55.0f || tempC > 125.0f) {
      Serial.print(F("[1wire] #"));
      Serial.print(i);
      Serial.println(F(" read failed (disconnected? CRC error?) - skipping"));
      continue;
    }

    char payload[160];
    if (!primarySent) {
      snprintf(payload, sizeof(payload),
               "{\"temp_c\":%.2f,\"sensor\":\"%s\",\"sensors\":%u,\"uptime_s\":%lu}",
               tempC, hex, (unsigned)sensorCount, (unsigned long)uptimeS);
      Serial.print(F("[mqtt] publish "));
      Serial.print(MQTT_TOPIC);
      Serial.print(F(": "));
      Serial.println(payload);
      if (mqtt.connected()) mqtt.publish(MQTT_TOPIC, payload);
      primarySent = true;
    }

    if (sensorCount > 1) {
      char topic[96];
      snprintf(topic, sizeof(topic), "%s/%u", MQTT_TOPIC, (unsigned)i);
      snprintf(payload, sizeof(payload),
               "{\"temp_c\":%.2f,\"sensor\":\"%s\",\"index\":%u}",
               tempC, hex, (unsigned)i);
      Serial.print(F("[mqtt] publish "));
      Serial.print(topic);
      Serial.print(F(": "));
      Serial.println(payload);
      if (mqtt.connected()) mqtt.publish(topic, payload);
    }
  }

  if (primarySent) {
    publishCount++;
    digitalWrite(LED_BUILTIN, HIGH);
    delay(30);
    digitalWrite(LED_BUILTIN, LOW);
  }
}

// --- Arduino ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) { delay(10); }   // wait for USB CDC, but not forever

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.println();
  Serial.println(F("Fluidwire - Pico W DS18B20 temperature node"));
  Serial.print(F("[1wire] data on GP"));
  Serial.print(ONE_WIRE_PIN);
  Serial.print(F(", "));
  Serial.print(DS18B20_RESOLUTION);
  Serial.println(F("-bit resolution"));

  scanSensors();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(256);

  ensureWiFi();
  ensureMQTT();
}

void loop() {
  ensureWiFi();
  ensureMQTT();
  mqtt.loop();

  uint32_t now = millis();

  // Phase 1: time to take a reading -> start a conversion and come back later.
  if (!conversionPending && (lastPublish == 0 || now - lastPublish >= PUBLISH_INTERVAL_MS)) {
    if (sensorCount == 0) {
      scanSensors();                      // hot-plugged a probe? pick it up
    }
    startConversion();
    lastPublish = now;
  }

  // Phase 2: conversion done -> read all probes and publish.
  if (conversionPending && now - conversionStartedAt >= conversionMs) {
    conversionPending = false;
    publishReadings();
  }

  delay(10);
}
