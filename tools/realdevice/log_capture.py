#!/usr/bin/env python3
# tools/realdevice/log_capture.py —— 串口日志抓取 + 关键字高亮。
# 用法:
#   python3 tools/realdevice/log_capture.py -p /dev/tty.usbmodemXXXX   # 直接打印
#   python3 tools/realdevice/log_capture.py -p PORT -o /tmp/boot.log   # 落盘
# 典型场景:设备按复位后抓全程,操作复现(如首次进店 pair code 显示 "-"),
# 之后 Ctrl+C 停,用 grep 过滤 install/pair/resume 等关键行。
import argparse
import sys
import time

import serial

KEY_LINES = (
    "install", "pair", "httpd", "ONLINE", "resume", "token",
    "recovery", "recover", "nvs", "NVS", "install service",
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", required=True, help="串口设备路径")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-o", "--output", default=None,
                    help="同时写入日志文件")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, baudrate=args.baud, timeout=0.2)
        print(f"# 已打开 {args.port} @ {args.baud}", flush=True)
    except (OSError, serial.SerialException):
        print("# 打不开串口。先断开 Chrome 的 Web Serial 页面/工具占用,"
              "或检查 -p 路径", file=sys.stderr)
        sys.exit(1)

    fout = open(args.output, "w", buffering=1) if args.output else None
    buf = b""
    try:
        while True:
            chunk = ser.read(ser.in_waiting or 1)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip()
                if fout:
                    fout.write(text + "\n")
                mark = ">>>" if any(k in text for k in KEY_LINES) else "   "
                print(f"{mark} {text}", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        if fout:
            fout.close()


if __name__ == "__main__":
    main()
