// =============================================================================
//  main.cpp — Elite Clock main application
//  ESP32-C3 Super Mini + 2.8" UC8230 (ILI9341-compatible) 240×320 TFT
//
//  Screen layout (portrait 240×320):
//    ┌──────────────────────────┐  y=0
//    │  [PREV]   SHIP NAME  [NEXT]  │  ← touch zones top-left / top-right
//    │                          │
//    │    3-D ship (240×230px)  │  ← [SHADED TOGGLE] centre middle
//    │                          │
//    ├──────────────────────────┤  y=230
//    │  Elite-style radar panel │  ← tap = ship info overlay
//    │  HH:MM:SS   SHIP STATS   │
//    └──────────────────────────┘  y=320
// =============================================================================

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>
#include "ships.h"
#include "renderer.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "web_ui.h"
#include "ha_mqtt.h"

// ── Pin definitions ───────────────────────────────────────────────────────────
#define TOUCH_IRQ_PIN  10   // T_IRQ from XPT2046

// ── Display layout constants ──────────────────────────────────────────────────
#define SCR_W       240
#define SCR_H       320
#define SHIP_AREA_H 230     // ship 3-D view occupies top portion
#define RADAR_Y     230     // radar / info strip starts here
#define RADAR_H      90     // (320 - 230)
#define HDR_H        20     // header strip height

// Touch zones
#define PREV_ZONE_X1   0
#define PREV_ZONE_X2  80
#define PREV_ZONE_Y1   0
#define PREV_ZONE_Y2  40

#define NEXT_ZONE_X1 160
#define NEXT_ZONE_X2 240
#define NEXT_ZONE_Y1   0
#define NEXT_ZONE_Y2  40

#define SHAD_ZONE_X1  60
#define SHAD_ZONE_X2 180
#define SHAD_ZONE_Y1 100
#define SHAD_ZONE_Y2 160

#define RADAR_ZONE_Y1 RADAR_Y
#define RADAR_ZONE_Y2 SCR_H

// XPT2046 calibration (adjust for your panel)
#define TOUCH_XMIN  300
#define TOUCH_XMAX 3800
#define TOUCH_YMIN  300
#define TOUCH_YMAX 3800

// ── Colour palette ────────────────────────────────────────────────────────────
#define COL_BG       TFT_BLACK
#define COL_GREEN    0x07E0
#define COL_DKGREEN  0x0300
#define COL_AMBER    0xFD20
#define COL_CYAN     TFT_CYAN
#define COL_YELLOW   TFT_YELLOW
#define COL_BORDER   0x0260   // dark green border

// ── Global state (shared with web_ui / ha_mqtt) ───────────────────────────────
uint8_t  g_currentShip  = 0;
bool     g_shaded        = false;
float    g_rotSpeed      = 0.6f;
uint16_t g_shipDuration  = 30;
bool     g_shipChanged   = false;
int      g_shipChangeDir = 1;
bool     g_showInfo      = false;

// ── Hardware objects ──────────────────────────────────────────────────────────
TFT_eSPI          tft;
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ_PIN);
Renderer          renderer;

// ── Timing ────────────────────────────────────────────────────────────────────
uint32_t lastShipSwitch = 0;
uint32_t lastFrameTime  = 0;
uint32_t lastClockDraw  = 0;
uint32_t lastHaPublish  = 0;
uint32_t lastTouchTime  = 0;
bool     touchWasDown   = false;

// ── Frame buffer sprite for flicker-free rendering ────────────────────────────
TFT_eSprite shipSprite = TFT_eSprite(&tft);

// ── Helper: ship name accessor (used by web/MQTT modules) ────────────────────
const char* getShipName(int idx) {
    if (idx < 0 || idx >= NUM_SHIPS) return "Unknown";
    return SHIPS[idx].name;
}
int getNumShips() { return NUM_SHIPS; }

