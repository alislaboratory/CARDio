// CARDio -- PCB business card heart-rate monitor
// ESP32-C3 + MAX30102 pulse sensor + 13 WS2812B-2020 LEDs
//
// Place a thumb on the sensor: the nerve tree pulses in time with your heart,
// coloured by rate. Lift it and the final BPM is counted out on the LEDs.
//
// About the "soft brick": the MAX30102's SDA/SCL lines are on GPIO9/GPIO8,
// which the ESP32-C3 samples at reset to pick its boot mode (GPIO9 low =
// ROM bootloader). If a reset lands mid I2C read, the sensor can be left
// driving SDA low and the chip boots the bootloader instead of this app. So:
//   * I2C stays off for the first 3 s after reset (the startup animation);
//   * the sensor is read in short bursts (~2% bus time), not polled flat out;
//   * a stuck bus is clocked free before I2C starts;
//   * "park" (sent automatically by `pio run -t upload`) releases the bus
//     before the upload resets the chip, and "reboot" does the same.

#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>

#include <stdarg.h>

#include <algorithm>

#include "config.h"
#include "display.h"
#include "max30102.h"
#include "pulse.h"

namespace {

enum class AppState : uint8_t { Booting, Running, Parked };

constexpr uint32_t kParkTimeoutMs = 60000;

AppState appState = AppState::Booting;
uint32_t parkedAt = 0;

Display display;
Max30102 sensor;
BeatDetector detector;
BpmTracker tracker;

bool sensorOk = false;
uint32_t lastSensorTry = 0, lastPoll = 0;
int i2cFailures = 0;
bool busWasStuck = false;

uint8_t irCurrent = cfg::kIrCurrentDefault;
uint32_t lastGainChange = 0;
float irLevel = 0;  // smoothed raw IR: finger detection and auto gain
bool fingerOn = false;
int fingerDebounce = 0;
int settle = 0;  // samples left with the detector held (finger landing, gain change)
bool waitForStill = false;     // after the finger lands, hold until the level stops moving
int samplesSinceFinger = 0;
uint32_t recentIr[cfg::kStillWindow];
int recentCount = 0, recentPos = 0;

uint32_t lastBeatAt = 0;
float lastLockedBpm = 0;
uint32_t lastLockedAt = 0;

bool plotStream = false;
uint32_t longestStallMs = 0;
float demoBpm = 0;
uint32_t nextDemoBeat = 0;

// The MAX30102's own oscillator sets the sample rate. Measure it against
// millis() so beat intervals (and so the BPM) don't inherit its error.
struct SampleClock {
  uint32_t since = 0;
  uint32_t count = 0;
  float periodMs = 1000.0f / Max30102::kSampleRateHz;

  void restart(uint32_t now) {
    since = now;
    count = 0;
  }
  void add(int n, uint32_t now) {
    count += n;
    const uint32_t elapsed = now - since;
    if (elapsed < 10000) return;
    const float measured = float(elapsed) / count;
    if (measured > 9.0f && measured < 11.0f) periodMs += 0.3f * (measured - periodMs);
    restart(now);
  }
} sampleClock;

// Serial output that never waits: if the USB TX buffer can't take the whole
// line (e.g. plugged into a computer with nothing reading the port), drop it.
void logf(const char* fmt, ...) {
  char buf[200];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n <= 0) return;
  const size_t len = std::min(size_t(n), sizeof(buf) - 1);
  if (Serial.availableForWrite() >= int(len)) Serial.write(reinterpret_cast<const uint8_t*>(buf), len);
}

const char* resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    default: return "other";
  }
}

// ---------------------------------------------------------------------------
// I2C / sensor
// ---------------------------------------------------------------------------

// If a reset interrupted a read, the sensor may still be holding SDA low while
// it waits for clocks. Clock it out and finish with START+STOP.
bool recoverI2cBus() {
  pinMode(pins::kI2cSda, INPUT_PULLUP);
  pinMode(pins::kI2cScl, INPUT_PULLUP);
  delayMicroseconds(10);
  if (digitalRead(pins::kI2cSda) == HIGH) return false;

  pinMode(pins::kI2cScl, OUTPUT_OPEN_DRAIN);
  for (int i = 0; i < 9 && digitalRead(pins::kI2cSda) == LOW; ++i) {
    digitalWrite(pins::kI2cScl, LOW);
    delayMicroseconds(5);
    digitalWrite(pins::kI2cScl, HIGH);
    delayMicroseconds(5);
  }
  pinMode(pins::kI2cSda, OUTPUT_OPEN_DRAIN);
  digitalWrite(pins::kI2cSda, LOW);
  delayMicroseconds(5);
  digitalWrite(pins::kI2cSda, HIGH);
  delayMicroseconds(5);
  pinMode(pins::kI2cSda, INPUT);
  pinMode(pins::kI2cScl, INPUT);
  return true;
}

