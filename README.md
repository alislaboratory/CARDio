# CARDio - A PCB Business Card
![alt text](<Screenshot 2025-08-15 at 11.21.53.png>)
![alt text](<Screenshot 2025-08-15 at 11.21.58.png>)
![alt text](image-1.png)
![alt text](image.png)

## Project Description
CARDio is a PCB business card with an integrated electrocardiogram (ECG). Essentially it uses a MAX30102 heart-rate sensor to measure heart rate and pulse oximetry data, and display this using 12 NeoPixel RGB LEDs. It is controlled by an ESP32-C3 microcontroller which is both WiFi and BLE enabled.

## Why?
I had tinkered with the idea of PCB business cards in the past. My first iteration was essentially just a blank PCB with a silkscreen. It did have circuitry for an ATTiny85 and some NeoPixels, though I never got it to work properly. I liked the idea of handing a business card to somebody that wouldn't be thrown away - one that would actually have a use, like an NFC tag, flashing lights, or an ECG. Perhaps this was a little overkill, but I wanted something to hand to medical professionals or surgeons at conferences that would leave a memorable impression. It also demonstrates my interest to combine medicine and technology, in this case embedded electronics. 

## How it Works
This is my first actual PCB that does something, so it is not very complicated. It uses USB-C power to run the board, going into 2 LDOs - one for 3.3V power (NeoPixels and ESP32-C3), and one for 1.8V power (MAX30102 sensor). Most of the passive components are the boilerplate to get the microcontroller running, the components are actually fairly simple and integrated. Then, the MAX30102 communicates to the ESP32-C3 via I2C, and the NeoPixels simply communicate with a single data wire and are daisy-chained together. The ESP32-C3 has an integrated USB programmer, so it doesnt't need any extra circuitry for that. 

## Future Directions and Problems
On this first iteration (Rev 3.0, I will explain why it's the 3rd below), I wasn't expecting everything to work. Luckily, everything worked as expected upon delivery. To be fair, it was a simple circuit but it was my first time designing a PCB and I was fairly sure something was going to wrong. And although minor, it did. I realised after the fact that I had accidentally wired my I2C pins to the boot-mode control pins of the ESP32-C3, which meant sometimes the I2C communication wouldn't let me program the board as the boot control lines were being pulled low or at random states. This problem was solved in two ways:
- Use ArduinoOTA programming for all future programs. - This way, I could program via USB once the first time, then use WiFi to program every other time. However, it is a hassle and takes program memory.
- Add a small delay before activating I2C communications to allow for programming time (5 seconds or so). This would also be a little more convenient, however it would mean everytime the board ran, I would have to wait a few seconds for everything to fire up, which is okay, but not ideal.

I soft bricked two of my boards, because as soon as they turn on, I2C activates and I can't program them because the boot-mode control pins are being used. I will have to use some thin wires to reset the bootloader and reprogram them with the updated solutions.

For Rev 3.1, I will introduce the following new features:
- Battery and potentially battery charging (coin cell)
- Small OLED screen (0.91" screen) for displaying ECG and metrics
- Other sensors
- And many more I haven't thought of yet.


