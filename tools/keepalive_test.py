#!/usr/bin/env python3
"""复现设备 keepalive 参数 (idle=5s/interval=5s/count=3) 下 DeepSeek 长生成请求的连接行为"""
import socket, ssl, time

body = ('{"model":"deepseek-chat","messages":[{"role":"system","content":"x"},'
        '{"role":"user","content":"qing zhengli PCR yuanli yue 3000 zi"}],'
        '"max_tokens":6144,"temperature":0.4}')
s = socket.create_connection(('api.deepseek.com', 443), timeout=300)
s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 5)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 5)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 3)
ctx = ssl.create_default_context()
tls = ctx.wrap_socket(s, server_hostname='api.deepseek.com')
CR = '\r\n'
req = ('POST /chat/completions HTTP/1.1' + CR + 'Host: api.deepseek.com' + CR +
       'Content-Type: application/json' + CR +
       'Authorization: Bearer sk-aa84f50863bf4f789c28e6d9ae7c5601' + CR +
       'Accept-Encoding: identity' + CR + 'Content-Length: %d' % len(body) +
       CR + 'Connection: keep-alive' + CR + CR + body)
t0 = time.time()
tls.sendall(req.encode())
tls.settimeout(300)
buf = b''
hdr = 0
try:
    while True:
        d = tls.recv(4096)
        if not d:
            print('EOF at %.1fs' % (time.time() - t0))
            break
        buf += d
        if not hdr and b'\r\n\r\n' in buf:
            hdr = 1
            print('header at %.1fs' % (time.time() - t0))
        if hdr and len(buf) > 2000:
            print('data flowing, %d bytes at %.1fs' % (len(buf), time.time() - t0))
        if b'"id"' in buf and buf.endswith(b'}'):
            print('DONE %.1fs %d bytes' % (time.time() - t0, len(buf)))
            break
except Exception as e:
    print('EXC at %.1fs: %s' % (time.time() - t0, e))
tls.close()
