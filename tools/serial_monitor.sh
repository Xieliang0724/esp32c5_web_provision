#!/bin/bash
# 持续抓取串口日志到文件（后台运行）
# 用法: bash tools/serial_monitor.sh [输出文件]
# 默认输出: /tmp/esp32c5_serial.log
# 查看: tail -f /tmp/esp32c5_serial.log
# 停止: pkill -f serial_monitor.sh  或  pkill -f "cat /dev/cu.usbserial-5C310834821"

PORT="/dev/cu.usbserial-5C310834821"
BAUD=115200
OUT="${1:-/tmp/esp32c5_serial.log}"

# 设置波特率（macOS 需要 stty）
stty -f "$PORT" "$BAUD" 2>/dev/null

echo "开始抓取串口日志 -> $OUT (Ctrl+C 停止)"
# 后台持续读串口到文件（追加）
cat "$PORT" >> "$OUT" &
CAT_PID=$!
echo "cat PID: $CAT_PID"
echo "查看日志: tail -f $OUT"

trap 'echo "停止"; kill $CAT_PID 2>/dev/null; exit 0' INT TERM
wait $CAT_PID
