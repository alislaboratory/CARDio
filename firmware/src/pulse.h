#pragma once
#include <stdint.h>

// Second-order IIR section (RBJ cookbook), transposed direct form II.
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1 = 0, z2 = 0;

  static Biquad lowpass(float fs, float f0, float q);
  static Biquad highpass(float fs, float f0, float q);

  float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  // Put the filter in the steady state it would reach for a constant input x0,
  // so a step in DC level doesn't ring through the output.
  void prime(float x0);
};

// Finds heartbeats in the raw IR photoplethysmogram (PPG).
//
//  raw IR -> band-pass 0.5-4 Hz -> invert -> peak picking with hysteresis
//
// Thresholds scale with the peak-to-peak size of the last 2 s of signal, so the
// same code works for strong and weak pulses and any LED current.
class BeatDetector {
 public:
  void begin(float sampleRateHz);

  // Settle the filters on the current level and forget beat history.
  void prime(float raw);

  // Feed one sample. Returns the inter-beat interval in ms when a beat is
  // confirmed, 0 for the first beat after priming, and -1 otherwise.
  float process(float raw, float dtMs);

  float signal() const { return s_; }      // filtered pulse wave (beats are peaks)
  float amplitude() const { return p2p_; }  // recent peak-to-peak
  float dc() const { return dc_; }

 private:
  static constexpr int kWindow = 200;  // 2 s at 100 Hz

  Biquad hp_, lp_;
  float s_ = 0, p2p_ = 0, dc_ = 0;
  float window_[kWindow] = {};
  int wpos_ = 0, wfill_ = 0;

  bool seekingPeak_ = false;
  float trough_ = 0, peak_ = 0;
  double t_ = 0, tPeak_ = 0, tLastBeat_ = 0;
  bool haveLastBeat_ = false;
};

// Turns inter-beat intervals into a stable heart rate. Keeps the last few
// intervals, rejects ones that don't fit the rhythm (missed or doubled beats,
// motion), and reports "locked" once enough consistent beats in a row arrive.
class BpmTracker {
 public:
  void reset();

  // Returns true if the interval was consistent with the current rhythm.
  bool add(float ibiMs);

  bool locked() const { return locked_; }
  int streak() const { return streak_; }
  float bpm() const { return bpm_; }  // smoothed, 0 if unknown
  float medianIbi() const;

 private:
  static constexpr int kHistory = 7;

  void push(float ibiMs);
  float recentSpread(int n) const;  // longest / shortest of the last n intervals

  float ibis_[kHistory] = {};
  int count_ = 0, head_ = 0;
  int streak_ = 0, rejects_ = 0;
  bool locked_ = false;
  float bpm_ = 0;
};
