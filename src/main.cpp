// =============================================================================
//  main.cpp — Elite Clock main application
//  ESP32-C3 Super Mini + 2.8" UC8230 (ILI9341-compatible) 240×320 TFT
//
//  Screen layout (portrait 240×320):
//    ┌──────────────────────────────────┐  y=0
//    │  << [1/14] COBRA MK III      >> │  HDR_H=22 px  (touch: left=prev, right=next)
//    ├──────────────────────────────────┤  y=22
//    │                                  │
//    │   3-D ship sprite 240×208 px     │  SPRITE_H=208
//    │   [SHADED TOGGLE centre zone]    │  (touch: centre middle = toggle render mode)
//    │                                  │
//    ├──────────────────────────────────┤  y=230
//    │  HH:MM:SS         ONLINE/AP      │  RADAR_H=90
//    │  ○ Elite radar scanner  ○        │  (touch: tap = toggle ship info overlay)
//    └──────────────────────────────────┘  y=320
//
//  SPRITE NOTE: The ship sprite covers y=HDR_H..RADAR_Y (208px).
//  It is pushed to TFT at (0, HDR_H) so it never touches the header row.
//  The header is drawn directly onto TFT and only redrawn when ship changes
//  or shaded mode toggles.
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <math.h>
#include "ships.h"
#include "renderer.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "web_ui.h"
#include "ha_mqtt.h"

// ── Pin definitions ───────────────────────────────────────────────────────────
// TOUCH_CS is intentionally NOT defined via TFT_eSPI build flags — doing so
// causes TFT_eSPI's ESP32-C3 processor to include SPIFFS.h (for calibration
// storage), which is unavailable on Arduino-ESP32 2.x and breaks compilation.
// We drive the XPT2046_Touchscreen library directly using this local define.
#define TOUCH_CS_PIN   9    // XPT2046 chip select
#define TOUCH_IRQ_PIN  10   // XPT2046 interrupt (T_IRQ)

// ── Display layout ────────────────────────────────────────────────────────────
#define SCR_W       240
#define SCR_H       320
#define HDR_H        22     // header bar height (ship name + nav hints)
#define RADAR_Y     230     // radar/info panel top edge
#define RADAR_H      90     // radar panel height (320-230)
// Sprite covers the space BETWEEN header and radar:
#define SPRITE_Y    HDR_H
#define SPRITE_H    (RADAR_Y - HDR_H)   // = 208 px

// ── Touch zones (in screen coords) ───────────────────────────────────────────
// Header row — prev (left third) / next (right third)
#define PREV_X1   0
#define PREV_X2  79
#define NEXT_X1  161
#define NEXT_X2  239
#define NAV_Y1    0
#define NAV_Y2   (HDR_H + 20)   // extends slightly below header for easy tap

// Ship-view centre — shaded mode toggle
#define SHAD_X1   70
#define SHAD_X2  169
#define SHAD_Y1   (HDR_H + 80)
#define SHAD_Y2   (HDR_H + 140)

// Radar/info strip
#define RADAR_ZONE_Y1  RADAR_Y
#define RADAR_ZONE_Y2  SCR_H

// ── XPT2046 calibration — adjust to match your specific panel ─────────────────
//    Tip: print raw p.x / p.y values via Serial while touching corners,
//    then update these four values to map accurately.
#define TOUCH_XMIN   300
#define TOUCH_XMAX  3800
#define TOUCH_YMIN   300
#define TOUCH_YMAX  3800

// ── Colour palette (Elite phosphor-green on black) ────────────────────────────
#define COL_BG       TFT_BLACK
#define COL_GREEN    0x07E0u   // bright green
#define COL_DKGREEN  0x0300u   // dim green
#define COL_AMBER    0xFD20u   // amber warning
#define COL_CYAN     TFT_CYAN
#define COL_YELLOW   TFT_YELLOW
#define COL_BORDER   0x0260u   // dark green border line
#define COL_WHITE    TFT_WHITE

// ── Global state (shared with web_ui.h / ha_mqtt.h) ──────────────────────────
uint8_t  g_currentShip  = 0;
bool     g_shaded        = false;
float    g_rotSpeed      = 0.6f;
uint16_t g_shipDuration  = 30;
bool     g_shipChanged   = false;   // triggers header + radar redraw
int      g_shipChangeDir = 1;       // +1 next, -1 prev (for future transition FX)
bool     g_showInfo      = false;   // false = radar scanner, true = ship info overlay
bool     g_wifiReconnectPending = false;  // set by web UI to trigger reconnect

