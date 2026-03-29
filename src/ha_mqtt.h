#pragma once
// =============================================================================
//  ha_mqtt.h — Home Assistant MQTT integration
//
//  Auto-discovery entities created in HA:
//    • select   — Ship Selection  (all ship names as options)
//    • switch   — Shaded Mode
//    • number   — Rotation Speed  (0.1 – 5.0 °/frame, step 0.1)
//    • number   — Ship Duration   (10 – 600 s, step 5)
//    • button   — Previous Ship
//    • button   — Next Ship
//
//  IMPORTANT: We call _mqtt->setBufferSize(1024) before connecting because
//  the ship select discovery payload (~700 bytes with 19 ship names) exceeds
//  PubSubClient's default 256-byte limit and would be silently dropped.
// =============================================================================

#include <PubSubClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include "settings.h"
#include "ships.h"    // for SHIPS[] and NUM_SHIPS directly

// Forward declarations from main.cpp
extern uint8_t  g_currentShip;
extern bool     g_shaded;
extern float    g_rotSpeed;
extern uint16_t g_shipDuration;
extern bool     g_shipChanged;
extern void     seedStars(uint8_t idx);  // re-seed star-field on ship change

#define HA_DEVICE_ID    "eliteclock"
#define HA_DEVICE_NAME  "Elite Clock"
#define HA_DISC_PFX     "homeassistant"
#define HA_BASE         "eliteclock"

// ── Topic strings ─────────────────────────────────────────────────────────────
static const char *TOPIC_STATE    = HA_BASE "/state";
static const char *TOPIC_CMD_SHIP = HA_BASE "/ship/set";
static const char *TOPIC_CMD_SHAD = HA_BASE "/shaded/set";
static const char *TOPIC_CMD_ROT  = HA_BASE "/rotspeed/set";
static const char *TOPIC_CMD_DUR  = HA_BASE "/duration/set";
static const char *TOPIC_CMD_PREV = HA_BASE "/prev/set";
static const char *TOPIC_CMD_NEXT = HA_BASE "/next/set";

// ── MQTT payload buffer (must hold the select discovery with all ship names) ─
#define MQTT_BUFFER_SIZE  1024

// ── Shared device block helper ─────────────────────────────────────────────────
static void addDeviceBlock(JsonDocument &doc) {
    JsonObject dev       = doc.createNestedObject("device");
    dev["identifiers"][0] = HA_DEVICE_ID;
    dev["name"]           = HA_DEVICE_NAME;
    dev["manufacturer"]   = "DIY";
    dev["model"]          = "ESP32-C3 240x320";
    dev["sw_version"]     = "1.1";
}

// =============================================================================
class HaMqtt {
public:
    bool connected = false;

    void begin() {
        _wc   = new WiFiClient();
        _mqtt = new PubSubClient(*_wc);
        _mqtt->setBufferSize(MQTT_BUFFER_SIZE);
    }

    void loop() {
        if (!_mqtt || strlen(settings.cfg.haHost) == 0) return;

        if (!_mqtt->connected()) {
            connected = false;
            if (millis() - _lastRetry > 12000) {
                _lastRetry = millis();
                reconnect();
            }
        } else {
            connected = true;
            _mqtt->loop();
        }
    }

    void publishState() {
        if (!connected) return;
        StaticJsonDocument<256> doc;
        doc["ship"]     = SHIPS[g_currentShip].name;
        doc["shipIdx"]  = g_currentShip;
        doc["shaded"]   = g_shaded ? "ON" : "OFF";
        doc["rotSpeed"] = g_rotSpeed;
        doc["duration"] = g_shipDuration;
        char buf[256];
        serializeJson(doc, buf, sizeof(buf));
        _mqtt->publish(TOPIC_STATE, buf, true);
    }

private:
    WiFiClient   *_wc       = nullptr;
    PubSubClient *_mqtt     = nullptr;
    uint32_t      _lastRetry = 0;

