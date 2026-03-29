#pragma once
// =============================================================================
//  wifi_mgr.h — Wi-Fi manager
//
//  On boot:
//    1. If saved SSID non-empty → try WPA2 connect (15 s timeout).
//    2. If successful → STA mode, sync NTP.
//    3. If failed (or no saved SSID) → launch SoftAP "EliteClock-Setup"
//       AND start a DNS server that redirects ALL queries to 192.168.4.1
//       (captive portal pattern — most phones show the config page automatically).
//
//  Call dnsLoop() from loop() to keep the DNS server pumped.
// =============================================================================

#include <WiFi.h>
#include <DNSServer.h>
#include <time.h>
#include "settings.h"

#define WIFI_CONNECT_TIMEOUT_MS  15000
#define AP_SSID                  "EliteClock-Setup"
#define AP_PASS                  "eliteclock"
#define DNS_PORT                 53

enum class WifiState { CONNECTING, CONNECTED, ACCESS_POINT, NO_WIFI };

class WifiManager {
public:
    WifiState state   = WifiState::NO_WIFI;
    String    localIP = "0.0.0.0";

    void begin() {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(true);
        delay(100);

        if (strlen(settings.cfg.wifiSSID) > 0) {
            Serial.printf("[WiFi] Connecting to '%s'\n", settings.cfg.wifiSSID);
            state = WifiState::CONNECTING;
            WiFi.begin(settings.cfg.wifiSSID, settings.cfg.wifiPass);

            uint32_t t = millis();
            while (WiFi.status() != WL_CONNECTED) {
                if (millis() - t > WIFI_CONNECT_TIMEOUT_MS) break;
                delay(200);
            }
        }

        if (WiFi.status() == WL_CONNECTED) {
            state   = WifiState::CONNECTED;
            localIP = WiFi.localIP().toString();
            Serial.printf("[WiFi] Connected. IP: %s\n", localIP.c_str());
            syncNTP();
        } else {
            startAP();
        }
    }

    // ── Try new credentials, save on success ─────────────────────────────
    bool reconnect(const char *ssid, const char *pass) {
        // Stop AP + DNS
        if (_dns) { _dns->stop(); }
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, pass);

        uint32_t t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_CONNECT_TIMEOUT_MS) {
            delay(200);
        }

        if (WiFi.status() == WL_CONNECTED) {
            strncpy(settings.cfg.wifiSSID, ssid, 63);
            strncpy(settings.cfg.wifiPass, pass, 63);
            settings.save();

            state   = WifiState::CONNECTED;
            localIP = WiFi.localIP().toString();
            syncNTP();
            Serial.printf("[WiFi] Reconnected. IP: %s\n", localIP.c_str());
            return true;
        } else {
            Serial.println("[WiFi] Reconnect failed, falling back to AP");
            startAP();
            return false;
        }
    }

    bool isConnected() const { return state == WifiState::CONNECTED; }
    bool isAP()        const { return state == WifiState::ACCESS_POINT; }

    // ── Must be called from loop() to pump captive-portal DNS ────────────
    void dnsLoop() {
        if (_dns) _dns->processNextRequest();
    }

    // ── Sync NTP with saved GMT offset + DST ─────────────────────────────
    void syncNTP() {
        long  off = (long)settings.cfg.gmtOffset * 3600L;
        int   dst = settings.cfg.dstEnabled ? 3600 : 0;
        configTime(off, dst, "pool.ntp.org", "time.nist.gov", "time.google.com");
        Serial.printf("[NTP] Sync started (UTC%+d %s)\n",
                      settings.cfg.gmtOffset,
                      settings.cfg.dstEnabled ? "+DST" : "");
    }

    // ── Wi-Fi scan — returns JSON array of SSIDs ──────────────────────────
    String scanNetworks() {
        // Scan in STA mode (works even while AP is running in AP_STA mode
        // if we temporarily switch — but simpler to just scan from STA)
        int n = WiFi.scanNetworks(false, false);
        String out = "[";
        for (int i = 0; i < n; i++) {
            // Escape double-quotes in SSID
            String ssid = WiFi.SSID(i);
            ssid.replace("\"", "\\\"");
            out += "\"" + ssid + "\"";
            if (i < n - 1) out += ",";
        }
        out += "]";
        WiFi.scanDelete();
        return out;
    }

private:
    DNSServer *_dns = nullptr;

    void startAP() {
        Serial.printf("[WiFi] Starting SoftAP: %s\n", AP_SSID);
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASS);
        delay(100);

        localIP = WiFi.softAPIP().toString();
        state   = WifiState::ACCESS_POINT;
        Serial.printf("[WiFi] AP IP: %s\n", localIP.c_str());

        // ── Captive portal DNS: redirect every hostname to our IP ─────────
        if (!_dns) _dns = new DNSServer();
        _dns->setErrorReplyCode(DNSReplyCode::NoError);
        _dns->start(DNS_PORT, "*", WiFi.softAPIP());
        Serial.println("[DNS] Captive portal DNS started");
    }
};

WifiManager wifiMgr;