// ── Hardware instances ────────────────────────────────────────────────────────
TFT_eSPI            tft;
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);
Renderer            renderer;

// ── Timing ────────────────────────────────────────────────────────────────────
uint32_t lastShipSwitch = 0;
uint32_t lastFrameTime  = 0;
uint32_t lastClockDraw  = 0;
uint32_t lastHaPublish  = 0;
uint32_t lastTouchTime  = 0;
uint32_t lastOtaCheck   = 0;
bool     touchWasDown   = false;

// ── Off-screen frame buffer (only covers HDR_H → RADAR_Y) ────────────────────
//  Pushed at (0, SPRITE_Y) so it NEVER overwrites the header strip.
TFT_eSprite shipSprite = TFT_eSprite(&tft);

// ── Pre-computed star-field (stable per ship index) ──────────────────────────
struct Star { int16_t x, y; uint16_t col; };
static Star stars[48];

void seedStars(uint8_t shipIdx) {
    // Deterministic stars unique to each ship so they don't jump on rotation
    srand((uint32_t)shipIdx * 0x5A5A5A5Au + 0xDEADBEEFu);
    for (int i = 0; i < 48; i++) {
        stars[i].x   = rand() % SCR_W;
        stars[i].y   = rand() % SPRITE_H;
        uint8_t br   = rand() % 4;
        stars[i].col = (br == 3) ? COL_WHITE
                     : (br == 2) ? 0x4208u
                     : (br == 1) ? 0x2104u
                     :             0x1082u;
    }
}

// ── Helper accessors (used by web_ui.h / ha_mqtt.h) ──────────────────────────
const char* getShipName(int idx) {
    if (idx < 0 || idx >= NUM_SHIPS) return "Unknown";
    return SHIPS[idx].name;
}
int getNumShips() { return NUM_SHIPS; }

// =============================================================================
//  drawHeader()
//  Draws the 22-px strip at y=0..HDR_H directly on TFT (not in sprite).
//  Called once per ship change or shaded-mode toggle.
// =============================================================================
void drawHeader() {
    tft.fillRect(0, 0, SCR_W, HDR_H, COL_BG);
    tft.drawFastHLine(0, HDR_H - 1, SCR_W, COL_BORDER);

    // ── Render mode badge ─────────────────────────────────────────────────
    tft.setTextSize(1);
    if (g_shaded) {
        tft.setTextColor(COL_AMBER, COL_BG);
        tft.setCursor(2, 7);
        tft.print("SHAD");
    } else {
        tft.setTextColor(COL_DKGREEN, COL_BG);
        tft.setCursor(2, 7);
        tft.print("<< ");
    }

    // ── Ship name — centred ───────────────────────────────────────────────
    const char *name = SHIPS[g_currentShip].name;
    int tw = strlen(name) * 6;
    tft.setTextColor(COL_GREEN, COL_BG);
    tft.setCursor((SCR_W - tw) / 2, 7);
    tft.print(name);

    // ── Ship index (e.g. 3/14) ────────────────────────────────────────────
    char idx[8];
    snprintf(idx, sizeof(idx), "%d/%d", g_currentShip + 1, NUM_SHIPS);
    tft.setTextColor(COL_DKGREEN, COL_BG);
    // right-align: each char is 6px wide
    int idxW = strlen(idx) * 6;
    tft.setCursor(SCR_W - idxW - 2, 7);
    tft.print(idx);
}

