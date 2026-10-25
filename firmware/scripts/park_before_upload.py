"""
Pre-upload hook: ask the running CARDio firmware to "park" before esptool resets it.

Why: the MAX30102 shares GPIO8/GPIO9 with the ESP32-C3 boot-strap pins. If the
chip is reset in the middle of an I2C read, the sensor can be left holding SDA
(GPIO9) low, and the next reset boots the ROM bootloader instead of the app --
the card looks dead until it is unplugged. Parking makes the firmware finish
its I2C traffic, put the sensor to sleep and release the bus, so the reset
that esptool performs after flashing always lands on an idle bus.

If the firmware isn't running (blank chip, old firmware, port busy) this does
nothing and the upload proceeds as normal.
"""

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

import time

ESPRESSIF_VID = 0x303A
USB_SERIAL_JTAG_PID = 0x1001


def _find_port(env):
    port = env.subst("$UPLOAD_PORT")
    if port:
        return port
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID and p.pid == USB_SERIAL_JTAG_PID:
            return p.device
    return None


def park_board(source, target, env):
    port = _find_port(env)
    if not port:
        print("[cardio] no board port found to park; uploading anyway")
        return
    try:
        import serial
    except ImportError:
        return

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.1
    # DTR/RTS on the USB-Serial/JTAG port drive the chip's reset logic; opening
    # the port with them idle avoids resetting the board here.
    s.dtr = False
    s.rts = False
    try:
        s.open()
    except Exception as exc:  # port busy (monitor open?) or vanished
        print(f"[cardio] could not open {port} to park the board ({exc}); uploading anyway")
        return

    try:
        s.reset_input_buffer()
        # Parking twice is harmless, so ask a few times in case a reply is lost.
        for _ in range(3):
            s.write(b"\npark\n")
            deadline = time.time() + 0.6
            buf = b""
            while time.time() < deadline:
                buf += s.read(256)
                if b"PARKED" in buf:
                    print(f"[cardio] board on {port} parked the I2C bus; safe to reset")
                    return
        print("[cardio] board did not answer the park request (not running CARDio firmware?); uploading anyway")
    finally:
        s.close()
        time.sleep(0.2)


env.AddPreAction("upload", park_board)  # noqa: F821
