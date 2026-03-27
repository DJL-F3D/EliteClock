#pragma once
// =============================================================================
//  settings.h — Persistent settings via ESP32 Preferences (NVS flash)
// =============================================================================

#include <Preferences.h>

struct Settings {
    // Wi-Fi
    char     wifiSSID[64];
    char     wifiPass[64];

    // Time
    int8_t   gmtOffset;       // -12 .. +14
    bool     dstEnabled;

    // Ship display
    uint8_t  currentShip;     // index into SHIPS[]
    bool     shaded;          // true = flat shaded, false = wireframe
    float    rotSpeed;        // degrees per frame (0.1..5.0)
    uint16_t shipDuration;    // seconds before next ship (10..600)

    // Web
    char     haHost[64];      // Home Assistant MQTT broker
    uint16_t haMqttPort;
    char     haMqttUser[32];
    char     haMqttPass[32];

    void defaults() {
        wifiSSID[0]   = '\0';
        wifiPass[0]   = '\0';
        gmtOffset     = 0;
        dstEnabled    = false;
        currentShip   = 0;
        shaded        = false;
        rotSpeed      = 0.6f;
        shipDuration  = 30;   // 30 seconds
        haHost[0]     = '\0';
        haMqttPort    = 1883;
        haMqttUser[0] = '\0';
        haMqttPass[0] = '\0';
    }
};

class SettingsManager {
public:
    Settings cfg;

    void begin() {
        cfg.defaults();
        _prefs.begin("eliteclock", false);
        load();
    }

    void load() {
        _prefs.getBytes("cfg", &cfg, sizeof(cfg));
        // Sanity check version marker
        if (cfg.rotSpeed < 0.05f || cfg.rotSpeed > 10.0f) {
            cfg.defaults();
        }
        if (cfg.shipDuration < 10 || cfg.shipDuration > 600) {
            cfg.shipDuration = 30;
        }
    }

    void save() {
        _prefs.putBytes("cfg", &cfg, sizeof(cfg));
    }

    void end() {
        _prefs.end();
    }

private:
    Preferences _prefs;
};

// Global instance
SettingsManager settings;
