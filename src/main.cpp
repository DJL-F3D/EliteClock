// =============================================================================
//  main.cpp — Elite Clock v1.4  (LovyanGFX edition)
//  ILI9488 480×320 landscape, BBC Micro Elite colour scheme
//
//  LovyanGFX replaces TFT_eSPI.  Key improvements:
//    - Proper ILI9488 18-bit colour handling (no pushBlock WDT crash)
//    - Built-in XPT2046 touch on shared SPI bus (no separate library needed)
//    - PWM backlight control via setBrightness()
//    - No safeFillRect workarounds needed
//    - No global constructor watchdog issues
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>           // must come before ESPAsyncWebServer headers
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <math.h>
#include "esp_task_wdt.h"
#include "display_config.h" // LGFX class (ILI9488 + XPT2046 + backlight)
#include "ships.h"
#include "renderer.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "web_ui.h"
#include "ha_mqtt.h"

// ── Display layout (landscape 480×320) ───────────────────────────────────────
#define SCR_W    480
#define SCR_H    320
#define HDR_H     22
#define LEFT_W   115   // reduced from 150 → larger ship viewport
#define RIGHT_W  115   // reduced from 150 → larger ship viewport
#define RADAR_H   80
#define RADAR_Y  (SCR_H - RADAR_H)   // 240

#define SHIP_SW  (SCR_W - LEFT_W - RIGHT_W)  // 250 (was 180)
#define SHIP_SH  (SCR_H - HDR_H - RADAR_H)   // 218
#define SHIP_SX  LEFT_W                        // 115
#define SHIP_SY  HDR_H                         // 22

// ── Touch zones ───────────────────────────────────────────────────────────────
#define PREV_X2  (LEFT_W - 10)               // 105
#define NEXT_X1  (SCR_W - RIGHT_W + 10)      // 375
#define NAV_Y2    40
#define SHAD_X1  (LEFT_W + 20)               // 135 — centre of ship viewport
#define SHAD_X2  (SCR_W - RIGHT_W - 20)      // 345
#define SHAD_Y1  (HDR_H + 50)
#define SHAD_Y2  (HDR_H + 168)

// ── BBC Elite colour palette (RGB565) ─────────────────────────────────────────
#define COL_BG      TFT_BLACK
#define COL_WHITE   TFT_WHITE
#define COL_YELLOW  0xFFE0u
#define COL_CYAN    0x07FFu
#define COL_GREEN   0x07E0u
#define COL_DKGREEN 0x0320u
#define COL_ORANGE  0xFD20u
#define COL_BORDER  0x07E0u   // bright green (was dark 0x0260)

// Bar colors matching BBC Elite screenshot: red bottom → cyan top
static const uint16_t BAR_COLS[6] = {
    0xF800u,   // i=0 bottom: red
    0xFD20u,   // i=1: orange  
    0xFFE0u,   // i=2: yellow
    0x87E0u,   // i=3: yellow-green
    0x07E0u,   // i=4: green
    0x07FFu,   // i=5 top: cyan
};

// ── Global state ──────────────────────────────────────────────────────────────
uint8_t  g_currentShip         = 0;
bool     g_shaded               = false;
bool     g_backlightOn          = true;
float    g_rotSpeed             = 0.6f;
uint16_t g_shipDuration         = 30;
bool     g_shipChanged          = false;
int      g_shipChangeDir        = 1;
bool     g_showInfo             = false;
bool     g_wifiReconnectPending = false;
bool     g_spriteOK             = false;
bool     g_radarNeedsReset      = true;   // full radar redraw on next call

// ── Hardware objects ──────────────────────────────────────────────────────────
// Pointer to avoid any global-constructor timing issues during boot
LGFX             *tft        = nullptr;
lgfx::LGFX_Sprite *shipSprite = nullptr;
Renderer          renderer;

// ── Timing ────────────────────────────────────────────────────────────────────
uint32_t lastShipSwitch = 0;
uint32_t lastFrameTime  = 0;
uint32_t lastClockDraw  = 0;
uint32_t lastBarDraw    = 0;   // 100ms — bar animation + radar
uint32_t lastHaPublish  = 0;
uint32_t lastTouchTime  = 0;
bool     touchWasDown   = false;

// ── Star-field ────────────────────────────────────────────────────────────────
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