void startSensor(uint32_t now) {
  lastSensorTry = now;
  if (recoverI2cBus()) {
    busWasStuck = true;
    logf("i2c: SDA was held low -- clocked the bus free\n");
  }
  Wire.begin(pins::kI2cSda, pins::kI2cScl, 400000);
  Wire.setTimeOut(20);

  irCurrent = cfg::kIrCurrentDefault;
  sensorOk = sensor.begin(Wire, cfg::kRedCurrent, irCurrent);
  if (!sensorOk) {
    Wire.end();
    logf("sensor: MAX30102 not answering, will retry\n");
    return;
  }
  detector.begin(Max30102::kSampleRateHz);
  sampleClock.restart(now);
  lastPoll = now;
  i2cFailures = 0;
  logf("sensor: MAX30102 rev %u ready\n", sensor.revision());
}

// Leave the strap pins exactly as they are at power-up: sensor quiet, bus idle,
// both lines released to their pull-ups.
void releaseI2c() {
  if (sensorOk) sensor.shutdown();
  Wire.end();
  pinMode(pins::kI2cSda, INPUT);
  pinMode(pins::kI2cScl, INPUT);
  sensorOk = false;
}

void fingerPlaced(uint32_t ir, uint32_t now) {
  fingerOn = true;
  fingerDebounce = 0;
  tracker.reset();
  settle = cfg::kSettleSamples;
  waitForStill = true;
  samplesSinceFinger = recentCount = recentPos = 0;
  lastGainChange = lastBeatAt = now;
  lastLockedBpm = 0;
  logf("finger on (IR %lu)\n", (unsigned long)ir);
}

void fingerRemoved(uint32_t now) {
  fingerOn = false;
  fingerDebounce = 0;
  if (irCurrent != cfg::kIrCurrentDefault) {
    irCurrent = cfg::kIrCurrentDefault;
    sensor.setLedCurrents(cfg::kRedCurrent, irCurrent);
  }
  if (lastLockedBpm > 0 && now - lastLockedAt < 5000) {
    const int bpm = lroundf(lastLockedBpm);
    logf("finger off -- reading: %d bpm\n", bpm);
    display.startReadout(bpm, now);
  } else {
    logf("finger off (no steady reading)\n");
  }
  tracker.reset();
}

// Keep the IR level inside a comfortable band: big pulses, no clipping.
void adjustGain(uint32_t ir, uint32_t now) {
  if (now - lastGainChange < cfg::kAgcIntervalMs) return;
  const float level = irLevel / Max30102::kFullScale;
  const bool clipping = ir >= Max30102::kFullScale - 2000;

  int next = irCurrent;
  if (clipping || level > cfg::kAgcHigh) next = int(irCurrent * 0.7f);
  else if (level < cfg::kAgcLow) next = int(irCurrent * 1.4f) + 1;
  next = constrain(next, int(cfg::kIrCurrentMin), int(cfg::kIrCurrentMax));
  if (next == irCurrent) return;

  irCurrent = uint8_t(next);
  lastGainChange = now;
  sensor.setLedCurrents(cfg::kRedCurrent, irCurrent);
  settle = std::max(settle, 25);  // let the new level flush through the FIFO and filters
  logf("gain: IR level %.0f%% -> LED %.1f mA\n", level * 100, irCurrent * 0.2f);
}

void onBeat(float ibiMs, uint32_t now) {
  lastBeatAt = now;
  const bool consistent = ibiMs > 0 && tracker.add(ibiMs);
  if (tracker.locked() && consistent) {
    lastLockedBpm = tracker.bpm();
    lastLockedAt = now;
    display.beat(now, tracker.bpm(), ibiMs);
  } else {
    display.tentativeBeat(now);
  }
  if (!plotStream) {
    logf("beat  ibi %5.0f ms  %-6s bpm %5.1f%s\n", ibiMs, consistent ? "ok" : "skip",
                  tracker.bpm(), tracker.locked() ? "  [locked]" : "");
  }
}

// True once the IR level has stopped drifting (thumb settled on the pad), or
// after kMaxSettleSamples regardless.
bool thumbIsStill(uint32_t ir) {
  recentIr[recentPos] = ir;
  recentPos = (recentPos + 1) % cfg::kStillWindow;
  if (recentCount < cfg::kStillWindow) ++recentCount;
  if (++samplesSinceFinger >= cfg::kMaxSettleSamples) return true;
  if (recentCount < cfg::kStillWindow) return false;
  const auto range = std::minmax_element(recentIr, recentIr + cfg::kStillWindow);
  return *range.second - *range.first <= cfg::kStillFraction * irLevel;
}

