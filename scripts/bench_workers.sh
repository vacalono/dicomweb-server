#!/usr/bin/env bash
# W6: worker 数 -> QPS 调优曲线（SQLite 并发读的拐点）。
# 用法: bash scripts/bench_workers.sh [port]
set -uo pipefail

PORT="${1:-8080}"
URL="http://127.0.0.1:${PORT}/studies?limit=100"
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/../build/dicomweb-server"
DATA=/tmp/bench

ulimit -n 65535 2>/dev/null || true

for W in 1 2 4 8; do
  pkill -x dicomweb-server 2>/dev/null; sleep 1
  setsid "$BIN" --port "$PORT" --io-threads 4 --workers "$W" --data "$DATA" >/dev/null 2>&1 </dev/null &
  sleep 1
  printf 'workers=%s  ' "$W"
  wrk -t4 -c200 -d8s "$URL" 2>&1 | grep -E 'Requests/sec' || true
done
pkill -x dicomweb-server 2>/dev/null
echo done