// =============================================================================
//  drawRadarPanel()
//  Redraws the bottom 90px strip: clock + radar scanner OR ship info.
//  Called every second (clock tick) and on g_showInfo toggle.
// =============================================================================
void drawRadarPanel(bool showInfo) {
    const ShipDef &ship = SHIPS[g_currentShip];
    const int y = RADAR_Y;

    tft.fillRect(0, y, SCR_W, RADAR_H, COL_BG);
    tft.drawFastHLine(0, y, SCR_W, COL_BORDER);

    // ── Clock ─────────────────────────────────────────────────────────────
    struct tm ti;
    char timebuf[12] = "--:--:--";
    if (getLocalTime(&ti)) strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &ti);

    tft.setTextColor(COL_GREEN, COL_BG);
    tft.setTextSize(2);
    tft.setCursor(4, y + 5);
    tft.print(timebuf);

    // ── Connection status badge ───────────────────────────────────────────
    tft.setTextSize(1);
    if (wifiMgr.isAP()) {
        tft.setTextColor(COL_AMBER, COL_BG);
        tft.setCursor(168, y + 5);
        tft.print("AP MODE");
        tft.setCursor(168, y + 14);
        tft.setTextColor(COL_DKGREEN, COL_BG);
        tft.printf("%s", wifiMgr.localIP.c_str());
    } else if (wifiMgr.isConnected()) {
        tft.setTextColor(COL_DKGREEN, COL_BG);
        tft.setCursor(180, y + 5);
        tft.print("ONLINE");
    }

    if (showInfo) {
        // ── Ship data overlay ─────────────────────────────────────────────
        tft.setTextColor(COL_CYAN, COL_BG);
        tft.setTextSize(1);
        tft.setCursor(4, y + 24);
        tft.printf("CLASS : %-18s", ship.stats.role);

        tft.setTextColor(COL_GREEN, COL_BG);
        tft.setCursor(4, y + 36);
        tft.printf("SPEED : %-3d   LASER : %d  MSL: %d",
                    ship.stats.speed, ship.stats.lasers, ship.stats.missiles);
        tft.setCursor(4, y + 48);
        tft.printf("HULL  : %-3d", ship.stats.health);

        if (ship.stats.bounty > 0) {
            tft.setCursor(80, y + 48);
            tft.printf("BOUNTY: %u.%uCr",
                        ship.stats.bounty / 10, ship.stats.bounty % 10);
        }

        tft.setTextColor(COL_DKGREEN, COL_BG);
        tft.setCursor(4, y + 62);
        tft.printf("http://%s", wifiMgr.localIP.c_str());

        // Tap-hint
        tft.setCursor(4, y + 74);
        tft.print("[TAP FOR RADAR]");

    } else {
        // ── Elite-style elliptical radar scanner ──────────────────────────
        const int rx = SCR_W / 2;
        const int ry = y + 58;
        const int ra = 82;  // horizontal radius
        const int rb = 26;  // vertical radius

        // Outer and inner ellipses
        tft.drawEllipse(rx, ry, ra,     rb,     COL_DKGREEN);
        tft.drawEllipse(rx, ry, ra - 1, rb - 1, COL_DKGREEN);  // double thickness
        tft.drawEllipse(rx, ry, ra / 2, rb / 2, COL_DKGREEN);

        // Cross-hairs
        tft.drawFastHLine(rx - ra, ry, 2 * ra, COL_DKGREEN);
        tft.drawFastVLine(rx, ry - rb, 2 * rb, COL_DKGREEN);

        // Player dot (centre)
        tft.fillCircle(rx, ry, 2, COL_GREEN);

        // Target blip — position encodes ship index around the ellipse
        float ang = (float)g_currentShip / NUM_SHIPS * TWO_PI;
        int   bx  = rx + (int)(cosf(ang) * ra * 0.55f);
        int   by  = ry + (int)(sinf(ang) * rb * 0.55f);
        tft.fillCircle(bx, by, 3, SHIP_COLOURS[SHIPS[g_currentShip].colour]);
        tft.drawCircle(bx, by, 3, COL_GREEN);

        // Tap hint
        tft.setTextColor(COL_DKGREEN, COL_BG);
        tft.setTextSize(1);
        tft.setCursor(4, y + 78);
        tft.print("[TAP FOR INFO]");
    }
}

