#include "pulse.h"

#include <math.h>
#include <string.h>

#include <algorithm>

#include "config.h"

// ---------------------------------------------------------------------------
// Biquad
// ---------------------------------------------------------------------------
namespace {

Biquad normalised(float b0, float b1, float b2, float a0, float a1, float a2) {
  Biquad f;
  f.b0 = b0 / a0;
  f.b1 = b1 / a0;
  f.b2 = b2 / a0;
  f.a1 = a1 / a0;
  f.a2 = a2 / a0;
  return f;
}

}  // namespace

Biquad Biquad::lowpass(float fs, float f0, float q) {
  const float w0 = 2.0f * float(M_PI) * f0 / fs;
  const float c = cosf(w0), alpha = sinf(w0) / (2.0f * q);
  return normalised((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

Biquad Biquad::highpass(float fs, float f0, float q) {
  const float w0 = 2.0f * float(M_PI) * f0 / fs;
  const float c = cosf(w0), alpha = sinf(w0) / (2.0f * q);
  return normalised((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

void Biquad::prime(float x0) {
  const float gain = (b0 + b1 + b2) / (1 + a1 + a2);  // DC gain
  const float y0 = gain * x0;
  z2 = b2 * x0 - a2 * y0;
  z1 = y0 - b0 * x0;
}

// ---------------------------------------------------------------------------
// BeatDetector
// ---------------------------------------------------------------------------
namespace {

constexpr float kHysteresis = 0.30f;   // of recent peak-to-peak
constexpr float kMinRise = 0.50f;      // a beat's upstroke must be at least this much of p2p
constexpr float kMinPerfusion = 4e-4f; // p2p / DC below this is noise, not a pulse
constexpr float kDcAlpha = 0.02f;

}  // namespace

void BeatDetector::begin(float sampleRateHz) {
  hp_ = Biquad::highpass(sampleRateHz, 0.5f, 0.7071f);
  lp_ = Biquad::lowpass(sampleRateHz, 4.0f, 0.7071f);
}

void BeatDetector::prime(float raw) {
  hp_.prime(raw);
  const float filtered = hp_.process(raw);  // ~0 once primed
  lp_.prime(filtered);
  dc_ = raw;
  s_ = p2p_ = 0;
  memset(window_, 0, sizeof(window_));
  wpos_ = wfill_ = 0;
  seekingPeak_ = false;
  trough_ = peak_ = 0;
  haveLastBeat_ = false;
}

float BeatDetector::process(float raw, float dtMs) {
  t_ += dtMs;
  dc_ += (raw - dc_) * kDcAlpha;

  // More blood in the thumb absorbs more IR, so the systolic pulse is a dip in
  // the raw signal. Flip it so heartbeats are peaks.
  s_ = -lp_.process(hp_.process(raw));

  window_[wpos_] = s_;
  wpos_ = (wpos_ + 1) % kWindow;
  if (wfill_ < kWindow) ++wfill_;
  float lo = s_, hi = s_;
  for (int i = 0; i < wfill_; ++i) {
    lo = std::min(lo, window_[i]);
    hi = std::max(hi, window_[i]);
  }
  p2p_ = hi - lo;

  const float hyst = kHysteresis * p2p_;
  float result = -1;

  if (!seekingPeak_) {
    // Falling towards the diastolic minimum.
    if (s_ < trough_) {
      trough_ = s_;
    } else if (s_ > trough_ + hyst) {
      seekingPeak_ = true;
      peak_ = s_;
      tPeak_ = t_;
    }
  } else {
    // Rising through systole: follow the peak until we're clearly past it.
    if (s_ > peak_) {
      peak_ = s_;
      tPeak_ = t_;
    } else if (s_ < peak_ - hyst) {
      const float rise = peak_ - trough_;
      const bool pulse = p2p_ > kMinPerfusion * dc_ && rise > kMinRise * p2p_;
      const double sinceLast = tPeak_ - tLastBeat_;
      const double refractoryMs = 60000.0 / cfg::kMaxBpm;
      if (pulse && (!haveLastBeat_ || sinceLast >= refractoryMs)) {
        result = haveLastBeat_ ? float(sinceLast) : 0.0f;
        tLastBeat_ = tPeak_;
        haveLastBeat_ = true;
      }
      seekingPeak_ = false;
      trough_ = s_;
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// BpmTracker
// ---------------------------------------------------------------------------
namespace {

// Healthy hearts speed up and slow down with breathing (sinus arrhythmia), so
// the tolerance is generous.
constexpr float kTolerance = 0.30f;
constexpr int kRejectsBeforeRestart = 3;
constexpr float kSmoothing = 0.35f;
// To lock, the last few intervals must agree this closely (longest / shortest),
// so a run of noise that happens to pass the looser tolerance can't lock.
constexpr float kMaxLockSpread = 1.35f;

}  // namespace

float BpmTracker::recentSpread(int n) const {
  if (count_ < n) return INFINITY;
  float lo = INFINITY, hi = 0;
  for (int i = 1; i <= n; ++i) {
    const float v = ibis_[(head_ - i + kHistory) % kHistory];
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  return hi / lo;
}

void BpmTracker::reset() {
  count_ = head_ = 0;
  streak_ = rejects_ = 0;
  locked_ = false;
  bpm_ = 0;
}

void BpmTracker::push(float ibiMs) {
  ibis_[head_] = ibiMs;
  head_ = (head_ + 1) % kHistory;
  if (count_ < kHistory) ++count_;
}

float BpmTracker::medianIbi() const {
  if (count_ == 0) return 0;
  float sorted[kHistory];
  memcpy(sorted, ibis_, sizeof(float) * count_);
  std::sort(sorted, sorted + count_);
  return count_ % 2 ? sorted[count_ / 2] : 0.5f * (sorted[count_ / 2 - 1] + sorted[count_ / 2]);
}

bool BpmTracker::add(float ibiMs) {
  const float minIbi = 60000.0f / cfg::kMaxBpm;
  const float maxIbi = 60000.0f / cfg::kMinBpm;
  const bool plausible = ibiMs >= minIbi && ibiMs <= maxIbi;

  bool consistent = plausible;
  if (plausible && count_ >= 3) {
    const float m = medianIbi();
    consistent = fabsf(ibiMs - m) <= kTolerance * m;
  }

  if (!consistent) {
    streak_ = 0;
    if (++rejects_ >= kRejectsBeforeRestart) {
      // The rhythm really changed (or we'd latched onto noise): start over.
      reset();
      if (plausible) push(ibiMs);
    }
    return false;
  }

  rejects_ = 0;
  push(ibiMs);
  ++streak_;
  if (streak_ >= cfg::kLockBeats && recentSpread(cfg::kLockBeats) <= kMaxLockSpread) locked_ = true;

  const float target = 60000.0f / medianIbi();
  bpm_ = bpm_ <= 0 ? target : bpm_ + kSmoothing * (target - bpm_);
  return true;
}
