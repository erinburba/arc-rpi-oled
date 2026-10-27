# arc-rpi-oled

C++ userspace driver for a 4-wire SSD1306 128x64 OLED on a Raspberry Pi. SPI goes through `/dev/spidev`. D/C, RESET, and an optional button use the GPIO character device. The panel framebuffer builds and tests on macOS; the demo binary is built on the Pi.

## Wiring

| Signal | BCM | Header pin |
| --- | --- | --- |
| MOSI | 10 | 19 |
| SCLK | 11 | 23 |
| CS (CE0) | 8 | 24 |
| D/C | 24 | 18 |
| RESET | 25 | 22 |
| Button (optional, to GND) | 23 | 16 |
| 3.3 V and GND |  | 1 or 17, and 20 |

Chip select stays with the spidev driver. Wire CS to CE0. Do not bit-bang it.

The button uses the internal pull-up and is active low. A falling edge inverts the panel.

## Pi setup

Enable SPI, then confirm the device node exists:

```bash
sudo raspi-config nonint do_spi 0
ls /dev/spidev0.0
```

That writes `dtparam=spi=on` to `/boot/firmware/config.txt` (or `/boot/config.txt` on older images). A reboot is required the first time.

The process needs access to the SPI node and the GPIO chip:

```bash
sudo usermod -aG spi,gpio "$USER"
```

Log in again after changing groups.

On Pi 5 the 40-pin header is often `/dev/gpiochip4` rather than `/dev/gpiochip0`. Check `ls /dev/gpiochip*` and pass `--gpiochip` if the default open fails.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

On the Pi the same commands also build `oled_demo`:

```bash
./build/oled_demo
./build/oled_demo --button
./build/oled_demo --gpiochip /dev/gpiochip4 --button 23
```

Without `--button` the program draws a border and a filled block, then exits. `--button` waits for a press on BCM 23 until Ctrl-C. `--no-reset` skips the reset line for modules that do not break it out.
