# Legacy sketches

Arduino sketches from bringing up the Rev 3.0 board, kept for reference. They're superseded by [`firmware/`](../firmware/).

| Sketch | What it tried |
| --- | --- |
| `test_wifi.ino` | Starts a Wi-Fi access point serving a "Hello World" page |
| `test_leds.ino` | LED sweep test, with ArduinoOTA for updates over Wi-Fi |
| `measure_hr_wifi.ino` | Heart rate with the SparkFun MAX3010x library, shown on a live web page, with OTA |
| `static_bpm_light.ino` | First LED heartbeat: a fixed sweep animation while a finger is detected |

They need the SparkFun MAX3010x and Adafruit NeoPixel libraries. To try the Wi-Fi ones, fill in your own `WIFI_SSID`, `WIFI_PASSWORD` and `OTA_PASSWORD`.

These sketches poll the sensor non-stop, which can leave the board stuck in its bootloader after a reset. See [the soft-brick explanation](../firmware/README.md#the-soft-brick-explained). If that happens, unplug the card, plug it back in, and flash the new firmware.
