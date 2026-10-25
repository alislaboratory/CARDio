#include "display.h"

#include <Arduino.h>
#include <math.h>

#include <algorithm>

namespace {

constexpr float kTwoPi = 2.0f * float(M_PI);

// Light that arrives at tau = 0: quick attack, exponential tail.
float flash(float tauMs, float attackMs, float decayMs) {
  if (tauMs < 0) return 0;
  if (tauMs < attackMs) return tauMs / attackMs;
  return expf(-(tauMs - attackMs) / decayMs);
}

float clampf(float x, float lo, float hi) { return std::min(hi, std::max(lo, x)); }

float msSince(uint32_t now, uint32_t t0) { return float(int32_t(now - t0)); }

// Idle starlight: chance per LED per frame (100 fps) that a twinkle starts.
// 3/1000 * 11 LEDs * 100 fps = about 3 gentle twinkles a second across the tree.
constexpr int kTwinkleChancePerMille = 3;

// Readout timing
constexpr uint32_t kIntroMs = 700;
constexpr uint32_t kCountStepMs = 220;
constexpr uint32_t kHoldMs = 800;
constexpr uint32_t kFadeMs = 300;
constexpr uint32_t kZeroMs = 500;
constexpr uint32_t kDigitGapMs = 350;

uint32_t digitDuration(uint8_t v) { return v == 0 ? kZeroMs : v * kCountStepMs + kHoldMs + kFadeMs; }

}  // namespace

void Display::begin(uint32_t now) {
  px_.begin();
  for (int i = 0; i < layout::kFront; ++i) {
    shimmerRate_[i] = random(12, 35) / 100.0f;  // 0.12-0.35 Hz, each LED its own
    shimmerPhase_[i] = random(628) / 100.0f;
  }
  mode_ = Mode::Startup;
  modeSince_ = lastFrame_ = now;
  // The card's first heartbeat, part of the startup animation.
  addPulse(now + 1450, 1.0f, palette::kHeartRed, 200);
  addPulse(now + 1700, 0.5f, palette::kHeartRed, 200);
}

void Display::setMode(Mode mode, uint32_t now) {
  if (mode == mode_) return;
  mode_ = mode;
  modeSince_ = now;
  if (mode == Mode::Parked) {
    for (Pulse& p : pulses_) p.active = false;
  }
}

void Display::addPulse(uint32_t t0, float amp, const RGB& colour, float travelMs) {
  pulses_[nextPulse_] = {t0, amp, colour, travelMs, true};
  nextPulse_ = (nextPulse_ + 1) % kMaxPulses;
}

void Display::beat(uint32_t now, float bpm, float ibiMs) {
  liveColour_ = colourForBpm(bpm);
  const float ibi = ibiMs > 0 ? ibiMs : 60000.0f / bpm;
  const float travel = clampf(0.25f * ibi, 120, 220);
  // "lub" now, a softer "dub" (second heart sound) about a third of a beat later.
  addPulse(now, 1.0f, liveColour_, travel);
  addPulse(now + uint32_t(clampf(0.30f * ibi, 140, 230)), 0.45f, liveColour_, travel);
}

void Display::tentativeBeat(uint32_t now) {
  addPulse(now, 0.45f, mode_ == Mode::Live ? liveColour_ : palette::kScan, 160);
}

void Display::startReadout(int bpm, uint32_t now) {
  readoutColour_ = colourForBpm(bpm);
  uint8_t d[3];
  int n = 0;
  if (bpm >= 100) d[n++] = (bpm / 100) % 10;
  d[n++] = (bpm / 10) % 10;
  d[n++] = bpm % 10;

  uint32_t t = kIntroMs;
  for (int k = 0; k < n; ++k) {
    // Alternate colours so neighbouring digits are easy to tell apart.
    digits_[k] = {t, d[k], k % 2 == 0 ? readoutColour_ : palette::kWhite};
    t += digitDuration(d[k]) + kDigitGapMs;
  }
  digitCount_ = n;
  readoutEndMs_ = t;
  mode_ = Mode::Readout;  // (re)start the readout clock even if one is already showing
  modeSince_ = now;
}

// ---------------------------------------------------------------------------

void Display::render(uint32_t now) {
  px_.clear();

  switch (mode_) {
    case Mode::Startup: renderStartup(now); break;
    case Mode::Idle: renderIdle(now, 1.0f); break;
    case Mode::Acquiring: renderAcquiring(now); break;
    case Mode::Live: renderLive(); break;
    case Mode::Readout: renderReadout(now); break;
    case Mode::Parked: renderParked(); break;
    case Mode::SensorError: renderError(now); break;
  }
  if (mode_ != Mode::Parked) renderPulses(now);

  px_.show();
  lastFrame_ = now;
}

void Display::renderStartup(uint32_t now) {
  const float t = msSince(now, modeSince_);

  // 1) A spark climbs the spinal cord into the brain, leaving a fading trail.
  const float w = (t - 150.0f) / 850.0f;  // wavefront, in depth units
  for (int i = 0; i < layout::kFront; ++i) {
    const float d = layout::depth[i];
    const float x = (d - w) / 0.09f;
    px_.add(i, palette::kSpark * (0.95f * expf(-x * x)));
    if (d < w) px_.add(i, palette::kNerveTeal * (0.5f * expf(-(w - d) / 0.22f)));
  }

  // 2) The brain blooms as the spark arrives.
  const float bloom = 0.9f * flash(t - 950.0f, 80, 300);
  for (int i = 0; i < layout::kFront; ++i) {
    px_.add(i, palette::kBrainViolet * (bloom * smoothstep(0.6f, 0.9f, layout::depth[i])));
  }

  // 3) The heart kicks in: the lub-dub pulses were queued in begin().
  // 4) Ease into the idle glow.
  renderIdle(now, smoothstep(2000, float(kStartupMs), t));
}

