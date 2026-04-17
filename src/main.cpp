// =============================================================================
//  main.cpp — Elite Clock v1.2
//  ST7796 480×320 landscape, BBC Micro Elite colour scheme
//
//  Screen layout (landscape 480×320):
//  ┌──────────────────────────────────────────────────────────────────────┐ y=0
//  │  <<  COBRA MK III              14:37:52                    1/19  >> │ 22px
//  ├───────────────┬────────────────────────┬────────────────────────────┤ y=22
//  │ INSERVICE:    │                        │ COMBAT FACTOR:             │
//  │  3100         │   3-D SHIP SPRITE      │  CF7                       │
//  │  (Lave Orb.)  │   180 × 218 px         │ CREW: 1-2                  │
//  │ DIMENSIONS:   │                        │ ARMAMENTS:                 │
//  │  65/30/65ft   │   (touch centre zone   │  2x Beam Laser             │
//  │ SPEED:        │    = shaded toggle)    │  3 Missiles                │
//  │  0.35LM       │                        │ BOUNTY: None               │
//  │ HULL: 150     │                        │ CLASS: Fighter             │
//  │ DRIVE:        │                        │                            │
//  │  Voltaire     │                        │                            │
//  │  Whiplash     │                        │                            │
//  ├───────────────┴──────┬─────────────────┴────────────┬───────────────┤ y=240
//  │  [ANIM BARS]         │  RADAR ELLIPSE (seconds dot) │  [ANIM BARS]  │
//  │                      │       * ELITE *              │               │ 80px
//  └──────────────────────┴──────────────────────────────┴───────────────┘ y=320
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
#define TOUCH_CS_PIN   9
#define TOUCH_IRQ_PIN  10

// ── Display layout (landscape 480×320) ───────────────────────────────────────
#define SCR_W         480
#define SCR_H         320
#define HDR_H          22   // title bar
#define LEFT_W        150   // left info panel width
#define RIGHT_W       150   // right info panel width
#define RADAR_H        80   // bottom dashboard strip
#define RADAR_Y       (SCR_H - RADAR_H)            // = 240

// Ship sprite geometry
#define SHIP_SW       (SCR_W - LEFT_W - RIGHT_W)   // = 180
#define SHIP_SH       (SCR_H - HDR_H - RADAR_H)    // = 218
#define SHIP_SX       LEFT_W                        // = 150  push x
#define SHIP_SY       HDR_H                         // = 22   push y

// ── Touch zones ───────────────────────────────────────────────────────────────
#define PREV_X1    0
#define PREV_X2   79
#define NEXT_X1  400
#define NEXT_X2  479
#define NAV_Y2    40
#define SHAD_X1  165
#define SHAD_X2  315
#define SHAD_Y1  (HDR_H + 60)
#define SHAD_Y2  (HDR_H + 178)

// ── XPT2046 calibration — adjust to match your panel ────────────────────────
#define TOUCH_XMIN   300
#define TOUCH_XMAX  3800
#define TOUCH_YMIN   300
#define TOUCH_YMAX  3800

// ── BBC Micro Elite colour palette ────────────────────────────────────────────
#define COL_BG       TFT_BLACK
#define COL_WHITE    TFT_WHITE
#define COL_YELLOW   0xFFE0u   // bright yellow — main text / ship name
#define COL_CYAN     0x07FFu   // cyan — panel labels
#define COL_GREEN    0x07E0u   // green — radar / border
#define COL_DKGREEN  0x0320u   // dim green
#define COL_RED      0xF800u   // red
#define COL_ORANGE   0xFD20u   // amber/orange
#define COL_BORDER   0x0260u   // dark green separator lines
#define COL_MAGENTA  0xF81Fu

// Dashboard bar colours — bottom (bright) to top (dim), BBC palette order
static const uint16_t BAR_BRIGHT[6] = {
    0x07FFu, 0x07E0u, 0xAFE0u, 0xFFE0u, 0xFD20u, 0xF800u
};
static const uint16_t BAR_DIM[6] = {
    0x0180u, 0x0300u, 0x2560u, 0x6460u, 0x5820u, 0x4000u
};

// ── Global state (shared with web_ui.h / ha_mqtt.h) ──────────────────────────
uint8_t  g_currentShip  = 0;
bool     g_shaded        = false;
float    g_rotSpeed      = 0.6f;
uint16_t g_shipDuration  = 30;
bool     g_shipChanged   = false;
int      g_shipChangeDir = 1;
bool     g_showInfo      = false;
bool     g_wifiReconnectPending = false;
bool     g_spriteOK      = false;