// ─────────────────────────────────────────────────────────────────────────────
//  Draw the Elite-style radar / dashboard strip (bottom 90px)
// ─────────────────────────────────────────────────────────────────────────────
void drawRadarPanel(bool showInfo) {
    const ShipDef &ship = SHIPS[g_currentShip];
    int y = RADAR_Y;

    // Background
    tft.fillRect(0, y, SCR_W, RADAR_H, COL_BG);
    tft.drawFastHLine(0, y, SCR_W, COL_BORDER);

    // Clock (top of panel)
    struct tm ti;
    char timebuf[16] = "--:--:--";
    if (getLocalTime(&ti)) {
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &ti);
    }
    tft.setTextColor(COL_GREEN, COL_BG);
    tft.setTextSize(2);
    tft.setCursor(4, y + 4);
    tft.print(timebuf);

    // Wi-Fi / AP indicator
    tft.setTextSize(1);
    if (wifiMgr.isAP()) {
        tft.setTextColor(COL_AMBER, COL_BG);
        tft.setCursor(170, y + 4);
        tft.print("AP MODE");
    } else if (wifiMgr.isConnected()) {
        tft.setTextColor(COL_DKGREEN, COL_BG);
        tft.setCursor(178, y + 4);
        tft.print("ONLINE");
    }

    if (showInfo) {
        // ── Ship information overlay ──────────────────────────────────────
        tft.setTextColor(COL_CYAN, COL_BG);
        tft.setTextSize(1);
        tft.setCursor(4, y + 22);
        tft.printf("%-20s", ship.name);

        tft.setTextColor(COL_GREEN, COL_BG);
        tft.setCursor(4, y + 34);
        tft.printf("SPEED:  %-3d  LAS: %d  MSL: %d",
                    ship.stats.speed,
                    ship.stats.lasers,
                    ship.stats.missiles);
        tft.setCursor(4, y + 46);
        tft.printf("HULL:   %-3d  ROLE: %s",
                    ship.stats.health,
                    ship.stats.role);
        if (ship.stats.bounty > 0) {
            tft.setCursor(4, y + 58);
            tft.printf("BOUNTY: %u.%uCr",
                        ship.stats.bounty / 10,
                        ship.stats.bounty % 10);
        }

        // Web UI hint
        tft.setTextColor(COL_DKGREEN, COL_BG);
        tft.setCursor(4, y + 74);
        tft.printf("http://%s", wifiMgr.localIP.c_str());

    } else {
        // ── Radar scanner (classic Elite elliptical radar) ────────────────
        int rx = SCR_W / 2;
        int ry = y + RADAR_H / 2 + 6;
        int ra = 80;  // x radius
        int rb = 28;  // y radius

        tft.drawEllipse(rx, ry, ra, rb, COL_DKGREEN);
        tft.drawEllipse(rx, ry, ra/2, rb/2, COL_DKGREEN);
        tft.drawFastHLine(rx - ra, ry, 2*ra, COL_DKGREEN);
        tft.drawFastVLine(rx, ry - rb, 2*rb, COL_DKGREEN);

        // Scanner dot (ship position — decorative)
        // Show current ship's index as a blip position
        float ang = (float)g_currentShip / NUM_SHIPS * 2.0f * PI;
        int bx = rx + (int)(cosf(ang) * ra * 0.6f);
        int by = ry + (int)(sinf(ang) * rb * 0.6f);
        tft.fillCircle(bx, by, 2, COL_YELLOW);
        tft.fillCircle(rx, ry, 2, COL_GREEN); // player dot centre
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Draw header bar (ship name + prev/next hints)
// ─────────────────────────────────────────────────────────────────────────────
void drawHeader() {
    tft.fillRect(0, 0, SCR_W, HDR_H, COL_BG);
    tft.drawFastHLine(0, HDR_H, SCR_W, COL_BORDER);

    // Prev hint
    tft.setTextColor(COL_DKGREEN, COL_BG);
    tft.setTextSize(1);
    tft.setCursor(2, 6);
    tft.print("<<");

    // Ship name centred
    const char *name = SHIPS[g_currentShip].name;
    int tw = strlen(name) * 6;
    tft.setTextColor(COL_GREEN, COL_BG);
    tft.setCursor((SCR_W - tw) / 2, 6);
    tft.print(name);

    // Next hint
    tft.setCursor(SCR_W - 14, 6);
    tft.setTextColor(COL_DKGREEN, COL_BG);
    tft.print(">>");

    // Shaded indicator
    if (g_shaded) {
        tft.setTextColor(COL_AMBER, COL_BG);
        tft.setCursor(2, 6);
        tft.print("[S]");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Render 3-D ship frame into sprite then push to TFT
// ─────────────────────────────────────────────────────────────────────────────
void renderShipFrame() {
    shipSprite.fillSprite(COL_BG);

    // Draw background star-field
    // (use seeded pseudo-random so they're stable per ship)
    srand(g_currentShip * 0x5A5A + 42);
    for (int i = 0; i < 40; i++) {
        int sx = rand() % 240;
        int sy = rand() % (SHIP_AREA_H - HDR_H);
        uint8_t br = rand() % 3;
        uint16_t sc = (br == 2) ? TFT_WHITE : (br == 1) ? 0x4208 : 0x2104;
        shipSprite.drawPixel(sx, HDR_H + sy, sc);
    }

    // Adjust renderer state
    renderer.shaded   = g_shaded;
    renderer.rotSpeedY= g_rotSpeed;

    // Offset render centre to ship view area centre
    // The renderer uses SHIP_VIEW_X/Y constants; we set sprite offsets below.
    renderer.render(shipSprite, SHIPS[g_currentShip]);

    // Push sprite to TFT (only ship area)
    shipSprite.pushSprite(0, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Touch handling
// ─────────────────────────────────────────────────────────────────────────────
void handleTouch() {
    if (!touch.tirqTouched() || !touch.touched()) {
        touchWasDown = false;
        return;
    }
    if (touchWasDown) return; // wait for release
    if (millis() - lastTouchTime < 300) return; // debounce

    TS_Point p = touch.getPoint();

    // Map raw touch to screen coords (landscape or portrait depending on panel)
    int tx = map(p.x, TOUCH_XMIN, TOUCH_XMAX, 0, SCR_W);
    int ty = map(p.y, TOUCH_YMIN, TOUCH_YMAX, 0, SCR_H);
    tx = constrain(tx, 0, SCR_W - 1);
    ty = constrain(ty, 0, SCR_H - 1);

    touchWasDown  = true;
    lastTouchTime = millis();

    // ── PREV ship (top-left) ─────────────────────────────────────────────
    if (tx >= PREV_ZONE_X1 && tx <= PREV_ZONE_X2 &&
        ty >= PREV_ZONE_Y1 && ty <= PREV_ZONE_Y2) {
        g_currentShip = (g_currentShip + NUM_SHIPS - 1) % NUM_SHIPS;
        settings.cfg.currentShip = g_currentShip;
        settings.save();
        g_shipChanged    = true;
        lastShipSwitch   = millis();
        return;
    }

    // ── NEXT ship (top-right) ────────────────────────────────────────────
    if (tx >= NEXT_ZONE_X1 && tx <= NEXT_ZONE_X2 &&
        ty >= NEXT_ZONE_Y1 && ty <= NEXT_ZONE_Y2) {
        g_currentShip = (g_currentShip + 1) % NUM_SHIPS;
        settings.cfg.currentShip = g_currentShip;
        settings.save();
        g_shipChanged  = true;
        lastShipSwitch = millis();
        return;
    }

    // ── SHADED toggle (centre middle of ship view) ───────────────────────
    if (tx >= SHAD_ZONE_X1 && tx <= SHAD_ZONE_X2 &&
        ty >= SHAD_ZONE_Y1 && ty <= SHAD_ZONE_Y2) {
        g_shaded = !g_shaded;
        settings.cfg.shaded = g_shaded;
        settings.save();
        return;
    }

    // ── RADAR zone — toggle info display ─────────────────────────────────
    if (ty >= RADAR_ZONE_Y1 && ty <= RADAR_ZONE_Y2) {
        g_showInfo = !g_showInfo;
        drawRadarPanel(g_showInfo);
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[Elite Clock] Booting...");

    // Load settings first
    settings.begin();
    g_currentShip = settings.cfg.currentShip;
    g_shaded      = settings.cfg.shaded;
    g_rotSpeed    = settings.cfg.rotSpeed;
    g_shipDuration= settings.cfg.shipDuration;

    // Initialise display
    tft.init();
    tft.setRotation(0);   // portrait
    tft.fillScreen(COL_BG);
    tft.setTextFont(1);

    // Show boot splash
    tft.setTextColor(COL_GREEN, COL_BG);
    tft.setTextSize(2);
    tft.setCursor(20, 60);
    tft.println("  ELITE CLOCK");
    tft.setTextSize(1);
    tft.setTextColor(COL_DKGREEN, COL_BG);
    tft.setCursor(30, 100);
    tft.println("LOADING SYSTEMS...");
    tft.setCursor(30, 116);
    tft.println("ESP32-C3 240x320");

    // Create ship sprite (ship area only, top SHIP_AREA_H px)
    shipSprite.createSprite(SCR_W, SHIP_AREA_H);
    shipSprite.setColorDepth(16);

    // Initialise touchscreen
    touch.begin();
    touch.setRotation(1);

    // Wi-Fi
    tft.setCursor(30, 140);
    tft.print("Wi-Fi...");
    wifiMgr.begin();
    tft.setCursor(100, 140);
    tft.setTextColor(wifiMgr.isConnected() ? COL_GREEN : COL_AMBER, COL_BG);
    tft.println(wifiMgr.isConnected() ? "OK" : (wifiMgr.isAP() ? "AP" : "FAIL"));

    // Web server
    tft.setTextColor(COL_DKGREEN, COL_BG);
    tft.setCursor(30, 156);
    tft.print("WebServer...");
    webUI.begin();
    tft.setCursor(130, 156);
    tft.setTextColor(COL_GREEN, COL_BG);
    tft.println("OK");

    // Home Assistant MQTT
    if (strlen(settings.cfg.haHost) > 0) {
        tft.setTextColor(COL_DKGREEN, COL_BG);
        tft.setCursor(30, 172);
        tft.print("HA MQTT...");
        haMqtt.begin();
        tft.setCursor(100, 172);
        tft.setTextColor(COL_AMBER, COL_BG);
        tft.println("CONNECTING");
    }

    delay(1500);

    // Clear and draw initial UI
    tft.fillScreen(COL_BG);
    drawHeader();
    drawRadarPanel(false);

    lastShipSwitch = millis();
    lastFrameTime  = millis();
    lastClockDraw  = millis();

    Serial.printf("[Elite Clock] Ready. Ship: %s\n", getShipName(g_currentShip));
}

// ─────────────────────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    // ── Handle touch ─────────────────────────────────────────────────────────
    handleTouch();

    // ── Auto ship cycling ─────────────────────────────────────────────────────
    if (now - lastShipSwitch >= (uint32_t)g_shipDuration * 1000UL) {
        g_currentShip = (g_currentShip + 1) % NUM_SHIPS;
        settings.cfg.currentShip = g_currentShip;
        settings.save();
        g_shipChanged  = true;
        lastShipSwitch = now;
    }

    // ── If ship changed, redraw header ────────────────────────────────────────
    if (g_shipChanged) {
        g_shipChanged   = false;
        renderer.angleY = 0;      // reset rotation
        drawHeader();
        drawRadarPanel(g_showInfo);
    }

    // ── Render ship frame (~25 fps) ───────────────────────────────────────────
    if (now - lastFrameTime >= 40) {
        lastFrameTime = now;
        renderer.angleY += g_rotSpeed;
        if (renderer.angleY >= 360.0f) renderer.angleY -= 360.0f;
        renderShipFrame();
    }

    // ── Redraw clock every second ─────────────────────────────────────────────
    if (now - lastClockDraw >= 1000) {
        lastClockDraw = now;
        drawRadarPanel(g_showInfo);
    }

    // ── MQTT loop + periodic state publish (every 30s) ────────────────────────
    haMqtt.loop();
    if (now - lastHaPublish >= 30000) {
        lastHaPublish = now;
        haMqtt.publishState();
    }
}