// =============================================================================
//  renderShipFrame()
//  Fills the 240×SPRITE_H sprite, renders the 3D ship, pushes to TFT at SPRITE_Y.
//  The sprite is positioned BELOW the header so it cannot overwrite it.
// =============================================================================
void renderShipFrame() {
    shipSprite.fillSprite(COL_BG);

    // ── Static star-field (pre-seeded per ship on ship change) ────────────
    for (int i = 0; i < 48; i++) {
        shipSprite.drawPixel(stars[i].x, stars[i].y, stars[i].col);
    }

    // ── Slow X-axis oscillation for 3D depth feel ─────────────────────────
    //    A gentle ±15° sine wave on X makes ships look much more alive.
    renderer.angleX = 15.0f + 12.0f * sinf(DEG_TO_RAD * renderer.angleY * 0.4f);

    // ── Sync renderer state from globals ─────────────────────────────────
    renderer.shaded = g_shaded;

    // ── Draw ─────────────────────────────────────────────────────────────
    renderer.render(shipSprite, SHIPS[g_currentShip]);

    // ── Push sprite below header, above radar ─────────────────────────────
    //   (0, SPRITE_Y) = (0, 22) on the TFT — header at y=0..21 is untouched
    shipSprite.pushSprite(0, SPRITE_Y);
}

// =============================================================================
//  flashTouchZone()
//  Brief green rectangle flash to give haptic-like visual feedback on touch.
// =============================================================================
void flashTouchZone(int x1, int y1, int x2, int y2) {
    tft.drawRect(x1, y1, x2 - x1, y2 - y1, COL_GREEN);
    delay(40);
    tft.drawRect(x1, y1, x2 - x1, y2 - y1, COL_BG);
}

// =============================================================================
//  handleTouch()
// =============================================================================
void handleTouch() {
    if (!touch.tirqTouched() || !touch.touched()) {
        touchWasDown = false;
        return;
    }
    if (touchWasDown)                          return;  // wait for lift
    if (millis() - lastTouchTime < 300)        return;  // debounce

    TS_Point p   = touch.getPoint();
    touchWasDown  = true;
    lastTouchTime = millis();

    // Map raw ADC → screen pixels
    int tx = (int)map(p.x, TOUCH_XMIN, TOUCH_XMAX, 0, SCR_W - 1);
    int ty = (int)map(p.y, TOUCH_YMIN, TOUCH_YMAX, 0, SCR_H - 1);
    tx = constrain(tx, 0, SCR_W - 1);
    ty = constrain(ty, 0, SCR_H - 1);

    Serial.printf("[Touch] raw(%d,%d) → screen(%d,%d)\n", p.x, p.y, tx, ty);

    // ── PREV ship ─────────────────────────────────────────────────────────
    if (tx <= PREV_X2 && ty <= NAV_Y2) {
        flashTouchZone(PREV_X1, NAV_Y1, PREV_X2, NAV_Y2);
        g_currentShip    = (g_currentShip + NUM_SHIPS - 1) % NUM_SHIPS;
        g_shipChangeDir  = -1;
        settings.cfg.currentShip = g_currentShip;
        settings.save();
        seedStars(g_currentShip);
        g_shipChanged  = true;
        lastShipSwitch = millis();
        return;
    }

    // ── NEXT ship ─────────────────────────────────────────────────────────
    if (tx >= NEXT_X1 && ty <= NAV_Y2) {
        flashTouchZone(NEXT_X1, NAV_Y1, NEXT_X2, NAV_Y2);
        g_currentShip    = (g_currentShip + 1) % NUM_SHIPS;
        g_shipChangeDir  = +1;
        settings.cfg.currentShip = g_currentShip;
        settings.save();
        seedStars(g_currentShip);
        g_shipChanged  = true;
        lastShipSwitch = millis();
        return;
    }

    // ── Shaded toggle (centre of ship view) ───────────────────────────────
    if (tx >= SHAD_X1 && tx <= SHAD_X2 && ty >= SHAD_Y1 && ty <= SHAD_Y2) {
        g_shaded = !g_shaded;
        settings.cfg.shaded = g_shaded;
        settings.save();
        drawHeader();  // update [SHAD] / << badge
        return;
    }

    // ── Radar / info toggle ───────────────────────────────────────────────
    if (ty >= RADAR_ZONE_Y1) {
        g_showInfo = !g_showInfo;
        drawRadarPanel(g_showInfo);
        return;
    }
}

