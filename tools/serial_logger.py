#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""串口日志记录: 带时间戳写入文件 + 实时打印, 断线自动重连.
用法: python3 tools/serial_logger.py [秒数] [输出文件] [reset=1]"""
import serial, time, sys

dur = int(sys.argv[1]) if len(sys.argv) > 1 else 300
out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/serial_capture.log"
do_reset = len(sys.argv) > 3 and sys.argv[3] == "reset=1"

if do_reset:
    try:
        s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.3)
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.25)
        s.setRTS(False)
        s.close()
        print("reset pulse sent")
    except Exception as e:
        print("reset failed:", e)
    time.sleep(0.5)

t0 = time.time()
f = open(out, 'ab', buffering=0)
while time.time() - t0 < dur:
    try:
        ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.5)
        # 关键: pyserial 默认 DTR/RTS 电平经 CH340 反相会拉低 EN (设备复位黑屏)!
        # 打开后必须立即设为安全电平 (False=False), 否则设备一直被按着复位
        ser.setDTR(False)
        ser.setRTS(False)
    except Exception:
        time.sleep(1)
        continue
    try:
        while time.time() - t0 < dur:
            d = ser.read(4096)
            if d:
                f.write(("[%06.1f] " % (time.time() - t0)).encode())
                f.write(d)
                sys.stdout.write(d.decode('utf-8', 'replace'))
                sys.stdout.flush()
    except Exception:
        pass
    finally:
        try:
            ser.close()
        except Exception:
            pass
    time.sleep(1)
f.close()
print("\n=== capture saved to %s ===" % out)
