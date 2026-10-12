#!/usr/bin/env bash
# W6: WADO 大文件吞吐 / TTFB / 内存峰值实测（需先跑 bench_prepare.py 生成 /tmp/bench_big.txt）。
# 用法: bash scripts/bench_metrics.sh [port]
set -uo pipefail

PORT="${1:-8080}"
BASE="http://127.0.0.1:${PORT}"

if [ ! -f /tmp/bench_big.txt ]; then echo "run scripts/bench_prepare.py first"; exit 1; fi
read -r S SE I < /tmp/bench_big.txt

echo "== WADO-RS download (16MB instance) =="
for _ in 1 2 3; do
  curl -s -o /dev/null -H "Accept: application/dicom" \
    -w "ttfb=%{time_starttransfer}s  speed=%{speed_download} B/s  size=%{size_download}\n" \
    "$BASE/studies/$S/series/$SE/instances/$I"
done

echo "== memory peak =="
PID="$(pgrep -x dicomweb-server | head -1 || true)"
if [ -n "$PID" ]; then grep -E 'VmRSS|VmHWM' "/proc/$PID/status"; fi

echo "== network ceiling (404, no sqlite) =="
wrk -t4 -c500 -d8s "$BASE/nonexistent" 2>&1 | grep -E 'Requests/sec|Latency'
