// =============================================================================
//  include/SPIFFS.h — compatibility stub for ESP32-C3 + TFT_eSPI
//
//  TFT_eSPI v2.5.43 unconditionally includes "SPIFFS.h" in its ESP32-C3
//  processor file (TFT_eSPI_ESP32_C3.h:131), even though SPIFFS is not
//  shipped with Arduino-ESP32 2.x for the C3.
//
//  This project uses LittleFS and does not use TFT_eSPI's built-in touch
//  calibration storage (we drive XPT2046 via a separate library).
//  This stub satisfies the include so the build succeeds without pulling in
//  any actual SPIFFS implementation.
//
//  PlatformIO searches the project include/ directory before library paths,
//  so this file is found first and the missing-header error is suppressed.
// =============================================================================
#pragma once