// ── Accessors used by web_ui.h / ha_mqtt.h ────────────────────────────────────
const char* getShipName(int idx) {
    if (idx < 0 || idx >= NUM_SHIPS) return "Unknown";
    return SHIPS[idx].name;
}
int getNumShips() { return NUM_SHIPS; }

// ── Backlight control (also called from ha_mqtt.h) ────────────────────────────
void setBacklight(bool on) {
    g_backlightOn = on;
    if (tft) tft->setBrightness(on ? 255 : 0);
    if (haMqtt.connected) haMqtt.publishState();
}

// ── Laser label ───────────────────────────────────────────────────────────────
static const char* laserStr(uint8_t l) {
    if (l == 0) return "None";
    if (l == 1) return "Pulse";
    if (l == 2) return "Beam";
    if (l <= 4) return "Military";
    return "Heavy Mil";
}

// =============================================================================
//  drawAnimBars() — BBC Elite VU-meter style bars
//  Each bar independently fluctuates in fill width using different sine
//  wave periods — organic, non-synchronised movement like the original game.
// =============================================================================
void drawAnimBars(int16_t x, int16_t y, int16_t w, uint32_t now) {
    static const uint32_t periods[6] = {5700, 4200, 6900, 4800, 6300, 3600};  // 3× slower
    static const uint32_t offsets[6] = {   0,  500,  250,  750,  125,  625};
    for (int i = 0; i < 6; i++) {
        int16_t by    = y + (5 - i) * 9;
        uint32_t t    = (now + offsets[i]) % periods[i];
        float    phase = (float)t / (float)periods[i];
        float    fill  = 0.40f + 0.60f * (0.5f + 0.5f * sinf(phase * 6.28318f));
        int16_t fillW  = (int16_t)((float)w * fill);
        int16_t gapW   = w - fillW;
        tft->fillRect(x,          by, fillW, 10, BAR_COLS[i]);
        if (gapW > 0)
            tft->fillRect(x + fillW, by, gapW, 10, COL_BG);
    }
}

// =============================================================================
//  drawHeader()
// =============================================================================
void drawHeader() {
    tft->fillRect(0, 0, SCR_W, HDR_H, COL_BG);
    tft->drawFastHLine(0, HDR_H - 2, SCR_W, COL_BORDER);
    tft->drawFastHLine(0, HDR_H - 1, SCR_W, COL_BORDER);

    tft->setTextSize(2);

    // Left: commander name (from settings, editable via web UI / HA)
    tft->setTextColor(COL_YELLOW, COL_BG);
    tft->setCursor(3, 3);
    tft->print(settings.cfg.cmdName);

    // Centre: ship name
    const char *name = SHIPS[g_currentShip].name;
    int tw = strlen(name) * 12;
    tft->setTextColor(COL_WHITE, COL_BG);
    tft->setCursor((SCR_W - tw) / 2, 3);
    tft->print(name);

    // Right: clock (HH:MM:SS, updated every second from loop)
    struct tm ti;
    char timebuf[12] = "--:--:--";
    if (getLocalTime(&ti, 0)) strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &ti);
    else snprintf(timebuf, sizeof(timebuf), "%02lu:%02lu:%02lu",
                  (millis()/3600000UL)%24, (millis()/60000UL)%60, (millis()/1000UL)%60);
    tft->setTextColor(COL_GREEN, COL_BG);
    tft->setCursor(SCR_W - 98, 3);    // 8 chars × 12px = 96px
    tft->print(timebuf);

    // Shaded badge (small, below clock)
    if (g_shaded) {
        tft->setTextSize(1);
        tft->setTextColor(COL_ORANGE, COL_BG);
        tft->setCursor(SCR_W - 40, 8);
        tft->print("[SHD]");
    }
}

