#!/usr/bin/env python3
"""serial_read_win.py - Windows 下读取 ESP32-C5 串口日志（COM12）

用法: python tools/serial_read_win.py [秒数]
"""
import serial
import time
import sys

PORT = "COM12"
BAUD = 115200


def main():
    seconds = 12
    if len(sys.argv) > 1 and sys.argv[1].isdigit():
        seconds = int(sys.argv[1])

    ser = serial.Serial(PORT, BAUD, timeout=1)
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        data = ser.read(4096)
        if data:
            buf += data
        else:
            time.sleep(0.05)
    with open("tools/log_capture.txt", "w", encoding="utf-8") as f:
        f.write(buf.decode("utf-8", errors="replace"))
    ser.close()


if __name__ == "__main__":
    main()
