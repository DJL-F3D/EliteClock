// =============================================================================
//  hello_display.cpp — ABSOLUTE MINIMUM (no TFT, no libraries)
//  Just blinks the built-in LED and prints to serial.
//  If this works, the ESP32-C3 is healthy and we add TFT back next.
//  If this also crashes, the board or bootloader has a fundamental problem.
// =============================================================================

#include <Arduino.h>

// ESP32-C3 Super Mini has an LED on GPIO8
// (same pin as TFT backlight — unplug display wiring first to avoid conflict)
#define LED_PIN 8

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n=== BLINK TEST — no libraries ===");
    Serial.printf("Reset reason: %d  (1=poweron 3=panic 5=brownout 8=watchdog)\n",
                  (int)esp_reset_reason());
    Serial.println("If you can read this, ESP32-C3 is alive.");

    pinMode(LED_PIN, OUTPUT);
    Serial.println("Setup complete.");
}

void loop() {
    static uint32_t count = 0;
    digitalWrite(LED_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PIN, LOW);
    delay(500);
    Serial.printf("Blink %lu — heap free: %lu bytes\n",
                  count++, (unsigned long)ESP.getFreeHeap());
}