// ── Hardware ──────────────────────────────────────────────────────────────────
TFT_eSPI            tft;
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);
Renderer            renderer;

// ── Timing ────────────────────────────────────────────────────────────────────
uint32_t lastShipSwitch = 0;
uint32_t lastFrameTime  = 0;
uint32_t lastClockDraw  = 0;
uint32_t lastHaPublish  = 0;
uint32_t lastTouchTime  = 0;
bool     touchWasDown   = false;

// ── Ship sprite (180 × 218, pushed at 150,22) ─────────────────────────────────
TFT_eSprite shipSprite = TFT_eSprite(&tft);

// ── Star-field (pre-computed per ship) ────────────────────────────────────────
struct Star { int16_t x, y; uint16_t col; };
static Star stars[48];

void seedStars(uint8_t idx) {
    srand((uint32_t)idx * 0x5A5A5A5Au + 0xDEADBEEFu);
    for (int i = 0; i < 48; i++) {
        stars[i].x = rand() % SHIP_SW;
        stars[i].y = rand() % SHIP_SH;
        uint8_t br = rand() % 4;
        stars[i].col = br == 3 ? (uint16_t)TFT_WHITE
                     : br == 2 ? 0x4208u
                     : br == 1 ? 0x2104u : 0x1082u;
    }
}

// ── Accessors (used by web_ui.h / ha_mqtt.h) ──────────────────────────────────
const char* getShipName(int idx) {
    if (idx < 0 || idx >= NUM_SHIPS) return "Unknown";
    return SHIPS[idx].name;
}
int getNumShips() { return NUM_SHIPS; }

// ── Laser type string ─────────────────────────────────────────────────────────
static const char* laserStr(uint8_t l) {
    if (l == 0) return "None";
    if (l == 1) return "Pulse";
    if (l == 2) return "Beam";
    if (l <= 4) return "Military";
    return "Heavy Mltry";
}

// =============================================================================
//  drawAnimBars()
//  Draws 6 animated horizontal bar segments in BBC Elite dashboard style.
//  x/y = top-left corner, w = bar width.
//  A "shimmer" cycles up through the bars using millis().
// =============================================================================
void drawAnimBars(int16_t x, int16_t y, int16_t w, uint32_t now) {
    int active = (now / 120) % 9;  // which bar briefly lights to white (0-8, 8=none)
    for (int i = 0; i < 6; i++) {
        // i=0 drawn at bottom, i=5 at top
        int16_t by = y + (5 - i) * 13;
        uint16_t col = (active == i) ? COL_WHITE : BAR_DIM[i];
        tft.fillRect(x, by, w, 10, col);
    }
}

// =============================================================================
//  drawHeader()
//  Full-width 22px title bar: ship name centred, clock left, index right.
//  Drawn directly on TFT (not in sprite). Redrawn on ship/shaded change.
// =============================================================================
void drawHeader() {
    tft.fillRect(0, 0, SCR_W, HDR_H, COL_BG);
    tft.drawFastHLine(0, HDR_H - 1, SCR_W, COL_BORDER);

    tft.setTextSize(1);

    // Nav hints
    tft.setTextColor(COL_DKGREEN, COL_BG);
    tft.setCursor(2, 7);
    tft.print("<<");
    tft.setCursor(SCR_W - 14, 7);
    tft.print(">>");

    // Ship name — large, centred, yellow
    const char *name = SHIPS[g_currentShip].name;
    tft.setTextSize(2);
    int tw = strlen(name) * 12;
    tft.setTextColor(COL_YELLOW, COL_BG);
    tft.setCursor((SCR_W - tw) / 2, 4);
    tft.print(name);

    // Shaded mode badge left of ship name (small)
    if (g_shaded) {
        tft.setTextSize(1);
        tft.setTextColor(COL_ORANGE, COL_BG);
        tft.setCursor(20, 7);
        tft.print("[SHAD]");
    }

    // Ship index right-aligned
    char idx[8];
    snprintf(idx, sizeof(idx), "%d/%d", g_currentShip + 1, NUM_SHIPS);
    tft.setTextSize(1);
    tft.setTextColor(COL_DKGREEN, COL_BG);
    tft.setCursor(SCR_W - 30, 7);
    tft.print(idx);
}

