# arc-rpi-oled

C++ userspace driver for an SSD1306 128x64 OLED on a Raspberry Pi. This board is driven over I2C at address `0x3C` on `/dev/i2c-1`. The panel framebuffer builds and tests on macOS; the demo binary is built on the Pi.

## I2C wiring

| Signal | BCM | Header pin |
| --- | --- | --- |
| SDA | 2 | 3 |
| SCL | 3 | 5 |
| 3.3 V and GND |  | 1 or 17, and 6 |

Enable I2C, then confirm the controller answers:

```bash
sudo raspi-config nonint do_i2c 0
sudo usermod -aG i2c "$USER"
i2cdetect -y 1
```

Log in again after changing groups. `0x3c` in the detect grid is this panel. Some modules use `0x3d`.

## SPI wiring

| Signal | BCM | Header pin |
| --- | --- | --- |
| MOSI | 10 | 19 |
| SCLK | 11 | 23 |
| CS (CE0) | 8 | 24 |
| D/C | 24 | 18 |
| RESET | 25 | 22 |
| Button (optional, to GND) | 23 | 16 |
| 3.3 V and GND |  | 1 or 17, and 20 |

Chip select stays with the spidev driver. Wire CS to CE0. Enable it with `sudo raspi-config nonint do_spi 0` and confirm `/dev/spidev0.0` exists after reboot.

The optional button uses the internal pull-up and is active low. A falling edge inverts the panel. On Pi 5 the header is often `/dev/gpiochip4`. The button needs the `gpio` group.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

On the Pi the same commands also build `oled_demo`:

```bash
./build/oled_demo
./build/oled_demo --address 0x3d
./build/oled_demo --button
```

Without `--button` the program draws a border and a filled block, then exits. `--button` waits for a press on BCM 23 until Ctrl-C.

## Hardware check

`scripts/oled_check.py` talks to the panel over I2C. It holds a solid white screen for 3 seconds, then draws a border and the letters OK.

```bash
python3 scripts/oled_check.py
python3 scripts/oled_check.py --address 0x3d
```