// =============================================================================
//  drawShipInfo()
// =============================================================================
void drawShipInfo() {
    const ShipDef   &ship = SHIPS[g_currentShip];
    const ShipStats &st   = ship.stats;
    const ShipLore  *lr   = ship.lore;

    tft->fillRect(0,               HDR_H, LEFT_W,  SHIP_SH, COL_BG);
    tft->fillRect(SCR_W - RIGHT_W, HDR_H, RIGHT_W, SHIP_SH, COL_BG);
    tft->drawFastVLine(LEFT_W - 2,          HDR_H, SHIP_SH, COL_BORDER);
    tft->drawFastVLine(LEFT_W - 1,          HDR_H, SHIP_SH, COL_BORDER);
    tft->drawFastVLine(SCR_W - RIGHT_W,     HDR_H, SHIP_SH, COL_BORDER);
    tft->drawFastVLine(SCR_W - RIGHT_W + 1, HDR_H, SHIP_SH, COL_BORDER);

    tft->setTextSize(1);
    int lx = 3, ly = HDR_H + 5;

    auto lbl = [&](const char *s) {
        tft->setTextColor(COL_CYAN, COL_BG);
        tft->setCursor(lx, ly); tft->print(s); ly += 10;
    };
    auto val = [&](const char *s) {
        tft->setTextColor(TFT_WHITE, COL_BG);   // white values match BBC screenshot
        tft->setCursor(lx + 4, ly); tft->print(s); ly += 10;
    };

    lbl("INSERVICE:");
    if (lr) {
        const char *p = strchr(lr->inservice, '(');
        if (p && p != lr->inservice) {
            char yr[16] = {};
            strncpy(yr, lr->inservice, (int)(p - lr->inservice) - 1);
            val(yr); val(p);
        } else { val(lr->inservice); }
    }
    ly += 3;
    lbl("DIMENSIONS:");
    if (lr) { char tmp[24]; snprintf(tmp, sizeof(tmp), "%sft", lr->dimensions); val(tmp); }
    ly += 3;
    lbl("SPEED:");
    { char tmp[16]; snprintf(tmp, sizeof(tmp), "%.2fLM", st.speed * 0.0125f); val(tmp); }
    ly += 3;
    lbl("HULL:");
    { char tmp[16]; snprintf(tmp, sizeof(tmp), "%d units", st.health); val(tmp); }
    ly += 3;
    lbl("DRIVE MOTORS:");
    if (lr) {
        const char *d = lr->drive;
        if (strlen(d) > 18) {
            const char *sp = nullptr;
            for (const char *c = d; c < d+18 && *c; c++) if (*c == ' ') sp = c;
            if (sp) {
                char line1[20] = {};
                strncpy(line1, d, (int)(sp - d));
                val(line1); val(sp + 1);
            } else { val(d); }
        } else { val(d); }
    }

    // Right panel
    int rx = SCR_W - RIGHT_W + 3, ry = HDR_H + 5;
    auto rlbl = [&](const char *s) {
        tft->setTextColor(COL_CYAN, COL_BG);
        tft->setCursor(rx, ry); tft->print(s); ry += 10;
    };
    auto rval = [&](const char *s) {
        tft->setTextColor(TFT_WHITE, COL_BG);   // white values
        tft->setCursor(rx + 4, ry); tft->print(s); ry += 10;
    };
    auto rvalFmt = [&](const char *fmt, ...) {
        char buf[32]; va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        rval(buf);
    };

    rlbl("COMBAT FACTOR:");
    rvalFmt("CF%d", st.lasers * 2 + st.missiles);
    ry += 3;
    rlbl("CREW:");
    if (lr) rval(lr->crew); else rvalFmt("1-%d", 1 + st.missiles/2);
    ry += 3;
    rlbl("ARMAMENTS:");
    if (st.lasers > 0)   rvalFmt("%dx %s Laser", st.lasers, laserStr(st.lasers));
    else                 rval("None");
    if (st.missiles > 0) rvalFmt("%d Missile%s", st.missiles, st.missiles>1?"s":"");
    ry += 3;
    rlbl("BOUNTY:");
    if (st.bounty > 0) rvalFmt("%u.%uCr", st.bounty/10, st.bounty%10);
    else               rval("None");
    ry += 3;
    rlbl("CLASS:");
    rval(st.role);
}

