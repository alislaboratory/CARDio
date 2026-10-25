#include "leds.h"

#include <Adafruit_NeoPixel.h>
#include <math.h>

#include <algorithm>

#include "config.h"

namespace layout {
namespace {

// Pick-and-place centres (mm, y up) for each chain index on the front.
struct Pos {
  float x, y;
};
constexpr Pos kPos[kFront] = {
    {71.50f, 78.87f},  // 0  LED9
    {71.25f, 75.18f},  // 1  LED10
    {69.47f, 68.83f},  // 2  LED3
    {67.06f, 61.60f},  // 3  LED1
    {66.80f, 57.40f},  // 4  LED2
    {68.83f, 53.21f},  // 5  LED4
    {69.72f, 64.77f},  // 6  LED6
    {74.30f, 70.49f},  // 7  LED5
    {77.72f, 72.26f},  // 8  LED7
    {66.04f, 72.14f},  // 9  LED8
    {63.25f, 59.31f},  // 10 LED12
};

// Nerve tree edges, following the silkscreen artwork.
constexpr uint8_t kEdges[][2] = {
    {5, 4}, {4, 3}, {4, 10}, {3, 6}, {6, 2}, {2, 1}, {1, 0}, {2, 9}, {2, 7}, {7, 8},
};
constexpr int kEdgeCount = sizeof(kEdges) / sizeof(kEdges[0]);

float edgeLength(int a, int b) { return hypotf(kPos[a].x - kPos[b].x, kPos[a].y - kPos[b].y); }

}  // namespace

float depth[kFront];
uint8_t bottomUp[kFront];

int neighbours(int led, uint8_t out[4]) {
  int n = 0;
  for (int e = 0; e < kEdgeCount && n < 4; ++e) {
    if (kEdges[e][0] == led) out[n++] = kEdges[e][1];
    else if (kEdges[e][1] == led) out[n++] = kEdges[e][0];
  }
  return n;
}

void init() {
  // Path length from the base along the tree (Bellman-Ford; it's 11 nodes).
  for (float& d : depth) d = INFINITY;
  depth[kBase] = 0;
  for (int pass = 0; pass < kFront; ++pass) {
    for (const auto& e : kEdges) {
      const float len = edgeLength(e[0], e[1]);
      depth[e[1]] = std::min(depth[e[1]], depth[e[0]] + len);
      depth[e[0]] = std::min(depth[e[0]], depth[e[1]] + len);
    }
  }
  const float longest = *std::max_element(depth, depth + kFront);
  for (float& d : depth) d /= longest;

  for (int i = 0; i < kFront; ++i) bottomUp[i] = i;
  std::sort(bottomUp, bottomUp + kFront, [](uint8_t a, uint8_t b) { return depth[a] < depth[b]; });
}

}  // namespace layout

namespace {
Adafruit_NeoPixel strip(layout::kCount, pins::kLedData, NEO_GRB + NEO_KHZ800);

uint8_t toByte(float v) {
  return uint8_t(lroundf(powf(clamp01(v), cfg::kGamma) * cfg::kBrightness));
}
}  // namespace

void Pixels::begin() {
  layout::init();
  strip.begin();
  clear();
  show();
}

void Pixels::clear() {
  for (RGB& c : fb_) c = RGB{};
}

void Pixels::addFront(const RGB& c) {
  for (int i = 0; i < layout::kFront; ++i) fb_[i] += c;
}

void Pixels::show() {
  for (int i = 0; i < layout::kCount; ++i) {
    strip.setPixelColor(i, toByte(fb_[i].r), toByte(fb_[i].g), toByte(fb_[i].b));
  }
  strip.show();
}