    // ── Connect + subscribe ────────────────────────────────────────────────
    void reconnect() {
        _mqtt->setServer(settings.cfg.haHost, settings.cfg.haMqttPort);
        _mqtt->setCallback(messageCallback);

        const char *usr = settings.cfg.haMqttUser;
        const char *pwd = settings.cfg.haMqttPass;
        bool ok = (strlen(usr) > 0)
            ? _mqtt->connect(HA_DEVICE_ID, usr, pwd,
                             HA_BASE "/status", 1, true, "offline")
            : _mqtt->connect(HA_DEVICE_ID, nullptr, nullptr,
                             HA_BASE "/status", 1, true, "offline");

        if (ok) {
            Serial.printf("[MQTT] Connected to %s:%u\n",
                          settings.cfg.haHost, settings.cfg.haMqttPort);
            connected = true;
            _mqtt->publish(HA_BASE "/status", "online", true);
            _mqtt->subscribe(TOPIC_CMD_SHIP);
            _mqtt->subscribe(TOPIC_CMD_SHAD);
            _mqtt->subscribe(TOPIC_CMD_ROT);
            _mqtt->subscribe(TOPIC_CMD_DUR);
            _mqtt->subscribe(TOPIC_CMD_PREV);
            _mqtt->subscribe(TOPIC_CMD_NEXT);
            publishDiscovery();
            publishState();
        } else {
            Serial.printf("[MQTT] Connect failed rc=%d (host=%s)\n",
                          _mqtt->state(), settings.cfg.haHost);
        }
    }

    // ── Auto-discovery payloads ────────────────────────────────────────────
    void publishDiscovery() {
        publishShipSelect();
        publishShadedSwitch();
        publishRotSpeed();
        publishDuration();
        publishPrevButton();
        publishNextButton();
    }

    // 1. Ship select — needs 'options' array so HA renders a real dropdown
    void publishShipSelect() {
        // Use DynamicJsonDocument because the options array can be large
        DynamicJsonDocument doc(1024);
        doc["name"]           = "Ship";
        doc["unique_id"]      = "eliteclock_ship";
        doc["command_topic"]  = TOPIC_CMD_SHIP;
        doc["state_topic"]    = TOPIC_STATE;
        doc["value_template"] = "{{ value_json.ship }}";
        doc["icon"]           = "mdi:rocket-launch";
        JsonArray opts = doc.createNestedArray("options");
        for (int i = 0; i < NUM_SHIPS; i++) {
            opts.add(SHIPS[i].name);
        }
        addDeviceBlock(doc);

        char buf[MQTT_BUFFER_SIZE];
        size_t n = serializeJson(doc, buf, sizeof(buf));
        Serial.printf("[MQTT] Ship select payload %u bytes\n", (unsigned)n);
        _mqtt->publish(
            HA_DISC_PFX "/select/" HA_DEVICE_ID "_ship/config",
            (uint8_t *)buf, n, true);
    }

    // 2. Shaded mode switch
    void publishShadedSwitch() {
        StaticJsonDocument<384> doc;
        doc["name"]           = "Shaded Mode";
        doc["unique_id"]      = "eliteclock_shaded";
        doc["command_topic"]  = TOPIC_CMD_SHAD;
        doc["state_topic"]    = TOPIC_STATE;
        doc["value_template"] = "{{ value_json.shaded }}";
        doc["payload_on"]     = "ON";
        doc["payload_off"]    = "OFF";
        doc["icon"]           = "mdi:cube-scan";
        addDeviceBlock(doc);
        char buf[384]; size_t n = serializeJson(doc, buf, sizeof(buf));
        _mqtt->publish(
            HA_DISC_PFX "/switch/" HA_DEVICE_ID "_shaded/config",
            (uint8_t *)buf, n, true);
    }

    // 3. Rotation speed number slider
    void publishRotSpeed() {
        StaticJsonDocument<384> doc;
        doc["name"]             = "Rotation Speed";
        doc["unique_id"]        = "eliteclock_rotspeed";
        doc["command_topic"]    = TOPIC_CMD_ROT;
        doc["state_topic"]      = TOPIC_STATE;
        doc["value_template"]   = "{{ value_json.rotSpeed }}";
        doc["min"]              = 0.1;
        doc["max"]              = 5.0;
        doc["step"]             = 0.1;
        doc["unit_of_measurement"] = "°/f";
        doc["icon"]             = "mdi:rotate-3d-variant";
        addDeviceBlock(doc);
        char buf[384]; size_t n = serializeJson(doc, buf, sizeof(buf));
        _mqtt->publish(
            HA_DISC_PFX "/number/" HA_DEVICE_ID "_rot/config",
            (uint8_t *)buf, n, true);
    }