void Display::renderIdle(uint32_t now, float level) {
  // Starlight: every LED shimmers faintly at its own pace, and now and then
  // one softly brightens and fades like a twinkling star.
  const float t = now * 0.001f;
  for (int i = 0; i < layout::kFront; ++i) {
    const float shimmer = 0.5f + 0.5f * sinf(kTwoPi * shimmerRate_[i] * t + shimmerPhase_[i]);
    px_.add(i, palette::kIdleBlue * (level * (0.10f + 0.05f * shimmer)));

    Twinkle& tw = twinkles_[i];
    if (tw.amp > 0) {
      const float x = msSince(now, tw.start) / tw.durMs;
      if (x >= 1) {
        tw.amp = 0;
      } else {
        const float e = sinf(float(M_PI) * x);
        px_.add(i, palette::kStarlight * (level * tw.amp * e * e));
      }
    } else if (random(1000) < kTwinkleChancePerMille) {
      tw = {now, uint16_t(random(700, 1600)), random(18, 34) / 100.0f};
    }
  }
  const float breathe = 0.5f + 0.5f * sinf(kTwoPi * t / 5.0f);
  const RGB back = palette::kWarmWhite * (level * (0.30f + 0.10f * breathe));
  px_.add(layout::kBackName, back);
  px_.add(layout::kBackUrl, back);
}

void Display::renderAcquiring(uint32_t now) {
  // A soft scan sweeps up the tree while we wait for a steady rhythm; LEDs
  // below the progress line stay lit as consistent beats come in.
  const float w = fmodf(now / 1200.0f, 1.0f) * 1.4f - 0.2f;
  for (int i = 0; i < layout::kFront; ++i) {
    const float x = (layout::depth[i] - w) / 0.12f;
    float v = 0.35f * expf(-x * x);
    if (progress_ > 0 && layout::depth[i] <= progress_ + 1e-3f) v = std::max(v, 0.30f);
    px_.add(i, palette::kScan * v);
  }
  px_.add(layout::kBackName, palette::kWarmWhite * 0.25f);
  px_.add(layout::kBackUrl, palette::kWarmWhite * 0.25f);
}

void Display::renderLive() {
  // Between beats the tree keeps a faint ember in the heart-rate colour.
  px_.addFront(liveColour_ * 0.14f);
  px_.add(layout::kBackName, palette::kWarmWhite * 0.20f);
  px_.add(layout::kBackUrl, palette::kWarmWhite * 0.20f);
}

void Display::renderReadout(uint32_t now) {
  const float t = msSince(now, modeSince_);

  // Intro: the whole tree swells once in the result colour.
  if (t < kIntroMs) px_.addFront(readoutColour_ * (0.5f * sinf(float(M_PI) * t / kIntroMs)));

  // Each digit is counted out bottom-up: one LED per unit, then a pause.
  // Zero is a single soft flash of the whole tree.
  for (int k = 0; k < digitCount_; ++k) {
    const Digit& d = digits_[k];
    const float local = t - d.startMs;
    if (local < 0 || local >= digitDuration(d.value)) continue;
    if (d.value == 0) {
      px_.addFront(d.colour * (0.35f * sinf(float(M_PI) * local / kZeroMs)));
      continue;
    }
    const float countEnd = float(d.value * kCountStepMs);
    const float fade = 1.0f - clamp01((local - countEnd - kHoldMs) / kFadeMs);
    for (int i = 0; i < d.value && i < layout::kFront; ++i) {
      const float since = local - i * kCountStepMs;
      if (since < 0) break;
      const float v = 0.55f + 0.45f * expf(-since / 120.0f);  // tick, then hold
      px_.add(layout::bottomUp[i], d.colour * (v * fade));
    }
  }
  px_.add(layout::kBackName, readoutColour_ * 0.20f);
  px_.add(layout::kBackUrl, readoutColour_ * 0.20f);
}

void Display::renderParked() {
  px_.addFront(palette::kParked * 0.30f);
  px_.add(layout::kBackName, palette::kParked * 0.30f);
  px_.add(layout::kBackUrl, palette::kParked * 0.30f);
}

void Display::renderError(uint32_t now) {
  const float blink = 0.15f + 0.45f * (0.5f + 0.5f * sinf(kTwoPi * now / 1000.0f));
  px_.add(layout::kBase, palette::kHeartRed * blink);
  px_.add(layout::kBackName, palette::kHeartRed * blink);
  px_.add(layout::kBackUrl, palette::kHeartRed * blink);
}

void Display::renderPulses(uint32_t now) {
  for (Pulse& p : pulses_) {
    if (!p.active) continue;
    const float since = msSince(now, p.t0);
    if (since > p.travelMs + 1200) {
      p.active = false;
      continue;
    }
    // The wave reaches each LED in turn, base first, brain last.
    for (int i = 0; i < layout::kFront; ++i) {
      px_.add(i, p.colour * (p.amp * flash(since - layout::depth[i] * p.travelMs, 30, 170)));
    }
    // The back bullets thump in heart red.
    const RGB back = palette::kHeartRed * (0.8f * p.amp * flash(since - 0.5f * p.travelMs, 30, 200));
    px_.add(layout::kBackName, back);
    px_.add(layout::kBackUrl, back);
  }
}
