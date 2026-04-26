// =============================================================================
//  hello_display.cpp — ILI9488 display test, ESP32-C3 safe
//
//  KEY FIX: TFT_eSPI is a POINTER, not a global object.
//  The global constructor of TFT_eSPI initialises the SPI hardware and hangs
//  the watchdog timer before setup() is ever called.
//  By using a pointer we delay construction until AFTER the watchdog is off.
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include "esp_task_wdt.h"

// ── Pointer — NO global constructor runs at startup ───────────────────────────
TFT_eSPI *tft = nullptr;

// ── Test configurations ───────────────────────────────────────────────────────
struct TC { bool inv; uint8_t rot; };
static const TC TESTS[] = {
    {false, 0}, {true, 0},
    {false, 1}, {true, 1},   // rot=1 landscape is most likely
    {false, 3}, {true, 3},
};
static const int NT = sizeof(TESTS)/sizeof(TESTS[0]);
uint8_t  g_t    = 0;
uint32_t g_next = 0;

void drawTest(uint8_t n, bool inv, uint8_t rot) {
    tft->setRotation(rot);
    tft->invertDisplay(inv);
    tft->fillScreen(TFT_BLACK);
    // Coloured corners
    tft->fillRect(0, 0, 40, 40, TFT_RED);
    tft->fillRect(tft->width()-40, 0, 40, 40, TFT_GREEN);
    tft->fillRect(0, tft->height()-40, 40, 40, TFT_BLUE);
    tft->fillRect(tft->width()-40, tft->height()-40, 40, 40, TFT_YELLOW);
    // Cross
    tft->drawFastHLine(0, tft->height()/2, tft->width(), TFT_WHITE);
    tft->drawFastVLine(tft->width()/2, 0, tft->height(), TFT_WHITE);
    // Label
    tft->setTextColor(TFT_WHITE, TFT_BLACK);
    tft->setTextSize(2);
    tft->setCursor(50, 12);
    tft->printf("TEST %d  inv=%s  rot=%d", n, inv?"ON":"OFF", rot);
    tft->setTextSize(1);
    tft->setTextColor(TFT_YELLOW, TFT_BLACK);
    tft->setCursor(50, 45);
    tft->print("RED=top-left  GRN=top-right");
    tft->setCursor(50, 58);
    tft->print("BLU=bot-left  YEL=bot-right");
    tft->setCursor(50, 80);
    tft->setTextColor(TFT_CYAN, TFT_BLACK);
    tft->printf("%d x %d", tft->width(), tft->height());
    Serial.printf("Showing test %d: inv=%s rot=%d  %dx%d\n",
                  n, inv?"ON":"OFF", rot, tft->width(), tft->height());
}

void setup() {
    // 1. Disable watchdog FIRST — before constructing TFT_eSPI
    esp_task_wdt_deinit();

    // 2. Serial
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== HELLO DISPLAY (pointer mode) ===");
    Serial.printf("Reset reason: %d  (8=watchdog 5=brownout 3=panic 1=poweron)\n",
                  (int)esp_reset_reason());

    // 3. Construct TFT_eSPI NOW (after watchdog is off)
    Serial.println("Constructing TFT_eSPI...");
    tft = new TFT_eSPI();
    Serial.println("Calling tft->init()...");
    tft->init();
    Serial.println("Init done — drawing test 1");

    drawTest(1, TESTS[0].inv, TESTS[0].rot);
    g_next = millis() + 5000;
}

void loop() {
    yield();
    if (millis() >= g_next) {
        g_t = (g_t + 1) % NT;
        drawTest(g_t + 1, TESTS[g_t].inv, TESTS[g_t].rot);
        g_next = millis() + 5000;
    }
}
