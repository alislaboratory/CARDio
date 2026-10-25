# CARDio firmware

Heart-rate firmware for the CARDio Rev 3.0 business card (ESP32-C3, MAX30102, 13 × WS2812B-2020).

## What it does

| When | LEDs |
| --- | --- |
| Power-on | **Startup animation** (3 s): a spark climbs the spinal cord, the brain lights up violet, then the card gives its first red lub-dub. I2C stays off during this time; see *Soft-brick* below. |
| Nothing on the sensor | **Idle**: starlight. A dim blue shimmer where individual LEDs gently twinkle brighter and fade. The two bullets on the back glow warm white. |
| Thumb on the pad | **Measuring**: a violet scan sweeps upward. Each steady beat fills in one more step from the base, plus a small flash. |
| Locked (5 consistent beats) | **Live**: every real heartbeat sends a lub-dub wave from the base of the cord up into the brain, in sync with your pulse. Colour = rate: blue < 60, teal/green 60–90, amber ≈ 100, red 130+. The back bullets thump red. |
| Thumb lifted | **Readout**: your final BPM is counted out bottom-up, one digit at a time. 72 bpm = 7 LEDs in the rate colour, pause, then 2 LEDs in white. A zero is a single soft flash of the whole tree. |

## Build & flash

Uses [PlatformIO](https://platformio.org/) (VS Code extension or CLI):

```bash
cd firmware
pio run -t upload            # build + flash
pio device monitor           # serial console, type `help`
```

The upload automatically asks the running firmware to **park**: release the I2C bus and put the sensor to sleep before esptool resets the chip.

## Serial commands

The card shows up as a USB serial port (115200 baud; the baud rate doesn't actually matter).

| Command | |
| --- | --- |
| `status` | sensor state, IR level, LED current, pulse amplitude, BPM |
| `plot` | toggle a 100 Hz CSV stream: `ir,red,filtered,amplitude,beat` |
| `demo 72` | fake heartbeats at 72 bpm (show off the LEDs without a thumb); `demo 0` stops |
| `readout 72` | play the BPM readout animation |
| `park` | release I2C so the chip can be reset/flashed safely |
| `reboot` | park, then restart |

## How the heart rate is measured

1. **Sensor**: MAX30102 in SpO2 mode, 400 sps averaged ×4 on-chip = 100 samples/s, 18-bit, 411 µs pulses. The IR channel is used for the pulse; the red LED just makes the pad glow.
2. **Finger detection**: the IR level goes from ~3k counts (nothing there) to ~100k+ with a thumb.
3. **Auto gain**: the IR LED current (1.2–29 mA) is adjusted to keep the signal between 25% and 80% of full scale, so it works for any thumb and any pressure without clipping.
4. **Filtering**: 0.5–4 Hz band-pass (two biquads). After the thumb lands, the filters are held until the IR level stops drifting (the thumb settling onto the pad otherwise looks like one enormous pulse).
5. **Beat detection**: peak picking with hysteresis. All thresholds scale with the last 2 s of peak-to-peak amplitude, so the dicrotic notch and small ripples are ignored.
6. **BPM**: median of the last 7 inter-beat intervals. Intervals > 30% away from the median (missed beats, motion) are rejected. A reading only "locks" after 5 consistent beats in a row whose intervals agree within 35%, so a noisy, pressed-too-hard thumb won't produce a confident wrong number.
7. **Clock**: the MAX30102's sample rate is measured against the ESP32's crystal, so the BPM doesn't inherit the sensor oscillator's tolerance.

## The soft-brick, explained

The MAX30102's **SDA is on GPIO9** and **SCL on GPIO8**, and those are the ESP32-C3's **boot strapping pins**. At every reset the chip samples GPIO9: high runs your program, low starts the ROM bootloader.

The old sketches polled the sensor flat out, so the I2C bus was busy almost 100% of the time. When a reset landed mid-read (which is exactly what happens after every upload), the MAX30102 could be left **holding SDA low** while it waited for the rest of the clock pulses. The chip then booted the bootloader instead of the sketch, and the card looked dead until a power cycle reset the sensor.

This firmware avoids that in four ways:

- **Short bursts**: the FIFO is read every 40 ms, so the bus is busy about 2% of the time instead of about 100%.
- **Park before reset**: `pio run -t upload` (and `reboot`) tell the firmware to finish its I2C traffic, put the sensor to sleep and release both lines first.
- **Bus recovery**: on start-up, if SDA is stuck low, SCL is clocked until the sensor lets go, then a STOP is sent.
- **3 s safe window**: I2C isn't touched for the first 3 s after reset (the startup animation). If a future firmware ever wedges the card: unplug, plug back in, and upload within those 3 s.

The ESP32-C3's built-in USB-Serial/JTAG can reset the chip into the bootloader by itself, so normal uploads don't need the boot button or any wires.

A hardware fix for Rev 3.1: move the I2C lines off GPIO8/9 (and GPIO2), e.g. to GPIO4/5.

## Files

- `src/main.cpp`: app state machine, finger detection, auto gain, serial commands, park/boot safety
- `src/max30102.*`: minimal sensor driver (burst FIFO reads)
- `src/pulse.*`: filters, beat detector, BPM tracker
- `src/display.*`: all LED animations
- `src/leds.*`: LED layout (from the pick-and-place file and schematic) and output with gamma
- `src/config.h`: pins and tuning constants
- `scripts/park_before_upload.py`: PlatformIO pre-upload hook