// =============================================================================
//  drawRadarPanel()
// =============================================================================
//  drawRadarPanel() — flash-free; clock moved to header; red radar ellipse
// =============================================================================
void drawRadarPanel(bool showInfo) {
    const int y   = RADAR_Y;
    uint32_t  now = millis();
    const int rx  = SCR_W / 2, ry2 = y + 44, ra = 78, rb = 24;

    static int  s_prevBX = -1, s_prevBY = -1;

    if (g_radarNeedsReset) {
        g_radarNeedsReset = false;
        s_prevBX = s_prevBY = -1;

        tft->fillRect(0, y, SCR_W, RADAR_H, COL_BG);
        tft->drawFastHLine(0, y,   SCR_W, COL_BORDER);
        tft->drawFastHLine(0, y+1, SCR_W, COL_BORDER);

        // Radar ellipse — RED as per BBC screenshot
        tft->drawEllipse(rx, ry2, ra,     rb,     TFT_RED);
        tft->drawEllipse(rx, ry2, ra - 1, rb - 1, 0xC000u);  // dark red inner
        tft->drawEllipse(rx, ry2, ra / 2, rb / 2, 0xC000u);
        tft->drawFastHLine(rx - ra, ry2, 2*ra, 0xC000u);
        tft->drawFastVLine(rx, ry2 - rb, 2*rb, 0xC000u);
        tft->fillCircle(rx, ry2, 3, TFT_RED);

        // ELITE logo
        tft->setTextColor(COL_YELLOW, COL_BG);
        tft->setTextSize(2);
        tft->setCursor(rx - 30, y + 62);
        tft->print("ELITE");

        // Touch hint
        tft->setTextSize(1);
        tft->setTextColor(COL_DKGREEN, COL_BG);
        tft->setCursor(SCR_W - 100, y + 4);
        tft->print(showInfo ? "[TAP:INFO]" : "[TAP:INFO]");

        // IP / AP badge — top of radar strip, right of bars
        if (wifiMgr.isAP()) {
            tft->setTextColor(COL_ORANGE, COL_BG);
            tft->setCursor(68, y + 4);
            tft->printf("AP %s", wifiMgr.localIP.c_str());
        } else if (wifiMgr.isConnected()) {
            tft->setTextColor(COL_DKGREEN, COL_BG);
            tft->setCursor(68, y + 4);
            tft->printf("%s", wifiMgr.localIP.c_str());
        }

        drawAnimBars(4,          y + 14, 55, now);
        drawAnimBars(SCR_W - 60, y + 14, 55, now + 300);
    } else {
        // Incremental: erase old blip, restore ellipse
        if (s_prevBX >= 0) {
            tft->fillCircle(s_prevBX, s_prevBY, 5, COL_BG);
            tft->drawEllipse(rx, ry2, ra,     rb,     TFT_RED);
            tft->drawEllipse(rx, ry2, ra - 1, rb - 1, 0xC000u);
            tft->drawEllipse(rx, ry2, ra / 2, rb / 2, 0xC000u);
            tft->drawFastHLine(rx - ra, ry2, 2*ra, 0xC000u);
            tft->drawFastVLine(rx, ry2 - rb, 2*rb, 0xC000u);
            tft->fillCircle(rx, ry2, 3, TFT_RED);
        }
        drawAnimBars(4,          y + 14, 55, now);
        drawAnimBars(SCR_W - 60, y + 14, 55, now + 300);
    }

    // Seconds blip
    int  curSec = (millis() / 1000) % 60;
    struct tm ti;
    if (getLocalTime(&ti, 0)) curSec = ti.tm_sec;

    float ang = (curSec * 6.0f - 90.0f) * DEG_TO_RAD;
    int bx = rx + (int)(cosf(ang) * ra * 0.65f);
    int by2 = ry2 + (int)(sinf(ang) * rb * 0.65f);
    tft->fillCircle(bx, by2, 4, COL_YELLOW);
    tft->drawCircle(bx, by2, 4, COL_WHITE);
    s_prevBX = bx; s_prevBY = by2;
}

// =============================================================================
//  renderShipFrame()
// =============================================================================
void renderShipFrame() {
    if (!g_spriteOK) {
        tft->fillRect(SHIP_SX, SHIP_SY, SHIP_SW, SHIP_SH, COL_BG);
        return;
    }
    shipSprite->fillSprite(COL_BG);
    for (int i = 0; i < 48; i++)
        shipSprite->drawPixel(stars[i].x, stars[i].y, stars[i].col);
    renderer.angleX += renderer.driftX;
    if (renderer.angleX >= 360.0f) renderer.angleX -= 360.0f;
    renderer.shaded = g_shaded;
    renderer.render(*shipSprite, SHIPS[g_currentShip]);
    shipSprite->pushSprite(SHIP_SX, SHIP_SY);
}

