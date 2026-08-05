#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CH340 恢复尝试: 多次 EN 脉冲 + 读取, 看芯片是否响应"""
import serial, time, sys

s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.3)
for i in range(4):
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.3)
    s.setRTS(False)
    time.sleep(0.8)
    d = s.read(4096)
    print('try', i, 'got', len(d), 'bytes')
    if d:
        print(d.decode('utf-8', 'replace')[:300])
        break
    time.sleep(0.5)
s.close()
