#pragma once
#include <math.h>

// Linear-ish colour in 0..1 per channel (gamma is applied on output).
struct RGB {
  float r = 0, g = 0, b = 0;
};

inline RGB operator*(const RGB& c, float k) { return {c.r * k, c.g * k, c.b * k}; }
inline RGB operator+(const RGB& a, const RGB& b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
inline RGB& operator+=(RGB& a, const RGB& b) { return a = a + b; }

inline RGB lerp(const RGB& a, const RGB& b, float t) { return a * (1 - t) + b * t; }

inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

inline float smoothstep(float edge0, float edge1, float x) {
  const float t = clamp01((x - edge0) / (edge1 - edge0));
  return t * t * (3 - 2 * t);
}

namespace palette {
constexpr RGB kHeartRed{1.00f, 0.02f, 0.06f};
constexpr RGB kSpark{0.70f, 0.92f, 1.00f};
constexpr RGB kNerveTeal{0.00f, 0.55f, 0.80f};
constexpr RGB kBrainViolet{0.55f, 0.20f, 1.00f};
constexpr RGB kIdleBlue{0.10f, 0.22f, 1.00f};
constexpr RGB kStarlight{0.55f, 0.70f, 1.00f};
constexpr RGB kScan{0.60f, 0.08f, 1.00f};  // violet: distinct from the idle blue
constexpr RGB kWarmWhite{1.00f, 0.80f, 0.55f};
constexpr RGB kWhite{1.00f, 1.00f, 1.00f};
constexpr RGB kParked{0.55f, 0.00f, 1.00f};
}  // namespace palette

// Heart-rate colour: calm blue (bradycardia) -> teal -> green (normal resting)
// -> amber (100+, tachycardia) -> red.
inline RGB colourForBpm(float bpm) {
  struct Stop {
    float bpm;
    RGB c;
  };
  static const Stop stops[] = {
      {45, {0.10f, 0.25f, 1.00f}}, {60, {0.00f, 0.80f, 0.80f}}, {75, {0.10f, 1.00f, 0.25f}},
      {100, {1.00f, 0.70f, 0.00f}}, {120, {1.00f, 0.30f, 0.00f}}, {140, {1.00f, 0.00f, 0.08f}},
  };
  constexpr int n = sizeof(stops) / sizeof(stops[0]);
  if (bpm <= stops[0].bpm) return stops[0].c;
  for (int i = 1; i < n; ++i) {
    if (bpm <= stops[i].bpm) {
      const float t = (bpm - stops[i - 1].bpm) / (stops[i].bpm - stops[i - 1].bpm);
      return lerp(stops[i - 1].c, stops[i].c, t);
    }
  }
  return stops[n - 1].c;
}
