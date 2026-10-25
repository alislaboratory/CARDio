#pragma once
#include <Arduino.h>
#include <Wire.h>

// Minimal MAX30102 driver: SpO2 mode (red + IR LEDs), 100 samples/s
// (400 sps averaged x4 on-chip), FIFO read in bursts.
//
// Reads are deliberately bursty: on this board SDA/SCL are the ESP32-C3 strap
// pins, and the less time the bus spends mid-transfer the smaller the chance
// that a reset catches the sensor driving SDA low.
class Max30102 {
 public:
  static constexpr uint8_t kAddress = 0x57;
  static constexpr uint32_t kFullScale = 0x3FFFF;  // 18-bit samples
  static constexpr float kSampleRateHz = 100.0f;

  struct Sample {
    uint32_t red;
    uint32_t ir;
  };

  // Probes, resets and configures the sensor. False if it doesn't answer.
  bool begin(TwoWire& wire, uint8_t redCurrent, uint8_t irCurrent);

  // LED drive current, 0.2 mA per step.
  bool setLedCurrents(uint8_t red, uint8_t ir);

  // Reads up to maxSamples from the FIFO. Returns the count read, or -1 on
  // an I2C error. *overflowed is set if the FIFO filled up and lost samples.
  int readFifo(Sample* out, int maxSamples, bool* overflowed);

  // Low-power mode: LEDs off, registers retained.
  void shutdown();

  uint8_t revision() const { return revision_; }

 private:
  bool writeReg(uint8_t reg, uint8_t value);
  bool readRegs(uint8_t reg, uint8_t* buf, size_t len);

  TwoWire* wire_ = nullptr;
  uint8_t revision_ = 0;
};