// =============================================================================
//  handleTouch() — uses LovyanGFX built-in touch, no separate library needed
// =============================================================================
void handleTouch() {
    lgfx::touch_point_t tp;
    if (!tft->getTouch(&tp)) {
        touchWasDown = false;
        return;
    }
    if (touchWasDown)                   return;
    if (millis() - lastTouchTime < 300) return;

    touchWasDown  = true;
    lastTouchTime = millis();

    // LovyanGFX already maps raw ADC → screen coords using calibration values
    int tx = constrain((int)tp.x, 0, SCR_W - 1);
    int ty = constrain((int)tp.y, 0, SCR_H - 1);

    Serial.printf("[Touch] screen(%d,%d)\n", tx, ty);

    // PREV
    if (tx <= PREV_X2 && ty <= NAV_Y2) {
        tft->drawRect(0, 0, PREV_X2, NAV_Y2, COL_GREEN); delay(35);
        tft->drawRect(0, 0, PREV_X2, NAV_Y2, COL_BG);
        g_currentShip = (g_currentShip + NUM_SHIPS - 1) % NUM_SHIPS;
        settings.cfg.currentShip = g_currentShip;
        settings.save(); seedStars(g_currentShip);
        g_shipChanged = true; lastShipSwitch = millis();
        return;
    }
    // NEXT
    if (tx >= NEXT_X1 && ty <= NAV_Y2) {
        tft->drawRect(NEXT_X1, 0, SCR_W-NEXT_X1, NAV_Y2, COL_GREEN); delay(35);
        tft->drawRect(NEXT_X1, 0, SCR_W-NEXT_X1, NAV_Y2, COL_BG);
        g_currentShip = (g_currentShip + 1) % NUM_SHIPS;
        settings.cfg.currentShip = g_currentShip;
        settings.save(); seedStars(g_currentShip);
        g_shipChanged = true; lastShipSwitch = millis();
        return;
    }
    // SHADED
    if (tx >= SHAD_X1 && tx <= SHAD_X2 && ty >= SHAD_Y1 && ty <= SHAD_Y2) {
        g_shaded = !g_shaded;
        settings.cfg.shaded = g_shaded;
        settings.save(); drawHeader();
        return;
    }
    // RADAR
    if (ty >= RADAR_Y) {
        g_showInfo = !g_showInfo;
        g_radarNeedsReset = true;
        drawRadarPanel(g_showInfo);
        return;
    }
}

