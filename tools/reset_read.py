#!/usr/bin/env python3
"""触发 ESP32 正常启动复位并抓取串口日志 (esptool 标准 hard reset 序列)。"""
import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 12
s = serial.Serial(port, 115200, timeout=0.5)
s.setDTR(False)                # IO0=HIGH (反相), 正常启动
s.setRTS(True)                 # EN=LOW, 芯片复位
time.sleep(0.1)
s.setRTS(False)                # EN=HIGH, 开始启动
time.sleep(0.2)
t0 = time.time(); out = b''
while time.time() - t0 < secs:
    out += s.read(4096)
txt = out.decode('utf-8', 'replace')
print(txt[-4000:])
