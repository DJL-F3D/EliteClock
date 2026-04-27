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

#define SETTINGS_MAGIC  0xEC19840Cu   // bump when struct layout or defaults change

struct Settings {
    uint32_t magic;

    // Wi-Fi
    char     wifiSSID[64];
    char     wifiPass[64];

    // Time
    int8_t   gmtOffset;
    bool     dstEnabled;

    // Ship display
    uint8_t  currentShip;
    bool     shaded;
    float    rotSpeed;
    uint16_t shipDuration;

    // Commander name shown in header
    char     cmdName[24];     // e.g. "CMDR JAMESON"

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
        shipDuration = 60;
        haMqttPort   = 1883;
        strncpy(cmdName, "CMDR JAMESON", sizeof(cmdName) - 1);
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
