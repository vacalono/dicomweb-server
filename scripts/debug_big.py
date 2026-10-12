#!/usr/bin/env python3
"""调试：不同 body 大小下的 STOW 行为。"""
import sys
import urllib.error
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080"


def try_size(size):
    body = b"--X\r\nContent-Type: application/dicom\r\n\r\n" + b"A" * size + b"\r\n--X--\r\n"
    req = urllib.request.Request(
        BASE + "/studies", data=body, method="POST",
        headers={"Content-Type": "multipart/related; boundary=X"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            print(f"size={size:>10} -> {r.status}  {r.read()[:120]!r}")
    except urllib.error.HTTPError as e:
        print(f"size={size:>10} -> ERR {e.code}  {e.read()[:200]!r}")
    except Exception as e:
        print(f"size={size:>10} -> EXC {type(e).__name__}: {e}")


for s in (1000, 100_000, 1_000_000, 4_000_000, 8_000_000, 16_000_000):
    try_size(s)