    // 4. Ship duration number slider
    void publishDuration() {
        StaticJsonDocument<384> doc;
        doc["name"]             = "Ship Duration";
        doc["unique_id"]        = "eliteclock_duration";
        doc["command_topic"]    = TOPIC_CMD_DUR;
        doc["state_topic"]      = TOPIC_STATE;
        doc["value_template"]   = "{{ value_json.duration }}";
        doc["min"]              = 10;
        doc["max"]              = 600;
        doc["step"]             = 5;
        doc["unit_of_measurement"] = "s";
        doc["icon"]             = "mdi:timer-sand";
        addDeviceBlock(doc);
        char buf[384]; size_t n = serializeJson(doc, buf, sizeof(buf));
        _mqtt->publish(
            HA_DISC_PFX "/number/" HA_DEVICE_ID "_dur/config",
            (uint8_t *)buf, n, true);
    }

    // 5. Previous ship button
    void publishPrevButton() {
        StaticJsonDocument<256> doc;
        doc["name"]           = "Previous Ship";
        doc["unique_id"]      = "eliteclock_prev";
        doc["command_topic"]  = TOPIC_CMD_PREV;
        doc["payload_press"]  = "PRESS";
        doc["icon"]           = "mdi:chevron-left-circle";
        addDeviceBlock(doc);
        char buf[256]; size_t n = serializeJson(doc, buf, sizeof(buf));
        _mqtt->publish(
            HA_DISC_PFX "/button/" HA_DEVICE_ID "_prev/config",
            (uint8_t *)buf, n, true);
    }

    // 6. Next ship button
    void publishNextButton() {
        StaticJsonDocument<256> doc;
        doc["name"]           = "Next Ship";
        doc["unique_id"]      = "eliteclock_next";
        doc["command_topic"]  = TOPIC_CMD_NEXT;
        doc["payload_press"]  = "PRESS";
        doc["icon"]           = "mdi:chevron-right-circle";
        addDeviceBlock(doc);
        char buf[256]; size_t n = serializeJson(doc, buf, sizeof(buf));
        _mqtt->publish(
            HA_DISC_PFX "/button/" HA_DEVICE_ID "_next/config",
            (uint8_t *)buf, n, true);
    }

    // ── Inbound message handler ────────────────────────────────────────────
    static void messageCallback(char *topic, byte *payload, unsigned int len) {
        // Null-terminate the payload safely
        char val[128] = {};
        strncpy(val, (char *)payload, (len < 127) ? len : 127);

        String t(topic);

        // ── Ship select (HA sends ship NAME string) ────────────────────────
        if (t == TOPIC_CMD_SHIP) {
            for (int i = 0; i < NUM_SHIPS; i++) {
                if (strcmp(SHIPS[i].name, val) == 0) {
                    settings.cfg.currentShip = (uint8_t)i;
                    g_currentShip = (uint8_t)i;
                    seedStars(g_currentShip);
                    g_shipChanged = true;
                    settings.save();
                    Serial.printf("[MQTT] Ship set to %s (%d)\n", val, i);
                    return;
                }
            }
            Serial.printf("[MQTT] Unknown ship name: %s\n", val);
            return;
        }

        // ── Shaded switch ──────────────────────────────────────────────────
        if (t == TOPIC_CMD_SHAD) {
            g_shaded = (strcmp(val, "ON") == 0);
            settings.cfg.shaded = g_shaded;
            settings.save();
            return;
        }

        // ── Rotation speed ─────────────────────────────────────────────────
        if (t == TOPIC_CMD_ROT) {
            float rs = atof(val);
            if (rs >= 0.05f && rs <= 10.0f) {
                g_rotSpeed = rs;
                settings.cfg.rotSpeed = rs;
                settings.save();
            }
            return;
        }

        // ── Ship duration ──────────────────────────────────────────────────
        if (t == TOPIC_CMD_DUR) {
            int d = atoi(val);
            if (d >= 10 && d <= 600) {
                g_shipDuration = (uint16_t)d;
                settings.cfg.shipDuration = (uint16_t)d;
                settings.save();
            }
            return;
        }

        // ── Prev / Next buttons ────────────────────────────────────────────
        if (t == TOPIC_CMD_PREV) {
            g_currentShip = (g_currentShip + NUM_SHIPS - 1) % NUM_SHIPS;
            settings.cfg.currentShip = g_currentShip;
            seedStars(g_currentShip);
            g_shipChanged = true;
            settings.save();
            return;
        }
        if (t == TOPIC_CMD_NEXT) {
            g_currentShip = (g_currentShip + 1) % NUM_SHIPS;
            settings.cfg.currentShip = g_currentShip;
            seedStars(g_currentShip);
            g_shipChanged = true;
            settings.save();
            return;
        }
    }
};

HaMqtt haMqtt;
