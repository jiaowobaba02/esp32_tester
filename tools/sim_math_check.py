#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""数学填空答案质量检查 v4: 打印完整题干/答案/解析, 人工找自相矛盾"""
import sys
sys.path.insert(0, '/media/user/6AF00DA6F00D7A19/esp32-tester/tools')
from sim_quiz2 import simulate

res = simulate('数学', True, 12)
ok = sum(1 for r in res if r[0] == 'FILL_OK')
for r in res:
    print('=' * 20)
    print(r)
print(f"--- 成功率 {ok}/{len(res)} ---")