// =============================================================================
//  drawShipInfo()
//  Left panel (x=0..149) and right panel (x=330..479), y=22..239.
//  BBC Encyclopedia Galactica style — cyan labels, yellow values.
//  Called once per ship change (static content).
// =============================================================================
void drawShipInfo() {
    const ShipDef  &ship = SHIPS[g_currentShip];
    const ShipStats &st  = ship.stats;
    const ShipLore  *lr  = ship.lore;

    // ── Clear both panels ─────────────────────────────────────────────────
    tft.fillRect(0,          HDR_H, LEFT_W,  SHIP_SH, COL_BG);
    tft.fillRect(SCR_W - RIGHT_W, HDR_H, RIGHT_W, SHIP_SH, COL_BG);

    // Vertical separator lines
    tft.drawFastVLine(LEFT_W - 1,          HDR_H, SHIP_SH, COL_BORDER);
    tft.drawFastVLine(SCR_W - RIGHT_W,     HDR_H, SHIP_SH, COL_BORDER);

    tft.setTextSize(1);

    // ── Left panel: lore & performance ───────────────────────────────────
    int lx = 3;
    int ly = HDR_H + 5;
    auto lbl = [&](const char *s) {
        tft.setTextColor(COL_CYAN, COL_BG);
        tft.setCursor(lx, ly);
        tft.print(s);
        ly += 10;
    };
    auto val = [&](const char *s) {
        tft.setTextColor(COL_YELLOW, COL_BG);
        tft.setCursor(lx + 4, ly);
        tft.print(s);
        ly += 10;
    };
    auto valFmt = [&](const char *fmt, ...) {
        char buf[32];
        va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        val(buf);
    };

    lbl("INSERVICE:");
    // Split "3100 (Lave Orbital)" at the space before '('
    if (lr) {
        const char *p = strchr(lr->inservice, '(');
        if (p && p != lr->inservice) {
            char yr[16] = {};
            strncpy(yr, lr->inservice, (int)(p - lr->inservice) - 1);
            val(yr);
            val(p);
        } else {
            val(lr->inservice);
        }
    }
    ly += 4;

    lbl("DIMENSIONS:");
    if (lr) { char tmp[24]; snprintf(tmp,sizeof(tmp),"%sft",lr->dimensions); val(tmp); }
    ly += 4;

    lbl("SPEED:");
    valFmt("%.2fLM", st.speed * 0.0125f);
    ly += 4;

    lbl("HULL:");
    valFmt("%d units", st.health);
    ly += 4;

    lbl("DRIVE MOTORS:");
    if (lr) {
        // Wrap drive name if longer than ~22 chars
        const char *d = lr->drive;
        if (strlen(d) > 18) {
            char line1[20] = {}, line2[20] = {};
            // Split at last space before char 18
            const char *sp = nullptr;
            for (const char *c = d; c < d+18 && *c; c++) if (*c==' ') sp=c;
            if (sp) {
                strncpy(line1, d, (int)(sp-d));
                val(line1);
                val(sp+1);
            } else {
                val(d);
            }
        } else {
            val(d);
        }
    }

    // ── Right panel: combat stats ─────────────────────────────────────────
    int rx = SCR_W - RIGHT_W + 3;
    int ry = HDR_H + 5;
    auto rlbl = [&](const char *s) {
        tft.setTextColor(COL_CYAN, COL_BG);
        tft.setCursor(rx, ry);
        tft.print(s);
        ry += 10;
    };
    auto rval = [&](const char *s) {
        tft.setTextColor(COL_YELLOW, COL_BG);
        tft.setCursor(rx + 4, ry);
        tft.print(s);
        ry += 10;
    };
    auto rvalFmt = [&](const char *fmt, ...) {
        char buf[32];
        va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        rval(buf);
    };

    rlbl("COMBAT FACTOR:");
    rvalFmt("CF%d", st.lasers * 2 + st.missiles);
    ry += 4;

    rlbl("CREW:");
    if (lr) rval(lr->crew); else rvalFmt("1-%d", 1 + st.missiles / 2);
    ry += 4;

    rlbl("ARMAMENTS:");
    if (st.lasers > 0)   rvalFmt("%dx %s", st.lasers, laserStr(st.lasers));
    else                 rval("None");
    if (st.missiles > 0) rvalFmt("%d Missile%s", st.missiles, st.missiles>1?"s":"");
    ry += 4;

    rlbl("BOUNTY:");
    if (st.bounty > 0) rvalFmt("%u.%uCr", st.bounty/10, st.bounty%10);
    else               rval("None");
    ry += 4;

    rlbl("CLASS:");
    rval(st.role);
}

