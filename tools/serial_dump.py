#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""串口启动日志抓取 (验证烧录后设备正常启动)"""
import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
DURATION = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0

s = serial.Serial(PORT, 115200, timeout=2)
s.setDTR(False)
s.setRTS(True)
time.sleep(0.1)
s.setRTS(False)          # 触发复位
s.reset_input_buffer()
t0 = time.time()
while time.time() - t0 < DURATION:
    try:
        line = s.readline().decode(errors="replace").rstrip()
        if line:
            print(line, flush=True)
    except Exception as e:
        print("ERR", e)
        break
