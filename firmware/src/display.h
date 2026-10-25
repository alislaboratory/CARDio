#pragma once
#include <stdint.h>

#include "color.h"
#include "leds.h"

// Everything the LEDs do. The app reports what's happening (mode changes,
// beats) and render() draws the current frame from the clock.
//
//  Startup     a spark climbs the spinal cord, the brain lights up, the heart
//              gives its first lub-dub (I2C is still off: the safe-upload window)
//  Idle        starlight: a dim shimmer with gentle random twinkles; back bullets glow
//  Acquiring   finger on: a violet scan sweeps upward, filling in as steady beats arrive
//  Live        every real heartbeat sends a lub-dub wave up the tree, coloured
//              by heart rate (blue slow, green resting, amber/red fast)
//  Readout     finger lifted: the final BPM is counted out digit by digit
//  Parked      I2C released for flashing (dim purple)
//  SensorError MAX30102 not answering (base blinks red)
class Display {
 public:
  enum class Mode : uint8_t { Startup, Idle, Acquiring, Live, Readout, Parked, SensorError };

  static constexpr uint32_t kStartupMs = 3000;

  void begin(uint32_t now);
  void setMode(Mode mode, uint32_t now);
  Mode mode() const { return mode_; }
  bool startupDone(uint32_t now) const { return now - modeSince_ >= kStartupMs; }
  bool readoutDone(uint32_t now) const { return now - modeSince_ >= readoutEndMs_; }

  void setProgress(float p) { progress_ = p; }  // acquiring: 0..1 towards a lock
  void beat(uint32_t now, float bpm, float ibiMs);  // trusted beat: full lub-dub wave
  void tentativeBeat(uint32_t now);                 // unconfirmed beat: small flash
  void startReadout(int bpm, uint32_t now);

  void render(uint32_t now);

 private:
  struct Pulse {
    uint32_t t0;
    float amp;
    RGB colour;
    float travelMs;  // time for the wave to climb from base to brain
    bool active;
  };
  struct Twinkle {
    uint32_t start;
    uint16_t durMs;
    float amp;  // 0 = not twinkling
  };
  struct Digit {
    uint32_t startMs;
    uint8_t value;
    RGB colour;
  };

  void addPulse(uint32_t t0, float amp, const RGB& colour, float travelMs);

  void renderStartup(uint32_t now);
  void renderIdle(uint32_t now, float level);
  void renderAcquiring(uint32_t now);
  void renderLive();
  void renderReadout(uint32_t now);
  void renderParked();
  void renderError(uint32_t now);
  void renderPulses(uint32_t now);

  Pixels px_;
  Mode mode_ = Mode::Startup;
  uint32_t modeSince_ = 0;
  uint32_t lastFrame_ = 0;
  float progress_ = 0;
  RGB liveColour_ = palette::kHeartRed;

  static constexpr int kMaxPulses = 8;
  Pulse pulses_[kMaxPulses] = {};
  int nextPulse_ = 0;

  Twinkle twinkles_[layout::kFront] = {};
  float shimmerRate_[layout::kFront] = {};
  float shimmerPhase_[layout::kFront] = {};

  Digit digits_[3] = {};
  int digitCount_ = 0;
  RGB readoutColour_;
  uint32_t readoutEndMs_ = 0;
};
