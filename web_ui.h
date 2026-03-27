#pragma once
// =============================================================================
//  web_ui.h — AsyncWebServer REST endpoints
//  Serves the index.html from LittleFS and handles all /api/* routes.
// =============================================================================

#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "settings.h"
#include "wifi_mgr.h"

// Forward declarations from main
extern uint8_t  g_currentShip;
extern bool     g_shaded;
extern float    g_rotSpeed;
extern uint16_t g_shipDuration;
extern bool     g_shipChanged;
extern int      g_shipChangeDir;
extern const char* getShipName(int idx);
extern int      getNumShips();

class WebUI {
public:
    AsyncWebServer server{80};

    void begin() {
        if (!LittleFS.begin(true)) {
            Serial.println("[FS] LittleFS mount failed");
        }

        // ── Static files ──────────────────────────────────────────────────
        server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

        // ── GET /api/state — full state JSON ─────────────────────────────
        server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *req) {
            StaticJsonDocument<512> doc;
            doc["ship"]       = settings.cfg.currentShip;
            doc["shipName"]   = getShipName(settings.cfg.currentShip);
            doc["numShips"]   = getNumShips();
            doc["shaded"]     = settings.cfg.shaded;
            doc["rotSpeed"]   = settings.cfg.rotSpeed;
            doc["shipDur"]    = settings.cfg.shipDuration;
            doc["gmt"]        = settings.cfg.gmtOffset;
            doc["dst"]        = settings.cfg.dstEnabled;
            doc["ssid"]       = settings.cfg.wifiSSID;
            doc["haHost"]     = settings.cfg.haHost;
            doc["haMqttPort"] = settings.cfg.haMqttPort;
            doc["haMqttUser"] = settings.cfg.haMqttUser;
            doc["mode"]       = wifiMgr.isAP() ? "AP" : "STA";
            doc["ip"]         = wifiMgr.localIP;
            String out; serializeJson(doc, out);
            req->send(200, "application/json", out);
        });

        // ── GET /api/time — current time string ───────────────────────────
        server.on("/api/time", HTTP_GET, [](AsyncWebServerRequest *req) {
            struct tm ti;
            StaticJsonDocument<128> doc;
            if (getLocalTime(&ti)) {
                char buf[20];
                strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
                doc["time"] = buf;
                char datebuf[20];
                strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", &ti);
                doc["date"] = datebuf;
            } else {
                doc["time"] = "--:--:--";
            }
            String out; serializeJson(doc, out);
            req->send(200, "application/json", out);
        });

        // ── POST /api/ship — prev/next navigation ────────────────────────
        server.on("/api/ship", HTTP_POST, [](AsyncWebServerRequest *req) {
            if (req->hasParam("action", true)) {
                String act = req->getParam("action", true)->value();
                int n = getNumShips();
                if (act == "next") {
                    settings.cfg.currentShip = (settings.cfg.currentShip + 1) % n;
                } else if (act == "prev") {
                    settings.cfg.currentShip = (settings.cfg.currentShip + n - 1) % n;
                } else if (req->hasParam("index", true)) {
                    int idx = req->getParam("index", true)->value().toInt();
                    if (idx >= 0 && idx < n) settings.cfg.currentShip = idx;
                }
                g_currentShip = settings.cfg.currentShip;
                g_shipChanged = true;
                settings.save();
            }
            StaticJsonDocument<128> doc;
            doc["ship"]     = settings.cfg.currentShip;
            doc["shipName"] = getShipName(settings.cfg.currentShip);
            String out; serializeJson(doc, out);
            req->send(200, "application/json", out);
        });

        // ── POST /api/display — update display parameters ─────────────────
        server.on("/api/display", HTTP_POST, [](AsyncWebServerRequest *req) {
            if (req->hasParam("shaded", true)) {
                settings.cfg.shaded = req->getParam("shaded", true)->value().toInt() != 0;
                g_shaded = settings.cfg.shaded;
            }
            if (req->hasParam("rotSpeed", true)) {
                float rs = req->getParam("rotSpeed", true)->value().toFloat();
                if (rs >= 0.05f && rs <= 10.0f) {
                    settings.cfg.rotSpeed = rs;
                    g_rotSpeed = rs;
                }
            }
            if (req->hasParam("shipDur", true)) {
                int sd = req->getParam("shipDur", true)->value().toInt();
                if (sd >= 10 && sd <= 600) {
                    settings.cfg.shipDuration = sd;
                    g_shipDuration = sd;
                }
            }
            settings.save();
            req->send(200, "application/json", "{\"ok\":true}");
        });

        // ── GET /api/wifi/scan ────────────────────────────────────────────
        server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
            String nets = wifiMgr.scanNetworks();
            req->send(200, "application/json", nets);
        });

        // ── POST /api/wifi/connect ────────────────────────────────────────
        server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *req) {
            String ssid, pass;
            if (req->hasParam("ssid", true)) ssid = req->getParam("ssid", true)->value();
            if (req->hasParam("pass", true)) pass = req->getParam("pass", true)->value();
            req->send(200, "application/json", "{\"ok\":true,\"msg\":\"Connecting...\"}");
            // Delay so response can be sent, then reconnect
            delay(500);
            wifiMgr.reconnect(ssid.c_str(), pass.c_str());
        });

        // ── POST /api/time — save GMT/DST settings ───────────────────────
        server.on("/api/time", HTTP_POST, [](AsyncWebServerRequest *req) {
            if (req->hasParam("gmt", true)) {
                settings.cfg.gmtOffset = req->getParam("gmt", true)->value().toInt();
            }
            if (req->hasParam("dst", true)) {
                settings.cfg.dstEnabled = req->getParam("dst", true)->value().toInt() != 0;
            }
            settings.save();
            if (wifiMgr.isConnected()) wifiMgr.syncNTP();
            req->send(200, "application/json", "{\"ok\":true}");
        });

        // ── POST /api/time/manual — set clock manually ───────────────────
        server.on("/api/time/manual", HTTP_POST, [](AsyncWebServerRequest *req) {
            if (req->hasParam("dt", true)) {
                String dt = req->getParam("dt", true)->value();
                // parse "YYYY-MM-DD HH:MM:SS"
                struct tm ti = {};
                if (sscanf(dt.c_str(), "%d-%d-%d %d:%d:%d",
                           &ti.tm_year, &ti.tm_mon, &ti.tm_mday,
                           &ti.tm_hour, &ti.tm_min, &ti.tm_sec) == 6) {
                    ti.tm_year -= 1900;
                    ti.tm_mon  -= 1;
                    time_t t    = mktime(&ti);
                    struct timeval tv = {t, 0};
                    settimeofday(&tv, nullptr);
                    req->send(200, "application/json", "{\"ok\":true}");
                    return;
                }
            }
            req->send(400, "application/json", "{\"ok\":false}");
        });

        // ── POST /api/ha — Home Assistant MQTT settings ───────────────────
        server.on("/api/ha", HTTP_POST, [](AsyncWebServerRequest *req) {
            if (req->hasParam("host", true))
                strncpy(settings.cfg.haHost, req->getParam("host",true)->value().c_str(), 63);
            if (req->hasParam("port", true))
                settings.cfg.haMqttPort = req->getParam("port",true)->value().toInt();
            if (req->hasParam("user", true))
                strncpy(settings.cfg.haMqttUser, req->getParam("user",true)->value().c_str(), 31);
            if (req->hasParam("pass", true))
                strncpy(settings.cfg.haMqttPass, req->getParam("pass",true)->value().c_str(), 31);
            settings.save();
            req->send(200, "application/json", "{\"ok\":true}");
        });

        // 404
        server.onNotFound([](AsyncWebServerRequest *req) {
            req->send(404, "text/plain", "Not found");
        });

        server.begin();
        Serial.println("[Web] Server started");
    }
};

WebUI webUI;
