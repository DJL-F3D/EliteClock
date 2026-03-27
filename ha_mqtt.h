#pragma once
// =============================================================================
//  ha_mqtt.h — Home Assistant MQTT integration
//  Publishes ship clock state to HA and subscribes to command topics.
//  Uses MQTT auto-discovery so entities appear automatically in HA.
// =============================================================================

#include <PubSubClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include "settings.h"

// Forward declarations
extern uint8_t  g_currentShip;
extern bool     g_shaded;
extern float    g_rotSpeed;
extern uint16_t g_shipDuration;
extern bool     g_shipChanged;
extern const char* getShipName(int idx);
extern int      getNumShips();

#define HA_DEVICE_ID   "eliteclock"
#define HA_DEVICE_NAME "Elite Clock"
#define HA_DISC_PFX    "homeassistant"
#define HA_STATE_PFX   "eliteclock"

// ── Topic helpers ─────────────────────────────────────────────────────────
#define TOPIC_STATE     HA_STATE_PFX "/state"
#define TOPIC_CMD_SHIP  HA_STATE_PFX "/ship/set"
#define TOPIC_CMD_SHAD  HA_STATE_PFX "/shaded/set"
#define TOPIC_CMD_ROT   HA_STATE_PFX "/rotspeed/set"
#define TOPIC_CMD_DUR   HA_STATE_PFX "/duration/set"
#define TOPIC_CMD_PREV  HA_STATE_PFX "/prev/set"
#define TOPIC_CMD_NEXT  HA_STATE_PFX "/next/set"

class HaMqtt {
public:
    bool connected = false;

    void begin() {
        _wc  = new WiFiClient();
        _mqtt= new PubSubClient(*_wc);
    }

