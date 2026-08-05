#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 main/ds_cert.h: DeepSeek 信任链 (中间 + 根, 不含易轮换的叶子证书) 为 C 字符串.
用法: python3 tools/gen_ds_cert.py   (从 /tmp/ds_chain.pem 读取 3 证书 PEM, 丢弃第 1 个叶子)
"""
import re, sys

src = '/tmp/ds_chain.pem'
if len(sys.argv) > 1:
    src = sys.argv[1]

pem = open(src, encoding='utf-8').read().strip()
pem = pem.replace('\r\n', '\n')
certs = re.findall(r'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', pem, re.S)
assert len(certs) == 3, f"需要 3 个证书 (叶子+中间+根), 实际 {len(certs)}"

# 丢弃叶子 (第 1 个): 叶子每季度轮换, 固定它会导致轮换后 TLS 验证失败;
# 中间 (TrustAsia DV TLS RSA CA 2025, 至 2035) + 根 (DigiCert G2, 至 2038) 长期稳定,
# 且覆盖服务器不下发中间证书的节点 (证书包只有根, 缺中间会验证失败).
chain = '\n'.join(certs[1:]) + '\n'

lines = []
lines.append('#ifndef DS_CERT_H')
lines.append('#define DS_CERT_H')
lines.append('')
lines.append('/* DeepSeek api.deepseek.com 信任链 (中间 + 根):')
lines.append(' * 1. TrustAsia DV TLS RSA CA 2025 (中间, 至 2035-01)')
lines.append(' * 2. DigiCert Global Root G2 (根, 至 2038-01)')
lines.append(' * 策略: 主用 esp_crt_bundle (系统证书包), 仅当其验证失败时回退本链,')
lines.append(' * 覆盖服务器不下发中间证书的节点. 故意不固定叶子证书 (api.deepseek.com')
lines.append(' * 每季度轮换, 固定叶子会在轮换后导致验证失败). 中间/根轮换后需重新导出. */')
lines.append('static const char s_ds_cert[] =')
for line in chain.split('\n'):
    lines.append('    "' + line + '\\n"')
lines.append('    ;')
lines.append('')
lines.append('#endif')

out = '\n'.join(lines) + '\n'
open('/media/user/6AF00DA6F00D7A19/esp32-tester/main/ds_cert.h', 'w', encoding='utf-8', newline='\n').write(out)
print(f"ds_cert.h 生成: {len(chain)} bytes PEM (2 证书, 无叶子)")
