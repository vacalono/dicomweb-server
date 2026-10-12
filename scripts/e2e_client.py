#!/usr/bin/env python3
"""DICOMweb 端到端验证：STOW 上传 -> QIDO 检索 -> WADO 下载（含 Range）。"""
import re
import sys
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080"


def post_multipart(url, dicom):
    b = "BOUND"
    body = (f"--{b}\r\nContent-Type: application/dicom\r\n\r\n").encode() + dicom + \
           f"\r\n--{b}--\r\n".encode()
    req = urllib.request.Request(
        url, data=body,
        headers={"Content-Type": f'multipart/related; type="application/dicom"; boundary={b}'})
    return urllib.request.urlopen(req).read().decode()


def get(url, headers=None):
    req = urllib.request.Request(url, headers=headers or {})
    r = urllib.request.urlopen(req)
    return r.status, dict(r.headers), r.read()


print("--- 1. QIDO before upload ---")
_, _, body = get(BASE + "/studies")
print(body.decode())

dicom = b"\x00" * 128 + b"DICM" + b"FAKEPIXEL" * 100
print("--- 2. STOW upload (%d bytes) ---" % len(dicom))
print(post_multipart(BASE + "/studies", dicom))

print("--- 3. QIDO studies ---")
_, _, studies = get(BASE + "/studies")
studies = studies.decode()
print(studies)
study = re.search(r'"0020000D".*?"Value":\["([^"]+)"\]', studies).group(1)
print("study uid =", study)

print("--- 4. QIDO series ---")
_, _, series_json = get(BASE + f"/studies/{study}/series")
series_json = series_json.decode()
print(series_json)
series = re.search(r'"0020000E".*?"Value":\["([^"]+)"\]', series_json).group(1)

print("--- 5. QIDO instances ---")
_, _, inst_json = get(BASE + f"/studies/{study}/series/{series}/instances")
inst_json = inst_json.decode()
print(inst_json)
inst = re.search(r'"00080018".*?"Value":\["([^"]+)"\]', inst_json).group(1)

print("--- 6. WADO single instance (Accept: application/dicom) ---")
status, headers, data = get(BASE + f"/studies/{study}/series/{series}/instances/{inst}",
                            {"Accept": "application/dicom"})
print("status=%d bytes=%d roundtrip_ok=%s" % (status, len(data), data == dicom))

print("--- 7. WADO series (multipart/related) ---")
status, headers, mp = get(BASE + f"/studies/{study}/series/{series}")
print("status=%d bytes=%d content_type=%s" % (status, len(mp), headers.get("Content-Type")))
print("boundary_in_body=%s" % (b"--" in mp))

print("--- 8. WADO Range bytes=0-99 ---")
status, headers, part = get(BASE + f"/studies/{study}/series/{series}/instances/{inst}",
                            {"Range": "bytes=0-99", "Accept": "application/dicom"})
print("status=%d len=%d content_range=%s" % (status, len(part), headers.get("Content-Range")))
