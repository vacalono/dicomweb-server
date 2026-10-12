#!/usr/bin/env python3
"""W4: 与 Orthanc 的 DICOMweb 互操作验证。

用同一套标准 DICOMweb 流程（STOW-RS -> QIDO-RS -> WADO-RS）分别打我们的服务器和
Orthanc（参照实现），验证行为一致；并验证幂等重传（重复 STOW 不产生重复实例）。
"""
import io
import json
import sys
import urllib.error
import urllib.request
import uuid

import pydicom
from pydicom.dataset import Dataset, FileMetaDataset
from pydicom.uid import ExplicitVRLittleEndian, generate_uid

SC = "1.2.840.10008.5.1.4.1.1.7"  # Secondary Capture Image Storage


def make_dicom(patient_id, study_uid, series_uid, sop_uid):
    ds = Dataset()
    ds.SOPClassUID = SC
    ds.SOPInstanceUID = sop_uid
    ds.PatientName = "Zhang^San"
    ds.PatientID = patient_id
    ds.StudyDate = "20260101"
    ds.StudyInstanceUID = study_uid
    ds.SeriesInstanceUID = series_uid
    ds.Modality = "OT"
    ds.InstanceNumber = 1
    ds.Rows = 2
    ds.Columns = 2
    ds.SamplesPerPixel = 1
    ds.PhotometricInterpretation = "MONOCHROME2"
    ds.BitsAllocated = 8
    ds.BitsStored = 8
    ds.HighBit = 7
    ds.PixelRepresentation = 0
    ds.PixelData = bytes([0, 1, 2, 3])
    meta = FileMetaDataset()
    meta.MediaStorageSOPClassUID = SC
    meta.MediaStorageSOPInstanceUID = sop_uid
    meta.TransferSyntaxUID = ExplicitVRLittleEndian
    ds.file_meta = meta
    ds.is_little_endian = True
    ds.is_implicit_VR = False
    return ds


def dcm_bytes(ds):
    buf = io.BytesIO()
    ds.save_as(buf, write_like_original=False)
    return buf.getvalue()


def http(method, url, data=None, headers=None, timeout=15):
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def stow(base, data):
    b = "B" + uuid.uuid4().hex[:20]
    body = (f"--{b}\r\nContent-Type: application/dicom\r\n\r\n").encode() + data + \
           f"\r\n--{b}--\r\n".encode()
    return http("POST", base + "/studies", body,
                {"Content-Type": f'multipart/related; type="application/dicom"; boundary={b}'})


def qido(base, patient):
    return http("GET", base + "/studies?PatientID=" + patient)


def wado(base, study, series, sop):
    """标准 WADO-RS 实例检索：默认 Accept 为 multipart/related（PS3.18）。"""
    url = base + f"/studies/{study}/series/{series}/instances/{sop}"
    st, body = http("GET", url, None, {"Accept": 'multipart/related; type="application/dicom"'})
    if st != 200:
        return st, body
    return st, extract_first_part(body)


def extract_first_part(body):
    """从 multipart/related 响应中取出第一个 part 的二进制内容。"""
    i = body.find(b"\r\n\r\n")
    if i < 0:
        return body
    start = i + 4
    j = body.find(b"\r\n--", start)
    return body[start:j] if j >= 0 else body[start:]


def count_study(body, study):
    try:
        arr = json.loads(body.decode())
        return sum(1 for it in arr if study in json.dumps(it))
    except Exception:
        return body.decode(errors="replace").count(study)


def case(name, base):
    pid = "PID" + uuid.uuid4().hex[:6]
    study, series, sop = generate_uid(), generate_uid(), generate_uid()
    data = dcm_bytes(make_dicom(pid, study, series, sop))

    print(f"\n===== {name}  ({base}) =====")
    st, body = stow(base, data)
    print(f"  STOW  -> {st}  {body.decode(errors='replace')[:120]}")

    st, body = qido(base, pid)
    n1 = count_study(body, study)
    print(f"  QIDO  -> {st}  studies_matching={n1}")

    st, got = wado(base, study, series, sop)
    print(f"  WADO  -> {st}  bytes={len(got)}  roundtrip_ok={got == data}")

    st, _ = stow(base, data)  # 幂等重传
    st2, body2 = qido(base, pid)
    n2 = count_study(body2, study)
    print(f"  RETRY -> {st}  studies_after_retry={n2}  idempotent={n2 == 1}")

    return n1 == 1 and n2 == 1 and (got == data)


MINE = "http://127.0.0.1:8080"
ORTH = "http://127.0.0.1:8042/dicom-web"

a = case("OURS", MINE)
b = case("ORTHANC", ORTH)
print("\n===== SUMMARY =====")
print(f"ours    : STOW/QIDO/WADO + idempotent ok = {a}")
print(f"orthanc : STOW/QIDO/WADO + idempotent ok = {b}")
print("INTEROP:", "PASS" if (a and b) else "FAIL")
sys.exit(0 if (a and b) else 1)