// =============================================================================
//  SETUP
// =============================================================================
void setup() {
    // Disable task watchdog before constructing LGFX
    esp_task_wdt_deinit();

    // Construct display objects
    tft        = new LGFX();
    shipSprite = new lgfx::LGFX_Sprite(tft);

    Serial.begin(115200);
    delay(200);
    Serial.printf("\n[Elite Clock v1.4 LovyanGFX] Starting...\n");
    Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());

    // 1. Settings
    settings.begin();
    g_currentShip = settings.cfg.currentShip;
    if (g_currentShip >= NUM_SHIPS) { g_currentShip = 0; settings.cfg.currentShip = 0; settings.save(); }
    g_shaded       = settings.cfg.shaded;
    g_rotSpeed     = settings.cfg.rotSpeed;
    g_shipDuration = settings.cfg.shipDuration;

    // 2. WiFi (before display so AP appears even if display hangs)
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
            tft->fillScreen(COL_BG);
            tft->setTextColor(COL_YELLOW, COL_BG); tft->setTextSize(2);
            tft->setCursor(120, 130); tft->println("OTA UPDATE");
        });
        ArduinoOTA.onProgress([](unsigned int prog, unsigned int total) {
            int pct = prog * 100 / total;
            tft->fillRect(80, 185, 320, 16, COL_BG);
            tft->fillRect(80, 185, pct * 320 / 100, 14, COL_DKGREEN);
        });
        ArduinoOTA.onEnd([]() {
            tft->setCursor(150, 210); tft->setTextColor(COL_GREEN, COL_BG);
            tft->println("Complete - rebooting");
        });
        ArduinoOTA.onError([](ota_error_t e) { (void)e; });
        ArduinoOTA.begin();
    }

    // 6. MQTT
    haMqtt.begin();

    // 7. Display init (LovyanGFX handles SPI setup internally)
    Serial.println("Calling tft->init()...");
    tft->init();
    tft->setRotation(0);    // offset_rotation=1 in config bakes landscape into init()
    tft->setBrightness(255);
    g_backlightOn = true;
    Serial.println("Display init done.");

    // Boot splash
    tft->fillScreen(COL_BG);
    tft->setTextColor(COL_YELLOW, COL_BG); tft->setTextSize(2);
    tft->setCursor(100, 80); tft->print("** ELITE CLOCK **");
    tft->setTextSize(1); tft->setTextColor(COL_GREEN, COL_BG);
    tft->setCursor(155, 115); tft->print("LovyanGFX Edition v1.4");
    tft->setCursor(165, 130); tft->print("ILI9488 480x320");

    tft->setCursor(140, 155);
    if (wifiMgr.isConnected()) {
        tft->setTextColor(COL_GREEN, COL_BG);
        tft->printf("WIFI: %s", wifiMgr.localIP.c_str());
    } else if (wifiMgr.isAP()) {
        tft->setTextColor(COL_ORANGE, COL_BG);
        tft->printf("AP: %s  %s", AP_SSID, wifiMgr.localIP.c_str());
    }

    // 8. Sprite
    g_spriteOK = (bool)shipSprite->createSprite(SHIP_SW, SHIP_SH);
    if (g_spriteOK) {
        shipSprite->setColorDepth(16);
        Serial.printf("[Sprite] %dx%d OK  heap=%lu\n", SHIP_SW, SHIP_SH, (unsigned long)ESP.getFreeHeap());
    } else {
        Serial.println("[Sprite] FAILED — low memory");
        tft->setTextColor(COL_ORANGE, COL_BG);
        tft->setCursor(140, 175); tft->print("LOW MEM — no sprite");
    }

    // 9. Stars + initial random tumble orientation
    seedStars(g_currentShip);
    srand(esp_random());
    renderer.angleY = (float)(rand() % 360);
    renderer.angleX = (float)(rand() % 360);
    renderer.angleZ = (float)(rand() % 360);
    renderer.driftX = ((float)((int)(rand() % 60) - 30)) / 100.0f;
    renderer.driftZ = ((float)((int)(rand() % 40) - 20)) / 100.0f;

    delay(1500);

    // 10. Main UI
    tft->fillScreen(COL_BG);
    drawHeader();
    drawShipInfo();
    drawRadarPanel(false);

    lastShipSwitch = millis();
    lastFrameTime  = millis();
    lastClockDraw  = millis();
    lastBarDraw    = millis();

    Serial.printf("[Elite Clock] Ready — ship: %s  IP: %s\n",
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

    if (now - lastShipSwitch >= (uint32_t)g_shipDuration * 1000UL) {
        g_currentShip = (g_currentShip + 1) % NUM_SHIPS;
        settings.cfg.currentShip = g_currentShip;
        settings.save(); seedStars(g_currentShip);
        g_shipChanged = true; lastShipSwitch = now;
    }

    if (g_shipChanged) {
        g_shipChanged = false;
        g_radarNeedsReset = true;
        renderer.angleY = (float)(rand() % 360);
        renderer.angleX = (float)(rand() % 360);
        renderer.angleZ = (float)(rand() % 360);
        // Random gentle drift on X and Z for organic tumble
        renderer.driftX = ((float)((int)(rand() % 60) - 30)) / 100.0f;  // -0.3..+0.3°/f
        renderer.driftZ = ((float)((int)(rand() % 40) - 20)) / 100.0f;  // -0.2..+0.2°/f
        seedStars(g_currentShip);
        drawHeader(); drawShipInfo(); drawRadarPanel(g_showInfo);
    }

    if (now - lastFrameTime >= 40) {
        lastFrameTime    = now;
        renderer.angleY += g_rotSpeed;
        if (renderer.angleY >= 360.0f) renderer.angleY -= 360.0f;
        renderer.angleZ += renderer.driftZ;
        if (renderer.angleZ >= 360.0f) renderer.angleZ -= 360.0f;
        renderShipFrame();
    }

    // Full radar panel every second
    if (now - lastClockDraw >= 1000) {
        lastClockDraw = now;
        lastBarDraw   = now;
        drawRadarPanel(g_showInfo);
        drawHeader();
    }
    // Bar animation only every 100ms (between full redraws)
    else if (now - lastBarDraw >= 100) {
        lastBarDraw = now;
        drawAnimBars(4,          RADAR_Y + 14, 55, now);
        drawAnimBars(SCR_W - 60, RADAR_Y + 14, 55, now + 300);
    }

    if (g_wifiReconnectPending) {
        g_wifiReconnectPending = false;
        bool ok = wifiMgr.reconnect(settings.cfg.wifiSSID, settings.cfg.wifiPass);
        if (ok) { MDNS.begin("eliteclock"); MDNS.addService("http","tcp",80); ArduinoOTA.begin(); }
        drawRadarPanel(g_showInfo);
    }

    haMqtt.loop();
    if (now - lastHaPublish >= 30000) {
        lastHaPublish = now;
        haMqtt.publishState();
    }
}
