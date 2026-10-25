#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// CARDio Rev 3.0 hardware (Rev3.0/SCH_ECG-business-card_2025-07-31.json)
// ---------------------------------------------------------------------------
namespace pins {
constexpr int kLedData = 3;  // WS2812B-2020 chain DIN (LED9 first)
constexpr int kI2cSda = 9;   // MAX30102 SDA. Also the ESP32-C3 BOOT strap: low at reset = bootloader.
constexpr int kI2cScl = 8;   // MAX30102 SCL. Also a strap pin.
}  // namespace pins

namespace cfg {

// --- Timing -----------------------------------------------------------------
// I2C stays off for this long after every reset (it is the startup animation).
// If a firmware ever misbehaves, this is the window in which a plain upload
// is guaranteed to work: plug in, then upload within ~3 s.
constexpr uint32_t kBootSafeWindowMs = 3000;
constexpr uint32_t kFrameMs = 10;       // LED refresh (100 fps)
constexpr uint32_t kSensorPollMs = 40;  // FIFO burst-read interval (FIFO holds 320 ms)

// --- LEDs -------------------------------------------------------------------
constexpr uint8_t kBrightness = 110;  // output for a full-scale (1.0) channel, 0-255
constexpr float kGamma = 2.0f;

// --- MAX30102 ---------------------------------------------------------------
// LED currents are in 0.2 mA steps. Red isn't used for the heart rate; it just
// gives the "Place Thumb" pad a faint red glow.
constexpr uint8_t kRedCurrent = 0x10;        // 3.2 mA
constexpr uint8_t kIrCurrentDefault = 0x24;  // 7.2 mA
constexpr uint8_t kIrCurrentMin = 0x06;
constexpr uint8_t kIrCurrentMax = 0x90;

// Finger detection on the IR level (18-bit counts, full scale 262143).
constexpr float kFingerOnCounts = 40000.0f;
constexpr float kFingerOffCounts = 20000.0f;
constexpr int kFingerOnSamples = 15;   // 150 ms debounce
constexpr int kFingerOffSamples = 25;  // 250 ms debounce

// Auto gain: keep the IR level in this band so pulses are big but never clip.
constexpr float kAgcLow = 0.25f;   // fraction of full scale
constexpr float kAgcHigh = 0.80f;
constexpr uint32_t kAgcIntervalMs = 400;

// After the finger lands the filters are held for at least kSettleSamples, and
// then until the IR level has been still (within kStillFraction over the last
// kStillWindow samples) -- a thumb still sinking onto the pad otherwise looks
// like a huge pulse and swamps the adaptive thresholds. Gives up after
// kMaxSettleSamples and starts anyway.
constexpr int kSettleSamples = 60;
constexpr int kStillWindow = 40;
constexpr float kStillFraction = 0.004f;
constexpr int kMaxSettleSamples = 300;

// --- Heart rate -------------------------------------------------------------
constexpr float kMinBpm = 35.0f;
constexpr float kMaxBpm = 210.0f;
constexpr int kLockBeats = 5;  // consistent beats before the reading is trusted

}  // namespace cfg
