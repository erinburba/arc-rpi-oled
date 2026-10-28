#!/usr/bin/env python3
"""Light every pixel, then draw a border and OK, over I2C.

Default bus is /dev/i2c-1 and the SSD1306 address is 0x3D.
Confirm the address with: i2cdetect -y 1
"""

import argparse
import errno
import fcntl
import os
import sys
import time

WIDTH = 128
HEIGHT = 64
I2C_SLAVE = 0x0703
DATA_CHUNK = 31
WHITE_SECONDS = 3

# 5x7 glyphs. Bit 4 is the left column.
GLYPHS = {
    "O": (
        0b01110,
        0b10001,
        0b10001,
        0b10001,
        0b10001,
        0b10001,
        0b01110,
    ),
    "K": (
        0b10001,
        0b10010,
        0b10100,
        0b11000,
        0b10100,
        0b10010,
        0b10001,
    ),
}

INIT = (
    bytes((0xAE,)),
    bytes((0xD5, 0x80)),
    bytes((0xA8, 0x3F)),
    bytes((0xD3, 0x00)),
    bytes((0x40,)),
    bytes((0x8D, 0x14)),
    bytes((0x20, 0x00)),
    bytes((0xA1,)),
    bytes((0xC8,)),
    bytes((0xDA, 0x12)),
    bytes((0x81, 0xCF)),
    bytes((0xD9, 0xF1)),
    bytes((0xDB, 0x40)),
    bytes((0xA4,)),
    bytes((0xA6,)),
    bytes((0xAF,)),
)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bus", type=int, default=1, help="I2C bus number (default 1)")
    parser.add_argument("--address", type=lambda text: int(text, 0), default=0x3D, help="7-bit address (default 0x3d)")
    return parser.parse_args()


def blank():
    return bytearray(WIDTH * HEIGHT // 8)


def set_pixel(buf, x, y):
    index = x + (y // 8) * WIDTH
    buf[index] |= 1 << (y % 8)


def blit(buf, glyph, x, y):
    for row, bits in enumerate(glyph):
        for col in range(5):
            if bits & (1 << (4 - col)):
                set_pixel(buf, x + col, y + row)


def render():
    buf = blank()
    for x in range(WIDTH):
        set_pixel(buf, x, 0)
        set_pixel(buf, x, HEIGHT - 1)
    for y in range(HEIGHT):
        set_pixel(buf, 0, y)
        set_pixel(buf, WIDTH - 1, y)
    text = "OK"
    glyph_width = 5
    gap = 2
    text_width = glyph_width * len(text) + gap * (len(text) - 1)
    x = (WIDTH - text_width) // 2
    y = (HEIGHT - len(GLYPHS["O"])) // 2
    for char in text:
        blit(buf, GLYPHS[char], x, y)
        x += 5 + gap
    return buf


def open_i2c(bus, address):
    if not 0 <= address <= 0x7F:
        raise SystemExit(f"address {address:#x} is outside the 7-bit range")
    path = f"/dev/i2c-{bus}"
    fd = os.open(path, os.O_RDWR)
    try:
        fcntl.ioctl(fd, I2C_SLAVE, address)
    except OSError:
        os.close(fd)
        raise
    print(f"Opened {path} address {address:#04x}")
    return fd


def write_all(fd, payload):
    view = memoryview(payload)
    while view:
        written = os.write(fd, view)
        if written <= 0:
            raise OSError(errno.EIO, "short I2C write")
        view = view[written:]


def command(fd, payload):
    write_all(fd, bytes((0x00,)) + payload)


def data(fd, payload):
    offset = 0
    while offset < len(payload):
        chunk = payload[offset : offset + DATA_CHUNK]
        write_all(fd, bytes((0x40,)) + chunk)
        offset += len(chunk)


def show(fd, frame):
    for init in INIT:
        command(fd, init)
    command(fd, bytes((0xA5,)))
    print(f"Panel should be solid white for {WHITE_SECONDS} seconds.")
    time.sleep(WHITE_SECONDS)
    command(fd, bytes((0xA4,)))
    command(fd, bytes((0x21, 0x00, WIDTH - 1)))
    command(fd, bytes((0x22, 0x00, HEIGHT // 8 - 1)))
    data(fd, frame)


def main():
    args = parse_args()
    frame = render()
    fd = None
    try:
        fd = open_i2c(args.bus, args.address)
        show(fd, frame)
        print("Border and OK are on the display. Press Enter to exit.")
        try:
            input()
        except EOFError:
            pass
    except OSError as exc:
        if exc.errno == errno.ENXIO:
            print(
                f"No device responded at {args.address:#x} on i2c-{args.bus}. Try --address 0x3c.",
                file=sys.stderr,
            )
        else:
            print(
                f"{exc}. Enable I2C and add this user to the i2c group.",
                file=sys.stderr,
            )
        return 1
    finally:
        if fd is not None:
            os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
