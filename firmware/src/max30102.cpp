#include "max30102.h"

namespace {

enum Reg : uint8_t {
  kIntrEnable1 = 0x02,
  kIntrEnable2 = 0x03,
  kFifoWrPtr = 0x04,  // 0x04..0x06 = WR_PTR, OVF_COUNTER, RD_PTR
  kFifoOvf = 0x05,
  kFifoRdPtr = 0x06,
  kFifoData = 0x07,
  kFifoConfig = 0x08,
  kModeConfig = 0x09,
  kSpo2Config = 0x0A,
  kLed1Pa = 0x0C,  // red
  kLed2Pa = 0x0D,  // IR
  kRevId = 0xFE,
  kPartId = 0xFF,
};

constexpr uint8_t kPartIdValue = 0x15;

constexpr uint8_t kModeReset = 0x40;
constexpr uint8_t kModeShutdown = 0x80;
constexpr uint8_t kModeSpo2 = 0x03;  // red + IR

// FIFO_CONFIG: average 4 samples, roll over when full, almost-full irrelevant.
constexpr uint8_t kFifoConfigValue = (0b010 << 5) | (1 << 4) | 0x0F;

// SPO2_CONFIG: ADC range 4096 nA, 400 sps, 411 us pulses (18-bit).
constexpr uint8_t kSpo2ConfigValue = (0b01 << 5) | (0b011 << 2) | 0b11;

constexpr int kBytesPerSample = 6;
// Wire's buffer is 128 bytes; stay under it.
constexpr int kMaxBurstSamples = 20;

}  // namespace

bool Max30102::begin(TwoWire& wire, uint8_t redCurrent, uint8_t irCurrent) {
  wire_ = &wire;

  uint8_t id = 0;
  if (!readRegs(kPartId, &id, 1) || id != kPartIdValue) return false;
  readRegs(kRevId, &revision_, 1);

  writeReg(kModeConfig, kModeReset);
  for (int i = 0; i < 20; ++i) {  // reset bit self-clears in < 1 ms
    delay(1);
    uint8_t mode = kModeReset;
    if (readRegs(kModeConfig, &mode, 1) && !(mode & kModeReset)) break;
  }

  bool ok = writeReg(kIntrEnable1, 0x00) &&  // INT# isn't wired to the ESP32
            writeReg(kIntrEnable2, 0x00) &&
            writeReg(kFifoConfig, kFifoConfigValue) &&
            writeReg(kSpo2Config, kSpo2ConfigValue) &&
            setLedCurrents(redCurrent, irCurrent) &&
            writeReg(kFifoWrPtr, 0) && writeReg(kFifoOvf, 0) && writeReg(kFifoRdPtr, 0) &&
            writeReg(kModeConfig, kModeSpo2);
  return ok;
}

bool Max30102::setLedCurrents(uint8_t red, uint8_t ir) {
  return writeReg(kLed1Pa, red) && writeReg(kLed2Pa, ir);
}

int Max30102::readFifo(Sample* out, int maxSamples, bool* overflowed) {
  uint8_t ptr[3];  // WR_PTR, OVF_COUNTER, RD_PTR
  if (!readRegs(kFifoWrPtr, ptr, sizeof(ptr))) return -1;

  int available = (ptr[0] - ptr[2]) & 0x1F;
  const bool lost = ptr[1] != 0;
  if (lost && available == 0) available = 32;  // full: pointers wrapped around
  if (overflowed) *overflowed = lost;

  const int n = min(available, maxSamples);
  int done = 0;
  while (done < n) {
    const int chunk = min(n - done, kMaxBurstSamples);
    uint8_t buf[kMaxBurstSamples * kBytesPerSample];
    if (!readRegs(kFifoData, buf, chunk * kBytesPerSample)) return done > 0 ? done : -1;
    for (int i = 0; i < chunk; ++i) {
      const uint8_t* b = buf + i * kBytesPerSample;
      out[done + i].red = ((uint32_t(b[0]) << 16) | (uint32_t(b[1]) << 8) | b[2]) & kFullScale;
      out[done + i].ir = ((uint32_t(b[3]) << 16) | (uint32_t(b[4]) << 8) | b[5]) & kFullScale;
    }
    done += chunk;
  }
  return done;
}

void Max30102::shutdown() {
  if (wire_) writeReg(kModeConfig, kModeShutdown | kModeSpo2);
}

bool Max30102::writeReg(uint8_t reg, uint8_t value) {
  wire_->beginTransmission(kAddress);
  wire_->write(reg);
  wire_->write(value);
  return wire_->endTransmission() == 0;
}

bool Max30102::readRegs(uint8_t reg, uint8_t* buf, size_t len) {
  wire_->beginTransmission(kAddress);
  wire_->write(reg);
  if (wire_->endTransmission(false) != 0) return false;
  const size_t got = wire_->requestFrom(static_cast<uint16_t>(kAddress), len, true);
  if (got != len) {
    while (wire_->available()) wire_->read();
    return false;
  }
  for (size_t i = 0; i < len; ++i) buf[i] = wire_->read();
  return true;
}
