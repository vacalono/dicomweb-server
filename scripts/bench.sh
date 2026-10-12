#!/usr/bin/env bash
# W5 压测脚本：QPS / P99 / TTFB / 内存 数据采集（用于替换简历占位符）
set -euo pipefail

BASE="${1:-http://127.0.0.1:8080}"
DURATION="${2:-30s}"
CONNS="${3:-1000}"
OUT="$(dirname "$0")/../docs/bench_raw_$(date +%Y%m%d_%H%M%S).txt"

echo "== target: $BASE  duration: $DURATION  connections: $CONNS ==" | tee "$OUT"

# 0) 前置：是否有 wrk
if ! command -v wrk >/dev/null 2>&1; then
  echo "!! wrk not found. install: apt install wrk / brew install wrk" | tee -a "$OUT"
fi

# 1) 元数据检索（小响应）—— 对应简历 "QPS / P99"
echo -e "\n## QIDO (small responses)" | tee -a "$OUT"
wrk -t4 -c"$CONNS" -d"$DURATION" --latency "$BASE/studies?limit=100" | tee -a "$OUT"

# 2) 大文件下载 TTFB / 吞吐 —— 对应简历 "TTFB / MB/s"
echo -e "\n## WADO (large file)" | tee -a "$OUT"
if [ -n "${SERIES_URL:-}" ]; then
  curl -o /dev/null -s -w "ttfb=%{time_starttransfer}s  speed=%{speed_download}B/s  size=%{size_download}\n" \
    "$SERIES_URL" | tee -a "$OUT"
else
  echo "(skip: set SERIES_URL=http://127.0.0.1:8080/studies/<uid>/series/<uid>)" | tee -a "$OUT"
fi

# 3) 内存峰值（压测同时另开终端观察）
echo -e "\n## memory (run during load)" | tee -a "$OUT"
PID="$(pgrep -f dicomweb-server | head -n1 || true)"
if [ -n "$PID" ]; then
  grep -E 'VmRSS|VmHWM' "/proc/$PID/status" | tee -a "$OUT"
else
  echo "(skip: server not running)" | tee -a "$OUT"
fi

echo -e "\n== raw log: $OUT =="
echo "下一步：把实测值填入 docs/指标记录表.md，并替换简历中的预测值。"
