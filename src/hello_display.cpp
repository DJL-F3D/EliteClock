// =============================================================================
//  hello_display.cpp — ILI9488 hello world, NO fillScreen/fillRect
//
//  Draws only using drawRect (outline), drawLine and text with NO background.
//  These use small pushBlock calls (one row at a time) rather than one huge burst.
//  If this shows output, pushBlock works for small transfers but stalls on large.
//  If still white/crashing, the issue is something more fundamental.
//
//  Learning applied:
//    - TFT_eSPI as pointer (no global constructor watchdog crash)
//    - esp_task_wdt_deinit() before construction
//    - ILI9488_DRIVER, 27MHz SPI
//    - Confirmed-working pins: MOSI=4, SCLK=2, CS=5, DC=1, RST=0, BL=8
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include "esp_task_wdt.h"

TFT_eSPI *tft = nullptr;

void setup() {
    // 1. Kill task watchdog before constructing TFT_eSPI
    esp_task_wdt_deinit();

    // 2. Construct display object safely
    tft = new TFT_eSPI();

    Serial.begin(115200);
    delay(300);
    Serial.printf("\n=== Hello Display (no fillScreen) ===\n");
    Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());

    // 3. Init and rotate
    Serial.println("init()...");
    tft->init();
    tft->setRotation(1);   // landscape 480x320
    Serial.println("init() done");

    // 4. Backlight on
    pinMode(8, OUTPUT);
    digitalWrite(8, HIGH);

    // ── Draw WITHOUT fillScreen ───────────────────────────────────────────
    // These are small transfers that should not trigger TG1WDT

    // White border rectangles
    tft->drawRect(0, 0, 480, 320, TFT_WHITE);
    tft->drawRect(4, 4, 472, 312, TFT_WHITE);
    Serial.println("borders drawn");

    // Coloured corner squares (small fillRect — 20x20 = 400 pixels = 1.2KB)
    tft->fillRect(8,   8,   20, 20, TFT_RED);
    tft->fillRect(452, 8,   20, 20, TFT_GREEN);
    tft->fillRect(8,   292, 20, 20, TFT_BLUE);
    tft->fillRect(452, 292, 20, 20, TFT_YELLOW);
    Serial.println("corner squares drawn");

    // Crosshair lines
    tft->drawFastHLine(0, 160, 480, TFT_CYAN);
    tft->drawFastVLine(240, 0, 320, TFT_CYAN);

    // Text — NO background colour (avoids pushBlock for background pixels)
    tft->setTextColor(TFT_WHITE);
    tft->setTextSize(3);
    tft->setCursor(95, 80);
    tft->print("ELITE CLOCK");

    tft->setTextColor(TFT_YELLOW);
    tft->setTextSize(2);
    tft->setCursor(130, 130);
    tft->print("ILI9488  480x320");

    tft->setTextColor(TFT_GREEN);
    tft->setTextSize(1);
    tft->setCursor(80, 175);
    tft->print("RED=top-left  GRN=top-right");
    tft->setCursor(80, 190);
    tft->print("BLU=bot-left  YEL=bot-right");

    tft->setTextColor(TFT_CYAN);
    tft->setCursor(80, 220);
    tft->print("If you can read this, pushBlock works for small transfers.");

    Serial.println("All drawing done — no crash!");
    Serial.println("Report: what do you see on screen?");
}

void loop() {
    // Flash the top-left corner to show the device is running (not hung)
    static bool lit = false;
    static uint32_t t = 0;
    if (millis() - t > 800) {
        t = millis();
        lit = !lit;
        tft->fillRect(8, 8, 20, 20, lit ? TFT_RED : TFT_BLACK);
        Serial.println(lit ? "blink ON" : "blink OFF");
    }
    yield();
}
