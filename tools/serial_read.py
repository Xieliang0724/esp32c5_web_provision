#!/usr/bin/env python3
"""
serial_read.py - 读取 ESP32-C5 串口日志（无需 TTY，可被 DSH 会话直接调用）

用法:
    python3 tools/serial_read.py [秒数] [--reset]

参数:
    秒数      读取时长（默认 10 秒）
    --reset   先复位开发板再读（抓完整启动日志）

示例:
    python3 tools/serial_read.py 5            # 读 5 秒当前日志
    python3 tools/serial_read.py 15 --reset   # 复位并读 15 秒启动日志

注意:
    - 若报 Permission denied: 当前会话沙箱未放行 /dev，需提升沙箱权限
    - 若报 Resource busy: 串口被其他进程占用，用 lsof 查找并关闭
    - 设备空闲时无输出属正常（ESP-IDF 日志是事件驱动的）
"""
import serial
import time
import sys

PORT = "/dev/cu.usbserial-5C310834821"
BAUD = 115200


def main():
    args = sys.argv[1:]
    seconds = 10
    do_reset = False
    for a in args:
        if a == "--reset":
            do_reset = True
        elif a.isdigit():
            seconds = int(a)

    ser = serial.Serial(PORT, BAUD, timeout=1)

    if do_reset:
        # 标准复位序列：拉低 RTS(EN) 再释放
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.15)
        ser.setRTS(False)
        time.sleep(0.15)
        ser.setDTR(True)

    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        data = ser.read(4096)
        if data:
            buf += data
        else:
            time.sleep(0.05)

    sys.stdout.write(buf.decode("utf-8", errors="replace"))
    ser.close()


if __name__ == "__main__":
    main()
