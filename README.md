# arc-rpi-oled

C++ userspace driver for an SSD1306 128x64 OLED on a Raspberry Pi. This board is driven over I2C at address `0x3D` on `/dev/i2c-1`. The panel framebuffer builds and tests on macOS; the demo binary is built on the Pi.

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

Log in again after changing groups. `0x3d` in the detect grid is this panel. Some modules use `0x3c`.

## SPI wiring

| Signal | BCM | Header pin |
| --- | --- | --- |
| MOSI | 10 | 19 |
| SCLK | 11 | 23 |
| CS (CE0) | 8 | 24 |
| D/C | 24 | 18 |
| RESET | 25 | 22 |
| Button A (optional, to GND) | 23 | 16 |
| Button B (optional, to GND) | 22 | 15 |
| 3.3 V and GND |  | 1 or 17, and 20 |

Chip select stays with the spidev driver. Wire CS to CE0. Enable it with `sudo raspi-config nonint do_spi 0` and confirm `/dev/spidev0.0` exists after reboot.

The optional buttons are switches to ground. `Button` enables the internal pull-up on each line and treats a low line as pressed. Presses and releases are separate events, accepted after 25 ms, so contact bounce does not repeat them. A press on BCM 23 inverts the panel. A press on BCM 22 toggles the filled block. On Pi 5 the header is often `/dev/gpiochip4`. The buttons need the `gpio` group.

## Build

The WebSocket client links OpenSSL. On the Pi:

```bash
sudo apt install libssl-dev
```

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

On the Pi the same commands also build `oled_demo`:

```bash
./build/oled_demo
./build/oled_demo --address 0x3c
./build/oled_demo --buttons
./build/oled_demo --buttons 23 22
```

Without `--buttons` or `--websocket` the program draws a border and a filled block, then exits. `--buttons` waits for the two switches until Ctrl-C.

## WebSocket

`WebSocketClient` connects to a `ws://` or `wss://` server and returns each text or binary message. Ping frames are answered with pong, and a close from the peer ends the receive loop. `wss://` checks the server certificate against the system trust store. `--ca` adds a PEM trust anchor for a private certificate.

```bash
./build/oled_demo --websocket ws://127.0.0.1:8765/oled
./build/oled_demo --websocket wss://panel.example/oled
./build/oled_demo --websocket wss://panel.example/oled --ca panel.pem
```

Each text message is printed. The panel inverts when a message arrives, until Ctrl-C.

## Hardware check

`scripts/oled_check.py` talks to the panel over I2C. It holds a solid white screen for 3 seconds, then draws a border and the letters OK.

```bash
python3 scripts/oled_check.py
python3 scripts/oled_check.py --address 0x3c
```