void processSample(const Max30102::Sample& s, uint32_t now) {
  irLevel += (float(s.ir) - irLevel) * 0.1f;

  if (!fingerOn) {
    fingerDebounce = irLevel > cfg::kFingerOnCounts ? fingerDebounce + 1 : 0;
    if (fingerDebounce >= cfg::kFingerOnSamples) fingerPlaced(s.ir, now);
  } else {
    fingerDebounce = irLevel < cfg::kFingerOffCounts ? fingerDebounce + 1 : 0;
    if (fingerDebounce >= cfg::kFingerOffSamples) fingerRemoved(now);
  }

  float ibi = -1;
  if (fingerOn) {
    adjustGain(s.ir, now);
    if (waitForStill) waitForStill = !thumbIsStill(s.ir);
    if (settle > 0 || waitForStill) {
      settle = std::max(settle - 1, 0);
      detector.prime(float(s.ir));
    } else {
      ibi = detector.process(float(s.ir), sampleClock.periodMs);
    }
  }

  if (plotStream) {
    logf("%lu,%lu,%.1f,%.1f,%d\n", (unsigned long)s.ir, (unsigned long)s.red,
                  detector.signal(), detector.amplitude(), ibi >= 0 ? 1 : 0);
  }
  if (ibi >= 0) onBeat(ibi, now);
}

void serviceSensor(uint32_t now) {
  if (!sensorOk) {
    if (now - lastSensorTry >= 2000) startSensor(now);
    return;
  }
  if (now - lastPoll < cfg::kSensorPollMs) return;
  lastPoll = now;

  Max30102::Sample samples[32];
  bool lost = false;
  const int n = sensor.readFifo(samples, 32, &lost);
  if (n < 0) {
    if (++i2cFailures >= 5) {
      logf("sensor: lost contact, restarting it\n");
      if (fingerOn) fingerRemoved(now);
      releaseI2c();
    }
    return;
  }
  i2cFailures = 0;
  if (lost) sampleClock.restart(now);
  else sampleClock.add(n, now);
  for (int i = 0; i < n; ++i) processSample(samples[i], now);

  // No beats for a while (pressing too hard, moving): start the lock over.
  if (fingerOn && (tracker.locked() || tracker.streak() > 0)) {
    const float timeout = std::max(3000.0f, 2.5f * tracker.medianIbi());
    if (now - lastBeatAt > timeout) {
      logf("signal lost -- re-acquiring\n");
      tracker.reset();
    }
  }
}

// ---------------------------------------------------------------------------
// Serial commands
// ---------------------------------------------------------------------------

void park(uint32_t now) {
  releaseI2c();
  fingerOn = false;
  plotStream = false;  // so the reply isn't stuck behind a full TX buffer
  appState = AppState::Parked;
  parkedAt = now;
  display.setMode(Display::Mode::Parked, now);
  display.render(now);
  // No Serial.flush(): HWCDC::flush() can discard the buffer when nothing is reading.
  logf("PARKED: I2C released, safe to reset/flash (resumes in 60 s or on 'reboot')\n");
}

void printStatus(uint32_t now) {
  logf("state %s | sensor %s | finger %s\n",
                appState == AppState::Parked ? "parked" : appState == AppState::Booting ? "booting" : "running",
                sensorOk ? "ok" : "offline", fingerOn ? "on" : "off");
  logf("IR level %.0f (%.0f%% FS) | IR LED %.1f mA | sample period %.3f ms\n", irLevel,
                100 * irLevel / Max30102::kFullScale, irCurrent * 0.2f, sampleClock.periodMs);
  logf("pulse amplitude %.0f (perfusion %.2f%%) | bpm %.1f | streak %d | %s\n",
                detector.amplitude(), detector.dc() > 0 ? 100 * detector.amplitude() / detector.dc() : 0,
                tracker.bpm(), tracker.streak(), tracker.locked() ? "locked" : "not locked");
  logf("uptime %lus | reset reason %s | i2c bus was stuck: %s\n", (unsigned long)(now / 1000),
                resetReason(), busWasStuck ? "yes" : "no");
  logf("longest main-loop stall since boot: %lu ms\n", (unsigned long)longestStallMs);
}

