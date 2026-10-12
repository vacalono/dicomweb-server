#!/usr/bin/env bash
# W6: Valgrind 内存泄漏检查。
# 启动服务器 -> 触发 STOW/QIDO/WADO -> SIGTERM 干净退出 -> 汇总泄漏报告。
# 用法: bash scripts/valgrind_check.sh [port]
set -uo pipefail

PORT="${1:-8081}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/../build/dicomweb-server"
DATA="$(mktemp -d)"
LOG="$(mktemp)"

if ! command -v valgrind >/dev/null 2>&1; then
  echo "valgrind not found: apt install valgrind"; exit 1
fi
[ -x "$BIN" ] || { echo "build first: cmake --build build -j"; exit 1; }

echo "== starting under valgrind on :$PORT =="
valgrind --leak-check=full --show-leak-kinds=definite,indirect \
  --errors-for-leak-kinds=definite,indirect --error-exitcode=99 \
  --log-file="$LOG" \
  "$BIN" --port "$PORT" --data "$DATA" --io-threads 2 --workers 2 &
VG_PID=$!

# 等待就绪
for _ in $(seq 1 60); do
  if curl -s -o /dev/null "http://127.0.0.1:$PORT/studies"; then break; fi
  sleep 1
done

echo "== exercising: STOW -> QIDO -> WADO -> Range =="
python3 "$HERE/e2e_client.py" "http://127.0.0.1:$PORT" >/dev/null 2>&1 || \
  echo "(e2e client failed; server log below)"
sleep 1

kill -TERM "$VG_PID" 2>/dev/null || true
wait "$VG_PID"; RC=$?

echo "== valgrind summary =="
grep -E "definitely lost|indirectly lost|possibly lost|still reachable|ERROR SUMMARY|All heap blocks" "$LOG" || true
echo "== log: $LOG =="
rm -rf "$DATA"

if [ "$RC" -ne 0 ]; then echo "VALGRIND: FAIL (rc=$RC)"; exit "$RC"; fi
echo "VALGRIND: PASS"
