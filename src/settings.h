#pragma once
// =============================================================================
//  settings.h — Persistent settings via ESP32 Preferences (NVS flash)
//
//  A 32-bit magic cookie at the start of the struct is the version marker.
//  If NVS has never been written (first boot) or the struct layout changes,
//  the magic won't match and defaults() is called automatically.
//  Bump SETTINGS_MAGIC any time the Settings struct layout changes.
// =============================================================================

#include <Preferences.h>
#include <string.h>

#define SETTINGS_MAGIC  0xEC19840Au   // 'EC' + Elite year 1984 + version 0xA

struct Settings {
    uint32_t magic;           // must equal SETTINGS_MAGIC — version sentinel

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

    // Home Assistant MQTT
    char     haHost[64];
    uint16_t haMqttPort;
    char     haMqttUser[32];
    char     haMqttPass[32];

    void defaults() {
        memset(this, 0, sizeof(*this));
        magic        = SETTINGS_MAGIC;
        gmtOffset    = 0;
        dstEnabled   = false;
        currentShip  = 0;
        shaded       = false;
        rotSpeed     = 0.6f;
        shipDuration = 30;
        haMqttPort   = 1883;
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
        size_t read = _prefs.getBytes("cfg", &cfg, sizeof(cfg));

        // Validate: wrong magic, wrong size, or obviously bad values → reset
        bool bad = (read != sizeof(cfg))
                || (cfg.magic != SETTINGS_MAGIC)
                || (cfg.rotSpeed  < 0.05f || cfg.rotSpeed  > 10.0f)
                || (cfg.shipDuration < 10 || cfg.shipDuration > 600)
                || (cfg.haMqttPort == 0);

        if (bad) {
            Serial.println("[Settings] First boot or schema change — using defaults");
            cfg.defaults();
            save();   // write defaults so next boot is clean
        }

        // currentShip is clamped later in main once NUM_SHIPS is known
    }

    void save() {
        cfg.magic = SETTINGS_MAGIC;    // always stamp before writing
        _prefs.putBytes("cfg", &cfg, sizeof(cfg));
    }

    void end() { _prefs.end(); }

private:
    Preferences _prefs;
};

// Global instance
SettingsManager settings;
