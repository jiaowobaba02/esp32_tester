#!/usr/bin/env python3
"""验证: 1) stream=true 下 DeepSeek 首字节/数据节奏  2) s_ds_cert 链能否验证当前服务器"""
import socket, ssl, time, re, subprocess

# --- 1) 流式请求节奏 ---
body = ('{"model":"deepseek-chat","stream":true,"messages":[{"role":"system","content":"x"},'
        '{"role":"user","content":"qing zhengli PCR yuanli yue 3000 zi"}],'
        '"max_tokens":6144,"temperature":0.4}')
s = socket.create_connection(('api.deepseek.com', 443), timeout=300)
s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
ctx = ssl.create_default_context()
tls = ctx.wrap_socket(s, server_hostname='api.deepseek.com')
CR = '\r\n'
req = ('POST /chat/completions HTTP/1.1' + CR + 'Host: api.deepseek.com' + CR +
       'Content-Type: application/json' + CR +
       'Authorization: Bearer sk-aa84f50863bf4f789c28e6d9ae7c5601' + CR +
       'Content-Length: %d' % len(body) + CR + 'Connection: keep-alive' + CR + CR + body)
t0 = time.time()
tls.sendall(req.encode())
tls.settimeout(300)
buf = b''
chunks = 0
while True:
    try:
        d = tls.recv(4096)
    except socket.timeout:
        print('stream: recv timeout at %.1fs' % (time.time() - t0))
        break
    if not d:
        print('stream: EOF at %.1fs' % (time.time() - t0))
        break
    buf += d
    chunks += 1
    if chunks <= 5:
        print('stream: chunk%d at %.1fs (%d bytes)' % (chunks, time.time() - t0, len(d)))
    if b'data: [DONE]' in buf:
        print('stream: DONE at %.1fs, %d bytes, %d chunks' % (time.time() - t0, len(buf), chunks))
        break
tls.close()

# --- 2) s_ds_cert 作为 CA 验证当前服务器链 ---
cert = subprocess.run(
    ['openssl', 's_client', '-connect', 'api.deepseek.com:443', '-servername',
     'api.deepseek.com', '-showcerts'],
    input=b'', capture_output=True, timeout=30).stdout
chain = re.findall(rb'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', cert, re.S)
open('/tmp/leaf.pem', 'wb').write(chain[0])
leaf = subprocess.run(['openssl', 'x509', '-in', '/tmp/leaf.pem', '-noout', '-subject', '-issuer'],
                      capture_output=True).stdout.decode()
print('leaf:', leaf.strip())
hdr = open('/media/user/6AF00DA6F00D7A19/esp32-tester/main/ds_cert.h').read()
pems = re.findall(r'"(-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----)(?:\\n)?"', hdr, re.S)
pems = [p.replace('\\n', '\n') for p in pems]
open('/tmp/ds_cert.pem', 'w').write('\n'.join(pems))
print('ds_cert.h contains %d certs' % len(pems))
verify = subprocess.run(
    ['openssl', 'verify', '-CAfile', '/tmp/ds_cert.pem', '/tmp/leaf.pem'],
    capture_output=True)
print('verify with ds_cert.pem:', (verify.stdout + verify.stderr).decode().strip()[:200])