// =============================================================================
//  drawRadarPanel()
//  Bottom 80px: clock row + animated bars + radar ellipse (seconds) + ELITE.
//  Called every second from loop().
// =============================================================================
void drawRadarPanel(bool showInfo) {
    const int y = RADAR_Y;
    uint32_t now = millis();

    tft.fillRect(0, y, SCR_W, RADAR_H, COL_BG);
    tft.drawFastHLine(0, y, SCR_W, COL_BORDER);

    // ── Clock — top-left of radar strip ──────────────────────────────────
    struct tm ti;
    char timebuf[12] = "--:--:--";
    int  curSec = 0;
    if (getLocalTime(&ti)) {
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &ti);
        curSec = ti.tm_sec;
    }
    tft.setTextColor(COL_GREEN, COL_BG);
    tft.setTextSize(2);
    tft.setCursor(4, y + 4);
    tft.print(timebuf);

    // Connection badge
    tft.setTextSize(1);
    if (wifiMgr.isAP()) {
        tft.setTextColor(COL_ORANGE, COL_BG);
        tft.setCursor(4, y + 24);
        tft.printf("AP: %s", wifiMgr.localIP.c_str());
    } else if (wifiMgr.isConnected()) {
        tft.setTextColor(COL_DKGREEN, COL_BG);
        tft.setCursor(4, y + 24);
        tft.printf("%s", wifiMgr.localIP.c_str());
    }

    // ── Animated bars — left side ─────────────────────────────────────────
    // bars start below clock text: x=4, y=y+38, w=55
    drawAnimBars(4, y + 38, 55, now);

    // ── Animated bars — right side ────────────────────────────────────────
    drawAnimBars(SCR_W - 60, y + 38, 55, now + 300); // phase-shifted

    // ── Radar ellipse — centred in strip ─────────────────────────────────
    const int rx  = SCR_W / 2;   // = 240
    const int ry2 = y + 44;
    const int ra  = 78;           // horizontal radius
    const int rb  = 24;           // vertical radius

    tft.drawEllipse(rx, ry2, ra, rb, COL_GREEN);
    tft.drawEllipse(rx, ry2, ra-1, rb-1, COL_DKGREEN); // double border
    tft.drawEllipse(rx, ry2, ra/2, rb/2, COL_DKGREEN);
    tft.drawFastHLine(rx - ra, ry2, 2*ra, COL_DKGREEN);
    tft.drawFastVLine(rx, ry2 - rb, 2*rb, COL_DKGREEN);

    // Player centre dot
    tft.fillCircle(rx, ry2, 3, COL_GREEN);

    // Seconds blip — orbits the ellipse once per minute (0s=top/12-o'clock)
    float secAngle = (curSec * 6.0f - 90.0f) * DEG_TO_RAD;
    int bx = rx + (int)(cosf(secAngle) * ra * 0.65f);
    int by = ry2 + (int)(sinf(secAngle) * rb * 0.65f);
    tft.fillCircle(bx, by, 4, COL_YELLOW);
    tft.drawCircle(bx, by, 4, COL_WHITE);

    // ── ELITE logo — centred below radar ─────────────────────────────────
    tft.setTextColor(COL_YELLOW, COL_BG);
    tft.setTextSize(2);
    tft.setCursor(rx - 30, y + 62);
    tft.print("ELITE");

    // ── Show/hide touch hint ──────────────────────────────────────────────
    tft.setTextSize(1);
    tft.setTextColor(COL_DKGREEN, COL_BG);
    tft.setCursor(SCR_W - 100, y + 4);
    tft.print(showInfo ? "[TAP:RADAR]" : "[TAP:INFO]");
}

// =============================================================================
//  renderShipFrame()
//  Renders 3D ship into 180×218 sprite, pushes at (150, 22).
// =============================================================================
void renderShipFrame() {
    if (!g_spriteOK) {
        // Low memory fallback — clear ship area and skip
        tft.fillRect(SHIP_SX, SHIP_SY, SHIP_SW, SHIP_SH, COL_BG);
        return;
    }

    shipSprite.fillSprite(COL_BG);

    // Star-field
    for (int i = 0; i < 48; i++) {
        shipSprite.drawPixel(stars[i].x, stars[i].y, stars[i].col);
    }

    // X-axis gentle oscillation for 3-D depth
    renderer.angleX = 15.0f + 12.0f * sinf(DEG_TO_RAD * renderer.angleY * 0.4f);
    renderer.shaded = g_shaded;
    renderer.render(shipSprite, SHIPS[g_currentShip]);

    shipSprite.pushSprite(SHIP_SX, SHIP_SY);
}

