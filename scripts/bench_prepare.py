#!/usr/bin/env python3
"""W6: 准备压测数据并上传（生成 N 个小实例 + 1 个 16MB 大实例用于吞吐测试）。"""
import io
import sys
import urllib.request
import uuid

import pydicom
from pydicom.dataset import Dataset, FileMetaDataset
from pydicom.uid import ExplicitVRLittleEndian, generate_uid

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 200
SC = "1.2.840.10008.5.1.4.1.1.7"


def make(pid, rows, cols):
    ds = Dataset()
    ds.SOPClassUID = SC
    ds.SOPInstanceUID = generate_uid()
    ds.PatientName = "Bench^Patient"
    ds.PatientID = pid
    ds.StudyDate = "20260101"
    ds.StudyInstanceUID = generate_uid()
    ds.SeriesInstanceUID = generate_uid()
    ds.Modality = "OT"
    ds.InstanceNumber = 1
    ds.Rows = rows
    ds.Columns = cols
    ds.SamplesPerPixel = 1
    ds.PhotometricInterpretation = "MONOCHROME2"
    ds.BitsAllocated = 8
    ds.BitsStored = 8
    ds.HighBit = 7
    ds.PixelRepresentation = 0
    ds.PixelData = bytes(rows * cols)
    meta = FileMetaDataset()
    meta.MediaStorageSOPClassUID = SC
    meta.MediaStorageSOPInstanceUID = ds.SOPInstanceUID
    meta.TransferSyntaxUID = ExplicitVRLittleEndian
    ds.file_meta = meta
    ds.is_little_endian = True
    ds.is_implicit_VR = False
    buf = io.BytesIO()
    ds.save_as(buf, write_like_original=False)
    return buf.getvalue(), ds.StudyInstanceUID, ds.SeriesInstanceUID, ds.SOPInstanceUID


def stow_many(items):
    b = "B" + uuid.uuid4().hex[:20]
    body = b""
    for data in items:
        body += (f"--{b}\r\nContent-Type: application/dicom\r\n\r\n").encode() + data + b"\r\n"
    body += f"--{b}--\r\n".encode()
    req = urllib.request.Request(
        BASE + "/studies", data=body, method="POST",
        headers={"Content-Type": f'multipart/related; type="application/dicom"; boundary={b}'})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.status


batch = []
n_ok = 0
for i in range(N):
    data, _, _, _ = make(f"BENCH{i:04d}", 64, 64)
    batch.append(data)
    if len(batch) == 50:
        stow_many(batch)
        n_ok += len(batch)
        batch = []
if batch:
    stow_many(batch)
    n_ok += len(batch)
print(f"uploaded {n_ok} small instances")

big, study, series, sop = make("BIG", 4096, 4096)  # ~16MB pixel data
print("STOW big ->", stow_many([big]), f"({len(big)} bytes)")
with open("/tmp/bench_big.txt", "w") as f:
    f.write(f"{study} {series} {sop}\n")
print("big instance:", study, series, sop)