void printHelp() {
  // One line at a time: logf drops anything bigger than the free TX space.
  logf("commands:\n");
  logf("  status        sensor / heart-rate state\n");
  logf("  plot          toggle CSV stream: ir,red,filtered,amplitude,beat (100 Hz)\n");
  logf("  demo <bpm>    fake heartbeats to show off the LEDs (demo 0 = off)\n");
  logf("  readout <bpm> play the BPM readout animation\n");
  logf("  park          release I2C so the chip can be reset/flashed safely\n");
  logf("  reboot        park, then restart\n");
}

void handleCommand(String cmd, uint32_t now) {
  cmd.trim();
  cmd.toLowerCase();
  if (cmd == "park") {
    park(now);
  } else if (cmd == "reboot") {
    park(now);
    delay(50);
    ESP.restart();
  } else if (cmd == "status" || cmd == "s") {
    printStatus(now);
  } else if (cmd == "plot" || cmd == "p") {
    plotStream = !plotStream;
    if (plotStream) logf("ir,red,filtered,amplitude,beat\n");
  } else if (cmd.startsWith("demo")) {
    demoBpm = constrain(cmd.substring(4).toFloat(), 0.0f, cfg::kMaxBpm);
    nextDemoBeat = now;
    logf("demo %s\n", demoBpm > 0 ? String(demoBpm, 0).c_str() : "off");
  } else if (cmd.startsWith("readout")) {
    const int bpm = cmd.substring(7).toInt();
    if (bpm > 0 && bpm < 1000) display.startReadout(bpm, now);
  } else if (cmd == "help" || cmd == "?") {
    printHelp();
  } else if (cmd.length() > 0) {
    logf("unknown command -- try 'help'\n");
  }
}

void pollSerial(uint32_t now) {
  static String line;
  while (Serial.available()) {
    const char c = char(Serial.read());
    if (c == '\n' || c == '\r') {
      if (line.length() > 0) handleCommand(line, now);
      line = "";
    } else if (line.length() < 32) {
      line += c;
    }
  }
}

// ---------------------------------------------------------------------------

void updateDisplayMode(uint32_t now) {
  using Mode = Display::Mode;
  if (appState == AppState::Booting) return;  // startup animation runs its course
  if (appState == AppState::Parked) {
    display.setMode(Mode::Parked, now);
    return;
  }

  if (demoBpm > 0) {
    if (int32_t(now - nextDemoBeat) >= 0) {
      const float ibi = 60000.0f / demoBpm;
      display.beat(now, demoBpm, ibi);
      nextDemoBeat = now + uint32_t(ibi);
    }
    display.setMode(Mode::Live, now);
    return;
  }
  if (!sensorOk) {
    display.setMode(Mode::SensorError, now);
    return;
  }
  if (fingerOn) {
    display.setProgress(float(tracker.streak()) / cfg::kLockBeats);
    display.setMode(tracker.locked() ? Mode::Live : Mode::Acquiring, now);
    return;
  }
  if (display.mode() == Mode::Readout && !display.readoutDone(now)) return;
  display.setMode(Mode::Idle, now);
}

}  // namespace

void setup() {
  // GPIO8/9 are boot straps: leave them alone (inputs, pulled up) for now.
  pinMode(pins::kI2cSda, INPUT);
  pinMode(pins::kI2cScl, INPUT);

  Serial.begin(115200);
  // Must be non-zero: HWCDC::write() counts a retry counter down from this
  // value, and 0 wraps around to ~4 billion -- the firmware hung as soon as the
  // TX buffer filled (plugged into a computer with no terminal reading).
  // logf() also never writes more than fits, so output can't block at all.
  Serial.setTxTimeoutMs(2);

  display.begin(millis());
  logf("\nCARDio -- reset reason: %s. Type 'help' for commands.\n", resetReason());
}

void loop() {
  const uint32_t now = millis();
  static uint32_t lastLoop = now;
  longestStallMs = std::max(longestStallMs, now - lastLoop);  // freeze diagnostic, see `status`
  lastLoop = now;
  pollSerial(now);

  switch (appState) {
    case AppState::Booting:
      if (now >= cfg::kBootSafeWindowMs) {
        appState = AppState::Running;
        startSensor(now);
      }
      break;
    case AppState::Running:
      serviceSensor(now);
      break;
    case AppState::Parked:
      if (now - parkedAt >= kParkTimeoutMs) {
        appState = AppState::Running;
        logf("park timed out, resuming\n");
        startSensor(now);
      }
      break;
  }

  updateDisplayMode(now);

  static uint32_t lastFrame = 0;
  if (now - lastFrame >= cfg::kFrameMs) {
    lastFrame = now;
    display.render(now);
  }
  delay(1);
}