// =============================================================================
//  flashTouchZone() — brief border flash as visual tap feedback
// =============================================================================
void flashTouchZone(int x1, int y1, int x2, int y2) {
    tft.drawRect(x1, y1, x2-x1, y2-y1, COL_GREEN);
    delay(35);
    tft.drawRect(x1, y1, x2-x1, y2-y1, COL_BG);
}

// =============================================================================
//  handleTouch()
// =============================================================================
void handleTouch() {
    if (!touch.tirqTouched() || !touch.touched()) {
        touchWasDown = false;
        return;
    }
    if (touchWasDown)                   return;
    if (millis() - lastTouchTime < 300) return;

    TS_Point p   = touch.getPoint();
    touchWasDown  = true;
    lastTouchTime = millis();

    // For landscape with rotation(1), XPT2046 raw axes may be swapped.
    // If prev/next don't respond, swap tx/ty here and re-calibrate.
    int tx = (int)map(p.x, TOUCH_XMIN, TOUCH_XMAX, 0, SCR_W - 1);
    int ty = (int)map(p.y, TOUCH_YMIN, TOUCH_YMAX, 0, SCR_H - 1);
    tx = constrain(tx, 0, SCR_W - 1);
    ty = constrain(ty, 0, SCR_H - 1);

    Serial.printf("[Touch] raw(%d,%d) screen(%d,%d)\n", p.x, p.y, tx, ty);

    // PREV — top-left
    if (tx <= PREV_X2 && ty <= NAV_Y2) {
        flashTouchZone(PREV_X1, 0, PREV_X2, NAV_Y2);
        g_currentShip = (g_currentShip + NUM_SHIPS - 1) % NUM_SHIPS;
        settings.cfg.currentShip = g_currentShip;
        settings.save();
        seedStars(g_currentShip);
        g_shipChanged  = true;
        lastShipSwitch = millis();
        return;
    }

    // NEXT — top-right
    if (tx >= NEXT_X1 && ty <= NAV_Y2) {
        flashTouchZone(NEXT_X1, 0, NEXT_X2, NAV_Y2);
        g_currentShip = (g_currentShip + 1) % NUM_SHIPS;
        settings.cfg.currentShip = g_currentShip;
        settings.save();
        seedStars(g_currentShip);
        g_shipChanged  = true;
        lastShipSwitch = millis();
        return;
    }

    // SHADED toggle — centre of ship view
    if (tx >= SHAD_X1 && tx <= SHAD_X2 && ty >= SHAD_Y1 && ty <= SHAD_Y2) {
        g_shaded = !g_shaded;
        settings.cfg.shaded = g_shaded;
        settings.save();
        drawHeader();
        return;
    }

    // RADAR / info toggle — bottom strip
    if (ty >= RADAR_Y) {
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
    delay(200);

    // 1. Settings
    settings.begin();
    g_currentShip = settings.cfg.currentShip;
    if (g_currentShip >= NUM_SHIPS) { g_currentShip = 0; settings.cfg.currentShip = 0; settings.save(); }
    g_shaded       = settings.cfg.shaded;
    g_rotSpeed     = settings.cfg.rotSpeed;
    g_shipDuration = settings.cfg.shipDuration;

    // 2. WiFi first — AP visible even if display hangs
    wifiMgr.begin();

    // 3. Web server
    webUI.begin();

    // 4. mDNS
    if (wifiMgr.isConnected()) { MDNS.begin("eliteclock"); MDNS.addService("http","tcp",80); }

    // 5. OTA
    if (wifiMgr.isConnected()) {
        ArduinoOTA.setHostname("eliteclock");
        ArduinoOTA.setPassword("eliteota");
        ArduinoOTA.onStart([]() {
            tft.fillScreen(COL_BG);
            tft.setTextColor(COL_YELLOW, COL_BG); tft.setTextSize(2);
            tft.setCursor(120, 130); tft.println("OTA UPDATE");
            tft.setTextSize(1); tft.setCursor(130, 160); tft.println("Do not power off...");
        });
        ArduinoOTA.onProgress([](unsigned int prog, unsigned int total) {
            int pct = prog * 100 / total;
            tft.fillRect(80, 185, 320, 16, COL_BG);
            tft.fillRect(80, 185, pct * 320 / 100, 14, COL_DKGREEN);
            tft.setTextColor(COL_GREEN, COL_BG); tft.setTextSize(1);
            tft.setCursor(230, 187); tft.printf("%3d%%", pct);
        });
        ArduinoOTA.onEnd([]() {
            tft.setCursor(150, 210); tft.setTextColor(COL_GREEN, COL_BG);
            tft.println("Complete - rebooting");
        });
        ArduinoOTA.onError([](ota_error_t e) { (void)e; });
        ArduinoOTA.begin();
    }

    // 6. MQTT
    haMqtt.begin();

    // 7. Display (landscape, rotation 1)
    tft.init();
    tft.setRotation(1);   // landscape: 480×320
    tft.fillScreen(COL_BG);
    tft.setTextFont(1);
    tft.setTextSize(1);

    // Boot splash
    tft.setTextColor(COL_YELLOW, COL_BG); tft.setTextSize(2);
    tft.setCursor(130, 100); tft.print("** ELITE CLOCK **");
    tft.setTextSize(1); tft.setTextColor(COL_GREEN, COL_BG);
    tft.setCursor(180, 130); tft.print("BBC Micro Edition");
    tft.setCursor(155, 145); tft.printf("480x320 ST7796 v1.2");

    tft.setCursor(160, 170);
    if (wifiMgr.isConnected()) {
        tft.setTextColor(COL_GREEN, COL_BG);
        tft.printf("WIFI: %s", wifiMgr.localIP.c_str());
    } else if (wifiMgr.isAP()) {
        tft.setTextColor(COL_ORANGE, COL_BG);
        tft.printf("AP: %s  %s", AP_SSID, wifiMgr.localIP.c_str());
    }

    // 8. Sprite (180×218)
    g_spriteOK = (bool)shipSprite.createSprite(SHIP_SW, SHIP_SH);
    if (g_spriteOK) {
        shipSprite.setColorDepth(16);
    } else {
        tft.setTextColor(COL_ORANGE, COL_BG);
        tft.setCursor(160, 190); tft.print("LOW MEM — no sprite");
    }

    // 9. Touch + stars
    touch.begin();
    touch.setRotation(1);
    seedStars(g_currentShip);

    delay(1200);

    // 10. Main UI
    tft.fillScreen(COL_BG);
    drawHeader();
    drawShipInfo();
    drawRadarPanel(false);

    lastShipSwitch = millis();
    lastFrameTime  = millis();
    lastClockDraw  = millis();

    Serial.printf("[Elite Clock] Ready  ship=%s  ip=%s\n",
                  getShipName(g_currentShip), wifiMgr.localIP.c_str());
}

// =============================================================================
//  LOOP
// =============================================================================
void loop() {
    uint32_t now = millis();

    ArduinoOTA.handle();
    wifiMgr.dnsLoop();
    handleTouch();

    // Auto-cycle ships
    if (now - lastShipSwitch >= (uint32_t)g_shipDuration * 1000UL) {
        g_currentShip    = (g_currentShip + 1) % NUM_SHIPS;
        settings.cfg.currentShip = g_currentShip;
        settings.save();
        seedStars(g_currentShip);
        g_shipChanged  = true;
        lastShipSwitch = now;
    }

    // Ship changed — redraw all static elements
    if (g_shipChanged) {
        g_shipChanged   = false;
        renderer.angleY = 0.0f;
        seedStars(g_currentShip);
        drawHeader();
        drawShipInfo();
        drawRadarPanel(g_showInfo);
    }

    // 3D render frame (~25 fps)
    if (now - lastFrameTime >= 40) {
        lastFrameTime   = now;
        renderer.angleY += g_rotSpeed;
        if (renderer.angleY >= 360.0f) renderer.angleY -= 360.0f;
        renderShipFrame();
    }

    // Clock + bar redraw every second
    if (now - lastClockDraw >= 1000) {
        lastClockDraw = now;
        drawRadarPanel(g_showInfo);
    }

    // Deferred WiFi reconnect
    if (g_wifiReconnectPending) {
        g_wifiReconnectPending = false;
        bool ok = wifiMgr.reconnect(settings.cfg.wifiSSID, settings.cfg.wifiPass);
        if (ok) { MDNS.begin("eliteclock"); MDNS.addService("http","tcp",80); ArduinoOTA.begin(); }
        drawRadarPanel(g_showInfo);
    }

    // HA MQTT
    haMqtt.loop();
    if (now - lastHaPublish >= 30000) {
        lastHaPublish = now;
        haMqtt.publishState();
    }
}
