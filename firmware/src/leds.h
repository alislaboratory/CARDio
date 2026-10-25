#pragma once
#include <stdint.h>

#include "color.h"

// Physical layout of the 13 WS2812B-2020s, in chain order (GPIO3 -> LED9 -> ...).
//
// Front: 11 LEDs drawn as a nervous system -- roots, a spinal cord and a brain.
// Back:  2 LEDs used as bullets next to the name and the URL.
//
//              0          brain top (LED9)
//              1          brain centre (LED10)
//        9           8    left lobe (LED8), right lobe outer (LED7)
//                 7       right lobe inner (LED5)
//             2           brainstem junction (LED3)
//             6           spinal cord (LED6)
//           3             spinal cord (LED1)
//       10                nerve root branch (LED12)
//           4             spinal cord (LED2)
//             5           base of the cord (LED4)
namespace layout {

constexpr int kCount = 13;
constexpr int kFront = 11;    // chain indices 0..10
constexpr int kBackName = 11; // LED13, beside "Ali Ashrafy : Medical Student"
constexpr int kBackUrl = 12;  // LED14, beside "ashrafy.au"
constexpr int kBase = 5;      // bottom of the spinal cord

// Distance along the nerves from the base (0) to the furthest LED (1).
extern float depth[kFront];
// Front LEDs ordered bottom -> top (by depth); used for counting animations.
extern uint8_t bottomUp[kFront];

// Tree neighbours of a front LED. Returns how many were written to out (max 4).
int neighbours(int led, uint8_t out[4]);

void init();

}  // namespace layout

// Frame buffer + output with gamma and a brightness cap.
class Pixels {
 public:
  void begin();
  void clear();
  void add(int i, const RGB& c) { fb_[i] += c; }
  void addFront(const RGB& c);
  void show();

 private:
  RGB fb_[layout::kCount];
};
