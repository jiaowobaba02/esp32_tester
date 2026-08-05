#!/usr/bin/env python3
"""不经过 pyserial 的串口读取: stty 已设好电平, raw fd 读取不碰 DTR/RTS,
不会复位设备 (黑屏). 用法: raw_serial_read.py [秒数]"""
import os, select, time, sys

dur = int(sys.argv[1]) if len(sys.argv) > 1 else 10
f = os.open('/dev/ttyUSB0', os.O_RDONLY | os.O_NONBLOCK)
end = time.time() + dur
buf = b''
while time.time() < end:
    r, _, _ = select.select([f], [], [], 0.5)
    if r:
        try:
            d = os.read(f, 4096)
        except BlockingIOError:
            continue
        if d:
            buf += d
os.close(f)
t = buf.decode('utf-8', 'replace')
print('bytes:', len(buf))
print('REBOOTED' if 'quiz app start' in t else 'no reboot - SAFE')
print(t[-1200:] if t else '(no output)')