// =============================================================================
//  SETUP
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println("\n\n╔══════════════════════╗");
    Serial.println(  "║   ELITE CLOCK v1.1   ║");
    Serial.println(  "╚══════════════════════╝");

    // ── Load persistent settings ──────────────────────────────────────────
    settings.begin();
    g_currentShip  = settings.cfg.currentShip;
    // Clamp: if NUM_SHIPS decreased since last save, index could be out of range
    if (g_currentShip >= NUM_SHIPS) {
        g_currentShip = 0;
        settings.cfg.currentShip = 0;
        settings.save();
    }
    g_shaded       = settings.cfg.shaded;
    g_rotSpeed     = settings.cfg.rotSpeed;
    g_shipDuration = settings.cfg.shipDuration;

    // ── TFT init ──────────────────────────────────────────────────────────
    tft.init();
    tft.setRotation(0);
    tft.fillScreen(COL_BG);
    tft.setTextFont(1);
    tft.setTextSize(1);

    // ── Boot splash ───────────────────────────────────────────────────────
    tft.setTextColor(COL_GREEN, COL_BG);
    tft.setTextSize(2);
    tft.setCursor(18, 50);  tft.print("** ELITE CLOCK **");
    tft.setTextSize(1);
    tft.setTextColor(COL_DKGREEN, COL_BG);
    tft.setCursor(60, 80);  tft.print("ESP32-C3 v1.1");
    tft.setCursor(36, 94);  tft.print("240x320 UC8230 TFT");

    auto splashLine = [&](int row, const char *label) {
        tft.setTextColor(COL_DKGREEN, COL_BG);
        tft.setCursor(20, 120 + row * 16);
        tft.print(label);
    };
    splashLine(0, "INIT DISPLAY    ...");
    splashLine(1, "INIT TOUCH      ...");
    splashLine(2, "WIFI            ...");
    splashLine(3, "WEB SERVER      ...");
    splashLine(4, "MDNS            ...");
    splashLine(5, "OTA             ...");
    splashLine(6, "HA MQTT         ...");

    auto splashOK = [&](int row, const char *msg, uint16_t col) {
        tft.setTextColor(col, COL_BG);
        tft.setCursor(140, 120 + row * 16);
        tft.print(msg);
    };

    // ── Create ship sprite ────────────────────────────────────────────────
    //   Size: SCR_W × SPRITE_H (240 × 208). Pushed at y=SPRITE_Y=22.
    if (!shipSprite.createSprite(SCR_W, SPRITE_H)) {
        Serial.println("[ERROR] Sprite allocation failed! Check PSRAM.");
    }
    shipSprite.setColorDepth(16);
    splashOK(0, "OK", COL_GREEN);

    // ── Touchscreen ───────────────────────────────────────────────────────
    touch.begin();
    touch.setRotation(1);
    splashOK(1, "OK", COL_GREEN);

    // ── Seed stars for initial ship ───────────────────────────────────────
    seedStars(g_currentShip);

    // ── Wi-Fi ─────────────────────────────────────────────────────────────
    wifiMgr.begin();
    if (wifiMgr.isConnected()) {
        splashOK(2, "CONNECTED", COL_GREEN);
        tft.setCursor(20, 120 + 2 * 16 + 8);
        tft.setTextColor(COL_DKGREEN, COL_BG);
        tft.printf("  %s", wifiMgr.localIP.c_str());
    } else {
        splashOK(2, wifiMgr.isAP() ? "AP STARTED" : "NO WIFI", COL_AMBER);
    }

    // ── Web server ────────────────────────────────────────────────────────
    webUI.begin();
    splashOK(3, "OK", COL_GREEN);

    // ── mDNS — device accessible as http://eliteclock.local ──────────────
    if (wifiMgr.isConnected()) {
        if (MDNS.begin("eliteclock")) {
            MDNS.addService("http", "tcp", 80);
            Serial.println("[mDNS] http://eliteclock.local");
            splashOK(4, "eliteclock.local", COL_GREEN);
        } else {
            splashOK(4, "FAILED", COL_AMBER);
        }
    } else {
        splashOK(4, "N/A", COL_DKGREEN);
    }

    // ── OTA firmware update ───────────────────────────────────────────────
    if (wifiMgr.isConnected()) {
        ArduinoOTA.setHostname("eliteclock");
        ArduinoOTA.setPassword("eliteota");   // change as desired

        ArduinoOTA.onStart([]() {
            tft.fillScreen(COL_BG);
            tft.setTextColor(COL_GREEN, COL_BG);
            tft.setTextSize(2);
            tft.setCursor(20, 80);
            tft.println("OTA UPDATE");
            tft.setTextSize(1);
            tft.setCursor(20, 110);
            tft.println("Do not power off...");
        });
        ArduinoOTA.onProgress([](unsigned int prog, unsigned int total) {
            int pct = prog * 100 / total;
            tft.fillRect(10, 140, 220, 16, COL_BG);
            tft.fillRect(10, 140, pct * 220 / 100, 14, COL_DKGREEN);
            tft.setTextColor(COL_GREEN, COL_BG);
            tft.setCursor(104, 142);
            tft.printf("%3d%%", pct);
        });
        ArduinoOTA.onEnd([]() {
            tft.setCursor(20, 165);
            tft.setTextColor(COL_GREEN, COL_BG);
            tft.println("Complete - rebooting");
        });
        ArduinoOTA.onError([](ota_error_t err) {
            (void)err; // Serial not available inside a no-capture lambda
        });
        ArduinoOTA.begin();
        splashOK(5, "READY (:8266)", COL_GREEN);
    } else {
        splashOK(5, "N/A", COL_DKGREEN);
    }

    // ── Home Assistant MQTT (always init; connects only when host is set) ─
    haMqtt.begin();
    splashOK(6, strlen(settings.cfg.haHost) > 0 ? "CONNECTING" : "NO HOST", COL_AMBER);

    delay(1800);

    // ── Build initial screen ──────────────────────────────────────────────
    tft.fillScreen(COL_BG);
    drawHeader();
    drawRadarPanel(false);

    lastShipSwitch = millis();
    lastFrameTime  = millis();
    lastClockDraw  = millis();

    Serial.printf("[Elite Clock] Running — ship: %s  IP: %s\n",
                  getShipName(g_currentShip), wifiMgr.localIP.c_str());
}

