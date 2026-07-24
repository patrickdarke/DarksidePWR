#!/usr/bin/env python3
# Capture a screenshot from the running panel over USB serial (needs
# pyserial). The firmware's 'S' command dumps the frame as hex; this
# decodes it to a PNG using only the stdlib.
#
#   python3 tools/capture_screenshot.py out.png            # current screen
#   python3 tools/capture_screenshot.py setup.png U 2.0    # open setup first
#
# The port is opened with pyserial DEFAULTS on purpose — deasserting
# DTR/RTS straps the ESP32-S3 into the ROM bootloader (see CLAUDE.md).
import glob
import re
import struct
import sys
import time
import zlib

import serial

HEX_RE = re.compile(rb"^[0-9a-f]+$")


def png_write(path, w, h, rgb_rows):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + row for row in rgb_rows)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def capture(ser, out_path):
    ser.reset_input_buffer()
    ser.write(b"S")
    ser.flush()
    # find begin marker
    w = h = None
    deadline = time.time() + 15
    while time.time() < deadline:
        line = ser.readline().strip()
        m = re.match(rb"\[shot\] begin (\d+)x(\d+) rgb565le", line)
        if m:
            w, h = int(m.group(1)), int(m.group(2))
            break
    if w is None:
        sys.exit("no [shot] begin marker")
    data = bytearray()
    need = w * h * 2
    deadline = time.time() + 60
    while len(data) < need and time.time() < deadline:
        line = ser.readline().strip()
        if line == b"[shot] end":
            break
        if HEX_RE.match(line):
            data.extend(bytes.fromhex(line.decode()))
    if len(data) < need:
        sys.exit(f"short frame: {len(data)}/{need} bytes")

    rows = []
    idx = 0
    for _ in range(h):
        row = bytearray()
        for _ in range(w):
            v = data[idx] | (data[idx + 1] << 8)  # little-endian RGB565
            idx += 2
            row.append(((v >> 11) & 0x1F) << 3 | ((v >> 13) & 0x07))
            row.append(((v >> 5) & 0x3F) << 2 | ((v >> 9) & 0x03))
            row.append((v & 0x1F) << 3 | ((v >> 2) & 0x07))
        rows.append(bytes(row))
    png_write(out_path, w, h, rows)
    print(f"wrote {out_path} ({w}x{h})")


def main():
    out = sys.argv[1]
    pre_cmd = sys.argv[2] if len(sys.argv) > 2 else ""  # e.g. "U" + delay
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports:
        sys.exit("no usbmodem port")
    ser = serial.Serial(ports[0], 115200, timeout=2)
    time.sleep(0.3)
    if pre_cmd:
        ser.write(pre_cmd.encode())
        ser.flush()
        time.sleep(float(sys.argv[3]) if len(sys.argv) > 3 else 1.5)
    capture(ser, out)
    ser.close()


if __name__ == "__main__":
    main()