    void loop() {
        if (!_mqtt || strlen(settings.cfg.haHost) == 0) return;

        if (!_mqtt->connected()) {
            connected = false;
            if (millis() - _lastRetry > 10000) {
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
        doc["ship"]     = getShipName(g_currentShip);
        doc["shipIdx"]  = g_currentShip;
        doc["shaded"]   = g_shaded ? "ON" : "OFF";
        doc["rotSpeed"] = g_rotSpeed;
        doc["duration"] = g_shipDuration;
        char buf[256];
        serializeJson(doc, buf, sizeof(buf));
        _mqtt->publish(TOPIC_STATE, buf, true);
    }

private:
    WiFiClient   *_wc   = nullptr;
    PubSubClient *_mqtt = nullptr;
    uint32_t      _lastRetry = 0;

    void reconnect() {
        _mqtt->setServer(settings.cfg.haHost, settings.cfg.haMqttPort);
        _mqtt->setCallback([](char *topic, byte *payload, unsigned int len) {
            handleMessage(topic, payload, len);
        });

        const char *cid = HA_DEVICE_ID;
        const char *usr = settings.cfg.haMqttUser;
        const char *pwd = settings.cfg.haMqttPass;

        bool ok = (strlen(usr) > 0)
            ? _mqtt->connect(cid, usr, pwd)
            : _mqtt->connect(cid);

        if (ok) {
            Serial.println("[MQTT] Connected to HA broker");
            connected = true;
            _mqtt->subscribe(TOPIC_CMD_SHIP);
            _mqtt->subscribe(TOPIC_CMD_SHAD);
            _mqtt->subscribe(TOPIC_CMD_ROT);
            _mqtt->subscribe(TOPIC_CMD_DUR);
            _mqtt->subscribe(TOPIC_CMD_PREV);
            _mqtt->subscribe(TOPIC_CMD_NEXT);
            publishDiscovery();
            publishState();
        } else {
            Serial.printf("[MQTT] Connect failed, rc=%d\n", _mqtt->state());
        }
    }

    // ── MQTT auto-discovery payloads ──────────────────────────────────────
    void publishDiscovery() {
        // 1. Select entity for ship choice
        {
            StaticJsonDocument<512> doc;
            doc["name"]        = "Ship Selection";
            doc["unique_id"]   = "eliteclock_ship";
            doc["command_topic"] = TOPIC_CMD_SHIP;
            doc["state_topic"] = TOPIC_STATE;
            doc["value_template"] = "{{ value_json.ship }}";
            doc["icon"] = "mdi:space-station";
            JsonObject dev = doc.createNestedObject("device");
            dev["identifiers"][0] = HA_DEVICE_ID;
            dev["name"]           = HA_DEVICE_NAME;
            dev["manufacturer"]   = "Elite Clock";
            dev["model"]          = "ESP32-C3";
            char buf[512]; serializeJson(doc, buf, sizeof(buf));
            _mqtt->publish(HA_DISC_PFX "/select/" HA_DEVICE_ID "_ship/config", buf, true);
        }
        // 2. Switch for shaded mode
        {
            StaticJsonDocument<384> doc;
            doc["name"]          = "Shaded Mode";
            doc["unique_id"]     = "eliteclock_shaded";
            doc["command_topic"] = TOPIC_CMD_SHAD;
            doc["state_topic"]   = TOPIC_STATE;
            doc["value_template"]= "{{ value_json.shaded }}";
            doc["payload_on"]    = "ON";
            doc["payload_off"]   = "OFF";
            doc["icon"]          = "mdi:cube-outline";
            JsonObject dev = doc.createNestedObject("device");
            dev["identifiers"][0] = HA_DEVICE_ID;
            char buf[384]; serializeJson(doc, buf, sizeof(buf));
            _mqtt->publish(HA_DISC_PFX "/switch/" HA_DEVICE_ID "_shaded/config", buf, true);
        }
        // 3. Number slider: rotation speed
        {
            StaticJsonDocument<384> doc;
            doc["name"]          = "Rotation Speed";
            doc["unique_id"]     = "eliteclock_rotspeed";
            doc["command_topic"] = TOPIC_CMD_ROT;
            doc["state_topic"]   = TOPIC_STATE;
            doc["value_template"]= "{{ value_json.rotSpeed }}";
            doc["min"]           = 0.1;
            doc["max"]           = 5.0;
            doc["step"]          = 0.1;
            doc["icon"]          = "mdi:rotate-3d";
            JsonObject dev = doc.createNestedObject("device");
            dev["identifiers"][0] = HA_DEVICE_ID;
            char buf[384]; serializeJson(doc, buf, sizeof(buf));
            _mqtt->publish(HA_DISC_PFX "/number/" HA_DEVICE_ID "_rot/config", buf, true);
        }
        // 4. Number slider: ship duration
        {
            StaticJsonDocument<384> doc;
            doc["name"]          = "Ship Duration";
            doc["unique_id"]     = "eliteclock_duration";
            doc["command_topic"] = TOPIC_CMD_DUR;
            doc["state_topic"]   = TOPIC_STATE;
            doc["value_template"]= "{{ value_json.duration }}";
            doc["min"]           = 10;
            doc["max"]           = 600;
            doc["step"]          = 5;
            doc["unit_of_measurement"] = "s";
            doc["icon"]          = "mdi:timer-outline";
            JsonObject dev = doc.createNestedObject("device");
            dev["identifiers"][0] = HA_DEVICE_ID;
            char buf[384]; serializeJson(doc, buf, sizeof(buf));
            _mqtt->publish(HA_DISC_PFX "/number/" HA_DEVICE_ID "_dur/config", buf, true);
        }
        // 5. Button: prev ship
        {
            StaticJsonDocument<256> doc;
            doc["name"]          = "Previous Ship";
            doc["unique_id"]     = "eliteclock_prev";
            doc["command_topic"] = TOPIC_CMD_PREV;
            doc["payload_press"] = "PRESS";
            doc["icon"]          = "mdi:chevron-left";
            JsonObject dev = doc.createNestedObject("device");
            dev["identifiers"][0] = HA_DEVICE_ID;
            char buf[256]; serializeJson(doc, buf, sizeof(buf));
            _mqtt->publish(HA_DISC_PFX "/button/" HA_DEVICE_ID "_prev/config", buf, true);
        }
        // 6. Button: next ship
        {
            StaticJsonDocument<256> doc;
            doc["name"]          = "Next Ship";
            doc["unique_id"]     = "eliteclock_next";
            doc["command_topic"] = TOPIC_CMD_NEXT;
            doc["payload_press"] = "PRESS";
            doc["icon"]          = "mdi:chevron-right";
            JsonObject dev = doc.createNestedObject("device");
            dev["identifiers"][0] = HA_DEVICE_ID;
            char buf[256]; serializeJson(doc, buf, sizeof(buf));
            _mqtt->publish(HA_DISC_PFX "/button/" HA_DEVICE_ID "_next/config", buf, true);
        }
    }

    static void handleMessage(char *topic, byte *payload, unsigned int len) {
        char val[128] = {};
        strncpy(val, (char*)payload, min((unsigned int)127, len));

        String t(topic);
        int n = getNumShips();

        if (t == TOPIC_CMD_SHAD) {
            g_shaded = (strcmp(val, "ON") == 0);
            settings.cfg.shaded = g_shaded;
            settings.save();
        } else if (t == TOPIC_CMD_ROT) {
            float rs = atof(val);
            if (rs >= 0.05f && rs <= 10.0f) {
                g_rotSpeed = rs;
                settings.cfg.rotSpeed = rs;
                settings.save();
            }
        } else if (t == TOPIC_CMD_DUR) {
            int d = atoi(val);
            if (d >= 10 && d <= 600) {
                g_shipDuration = d;
                settings.cfg.shipDuration = d;
                settings.save();
            }
        } else if (t == TOPIC_CMD_PREV) {
            settings.cfg.currentShip = (settings.cfg.currentShip + n - 1) % n;
            g_currentShip = settings.cfg.currentShip;
            g_shipChanged = true;
            settings.save();
        } else if (t == TOPIC_CMD_NEXT) {
            settings.cfg.currentShip = (settings.cfg.currentShip + 1) % n;
            g_currentShip = settings.cfg.currentShip;
            g_shipChanged = true;
            settings.save();
        }
    }
};

HaMqtt haMqtt;