// =============================================================================
//  LOOP
// =============================================================================
void loop() {
    uint32_t now = millis();

    // ── OTA handler ───────────────────────────────────────────────────────
    ArduinoOTA.handle();

    // ── Captive portal DNS (AP mode only, pumped in wifi_mgr) ────────────
    wifiMgr.dnsLoop();

    // ── Touch ─────────────────────────────────────────────────────────────
    handleTouch();

    // ── Auto ship cycling ─────────────────────────────────────────────────
    if (now - lastShipSwitch >= (uint32_t)g_shipDuration * 1000UL) {
        g_currentShip    = (g_currentShip + 1) % NUM_SHIPS;
        g_shipChangeDir  = +1;
        settings.cfg.currentShip = g_currentShip;
        settings.save();
        seedStars(g_currentShip);
        g_shipChanged  = true;
        lastShipSwitch = now;
    }

    // ── Ship-changed → refresh static UI elements ─────────────────────────
    if (g_shipChanged) {
        g_shipChanged   = false;
        renderer.angleY = 0.0f;
        seedStars(g_currentShip);   // update star-field for new ship
        drawHeader();
        drawRadarPanel(g_showInfo);
    }

    // ── 3D render frame (~25 fps, 40ms cadence) ───────────────────────────
    if (now - lastFrameTime >= 40) {
        lastFrameTime   = now;
        renderer.angleY += g_rotSpeed;
        if (renderer.angleY >= 360.0f) renderer.angleY -= 360.0f;
        renderShipFrame();
    }

    // ── Clock tick (once per second) ──────────────────────────────────────
    if (now - lastClockDraw >= 1000) {
        lastClockDraw = now;
        drawRadarPanel(g_showInfo);
    }

    // ── Deferred Wi-Fi reconnect (triggered by web UI POST) ──────────────
    if (g_wifiReconnectPending) {
        g_wifiReconnectPending = false;
        bool ok = wifiMgr.reconnect(settings.cfg.wifiSSID, settings.cfg.wifiPass);
        if (ok) {
            MDNS.begin("eliteclock");
            MDNS.addService("http", "tcp", 80);
            ArduinoOTA.begin();
        }
        drawRadarPanel(g_showInfo);  // refresh IP display
    }

    // ── HA MQTT loop + periodic state publish ────────────────────────────
    haMqtt.loop();
    if (now - lastHaPublish >= 30000) {
        lastHaPublish = now;
        haMqtt.publishState();
    }
}
