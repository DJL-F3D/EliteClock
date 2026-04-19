// =============================================================================
//  hello_display.cpp — Minimal ST7796 display test
//  Cycles through inversion ON/OFF and all 4 rotations so you can see
//  which combination gives correct colours and orientation.
//  No WiFi, no touch, no settings — display only.
//
//  INSTRUCTIONS:
//  1. Temporarily rename your src/main.cpp to src/main.cpp.bak
//  2. Save this file as src/main.cpp
//  3. Compile and upload
//  4. Watch what appears on screen — report back what you see
//  5. Restore main.cpp.bak when done
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

TFT_eSPI tft;

// ── Helper: fill screen and print label ──────────────────────────────────────
void showTest(const char *label, uint16_t bg, uint16_t fg) {
    tft.fillScreen(bg);
    tft.setTextColor(fg, bg);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.print(label);

    // Draw coloured corner squares so we can check colour accuracy
    tft.fillRect(0,   0,   30, 30, TFT_RED);
    tft.fillRect(tft.width()-30, 0,   30, 30, TFT_GREEN);
    tft.fillRect(0,   tft.height()-30, 30, 30, TFT_BLUE);
    tft.fillRect(tft.width()-30, tft.height()-30, 30, 30, TFT_YELLOW);

    // Centre cross
    tft.drawLine(0, tft.height()/2, tft.width(), tft.height()/2, fg);
    tft.drawLine(tft.width()/2, 0, tft.width()/2, tft.height(), fg);

    // White border
    tft.drawRect(0, 0, tft.width(), tft.height(), TFT_WHITE);

    delay(3000);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n=== DISPLAY HELLO WORLD ===");

    tft.init();
    Serial.println("tft.init() done");

    // ── Test 1: No inversion, rotation 0 (portrait) ───────────────────────
    tft.setRotation(0);
    tft.invertDisplay(false);
    Serial.println("Test 1: invert=OFF rot=0");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.print("1: inv=OFF rot=0");
    tft.setTextSize(1);
    tft.setCursor(10, 40);
    tft.print("BLACK bg WHITE text");
    tft.setCursor(10, 55);
    tft.print("RED corner = top-left");
    tft.fillRect(0, 0, 30, 30, TFT_RED);
    tft.fillRect(tft.width()-30, tft.height()-30, 30, 30, TFT_BLUE);
    delay(4000);

    // ── Test 2: Inversion ON, rotation 0 ─────────────────────────────────
    tft.setRotation(0);
    tft.invertDisplay(true);
    Serial.println("Test 2: invert=ON rot=0");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.print("2: inv=ON rot=0");
    tft.setTextSize(1);
    tft.setCursor(10, 40);
    tft.print("BLACK bg WHITE text");
    tft.setCursor(10, 55);
    tft.print("RED corner = top-left");
    tft.fillRect(0, 0, 30, 30, TFT_RED);
    tft.fillRect(tft.width()-30, tft.height()-30, 30, 30, TFT_BLUE);
    delay(4000);

    // ── Test 3: Inversion OFF, rotation 1 (landscape) ────────────────────
    tft.setRotation(1);
    tft.invertDisplay(false);
    Serial.println("Test 3: invert=OFF rot=1 (landscape)");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.print("3: inv=OFF rot=1");
    tft.setTextSize(1);
    tft.setCursor(10, 40);
    tft.print("BLACK bg WHITE text");
    tft.setCursor(10, 55);
    tft.print("RED corner = top-left");
    tft.fillRect(0, 0, 30, 30, TFT_RED);
    tft.fillRect(tft.width()-30, tft.height()-30, 30, 30, TFT_BLUE);
    delay(4000);

    // ── Test 4: Inversion ON, rotation 1 (landscape) ─────────────────────
    tft.setRotation(1);
    tft.invertDisplay(true);
    Serial.println("Test 4: invert=ON rot=1 (landscape)");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.print("4: inv=ON rot=1");
    tft.setTextSize(1);
    tft.setCursor(10, 40);
    tft.print("BLACK bg WHITE text");
    tft.setCursor(10, 55);
    tft.print("RED corner = top-left");
    tft.fillRect(0, 0, 30, 30, TFT_RED);
    tft.fillRect(tft.width()-30, tft.height()-30, 30, 30, TFT_BLUE);
    delay(4000);

    // ── Test 5: Colour bars — checks colour accuracy ─────────────────────
    tft.setRotation(1);
    tft.invertDisplay(false);
    Serial.println("Test 5: colour bars");
    int bw = tft.width() / 8;
    const uint16_t bars[] = {
        TFT_WHITE, TFT_YELLOW, TFT_CYAN, TFT_GREEN,
        TFT_MAGENTA, TFT_RED, TFT_BLUE, TFT_BLACK
    };
    const char *barNames[] = {"WHT","YEL","CYN","GRN","MAG","RED","BLU","BLK"};
    for (int i = 0; i < 8; i++) {
        tft.fillRect(i * bw, 0, bw, tft.height(), bars[i]);
        tft.setTextColor(i < 4 ? TFT_BLACK : TFT_WHITE, bars[i]);
        tft.setTextSize(1);
        tft.setCursor(i * bw + 2, 10);
        tft.print(barNames[i]);
    }
    delay(5000);

    Serial.println("All tests complete — looping test 3 (inv=OFF rot=1)");
}

void loop() {
    // Stay on a simple black screen with white text so it's easy to see
    tft.setRotation(1);
    tft.invertDisplay(false);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(3);
    int cx = tft.width() / 2;
    int cy = tft.height() / 2;
    tft.setCursor(cx - 90, cy - 20);
    tft.print("ELITE CLOCK");
    tft.setTextSize(1);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(cx - 60, cy + 20);
    tft.print("Display OK - inv=OFF rot=1");
    tft.setCursor(cx - 50, cy + 35);
    tft.print("Restore main.cpp.bak");
    delay(2000);
}
