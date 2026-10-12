#!/usr/bin/env python3
"""验证：单连接 pipelining 顺序 + 高并发，以及线程池/同步两种模式。"""
import concurrent.futures as cf
import socket
import sys
import urllib.request

HOST, PORT = "127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8080
MODE = sys.argv[2] if len(sys.argv) > 2 else "?"


def pipelining(n=5):
    """同一连接一次发 n 个请求，响应必须按序且全部成功（线程池下同连接必须串行）。"""
    s = socket.create_connection((HOST, PORT))
    s.settimeout(5)
    s.sendall(b"GET /studies HTTP/1.1\r\nHost: x\r\n\r\n" * n)
    data = b""
    while data.count(b"HTTP/1.1 ") < n:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    s.close()
    return data.count(b"HTTP/1.1 200")


def one(_):
    with urllib.request.urlopen(f"http://{HOST}:{PORT}/studies", timeout=8) as r:
        return r.status


def concurrent(n):
    with cf.ThreadPoolExecutor(n) as ex:
        return list(ex.map(one, range(n)))


print(f"[{MODE}] pipelining 5 reqs on 1 conn -> 200 responses = {pipelining(5)}/5")
if MODE == "stress":
    ok = 0
    for _ in range(5):
        ok += sum(c == 200 for c in concurrent(200))
    print(f"[{MODE}] 5 x 200 concurrent -> 200 = {ok}/1000")
else:
    codes = concurrent(50)
    print(f"[{MODE}] 50 concurrent requests -> 200 = {sum(c == 200 for c in codes)}/50")
