#!/usr/bin/env python3
"""调试：探测 Orthanc DICOMweb 的 WADO-RS Accept 要求。"""
import json
import urllib.error
import urllib.request

B = "http://127.0.0.1:8042/dicom-web"


def g(url, headers=None):
    req = urllib.request.Request(url, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


st, body = g(B + "/studies")
studies = json.loads(body)
print("studies:", len(studies))
study = studies[0]["0020000D"]["Value"][0]
series = json.loads(g(B + f"/studies/{study}/series")[1])[0]["0020000E"]["Value"][0]
inst = json.loads(g(B + f"/studies/{study}/series/{series}/instances")[1])[0]["00080018"]["Value"][0]
url = B + f"/studies/{study}/series/{series}/instances/{inst}"
print("url:", url)

for h in ({}, {"Accept": "application/dicom"},
          {"Accept": 'multipart/related; type="application/dicom"'},
          {"Accept": "application/octet-stream"}):
    st, body = g(url, h)
    print(f"Accept={h.get('Accept','(none)'):50s} -> {st}  len={len(body)}  {body[:120]!r}")
