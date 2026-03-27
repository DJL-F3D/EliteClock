#pragma once
// =============================================================================
//  wifi_mgr.h — Wi-Fi manager
//  1. On boot: tries to connect to saved SSID.
//  2. If not found / timeout: launches SoftAP "EliteClock-Setup".
//  3. Exposes isConnected() / isAP() / localIP().
// =============================================================================

#include <WiFi.h>
#include <time.h>
#include "settings.h"

#define WIFI_CONNECT_TIMEOUT_MS  15000
#define AP_SSID                  "EliteClock-Setup"
#define AP_PASS                  "eliteclock"

enum class WifiState { CONNECTING, CONNECTED, ACCESS_POINT, NO_WIFI };

class WifiManager {
public:
    WifiState state = WifiState::NO_WIFI;
    String    localIP;

    void begin() {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(true);
        delay(100);

        if (strlen(settings.cfg.wifiSSID) > 0) {
            Serial.printf("[WiFi] Connecting to %s\n", settings.cfg.wifiSSID);
            state = WifiState::CONNECTING;
            WiFi.begin(settings.cfg.wifiSSID, settings.cfg.wifiPass);

            uint32_t t = millis();
            while (WiFi.status() != WL_CONNECTED) {
                if (millis() - t > WIFI_CONNECT_TIMEOUT_MS) break;
                delay(250);
            }
        }

        if (WiFi.status() == WL_CONNECTED) {
            state   = WifiState::CONNECTED;
            localIP = WiFi.localIP().toString();
            Serial.printf("[WiFi] Connected, IP: %s\n", localIP.c_str());
            syncNTP();
        } else {
            startAP();
        }
    }

    // Call from web handler after saving new credentials
    void reconnect(const char *ssid, const char *pass) {
        strncpy(settings.cfg.wifiSSID, ssid, 63);
        strncpy(settings.cfg.wifiPass, pass, 63);
        settings.save();

        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, pass);

        uint32_t t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_CONNECT_TIMEOUT_MS) {
            delay(250);
        }

        if (WiFi.status() == WL_CONNECTED) {
            state   = WifiState::CONNECTED;
            localIP = WiFi.localIP().toString();
            syncNTP();
        } else {
            startAP();
        }
    }

    bool isConnected() { return state == WifiState::CONNECTED; }
    bool isAP()        { return state == WifiState::ACCESS_POINT; }

    void syncNTP() {
        long  off = (long)settings.cfg.gmtOffset * 3600L;
        int   dst = settings.cfg.dstEnabled ? 3600 : 0;
        configTime(off, dst, "pool.ntp.org", "time.nist.gov");
        Serial.println("[NTP] Time sync initiated");
    }

    // Perform a scan and return comma-separated SSID list
    String scanNetworks() {
        WiFi.mode(WIFI_STA);
        int n = WiFi.scanNetworks();
        String out = "[";
        for (int i = 0; i < n; i++) {
            out += "\"" + WiFi.SSID(i) + "\"";
            if (i < n-1) out += ",";
        }
        out += "]";
        return out;
    }

private:
    void startAP() {
        Serial.println("[WiFi] Starting AP: " AP_SSID);
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASS);
        localIP = WiFi.softAPIP().toString();
        state   = WifiState::ACCESS_POINT;
        Serial.printf("[WiFi] AP IP: %s\n", localIP.c_str());
    }
};

WifiManager wifiMgr;
