#!/usr/bin/env bash
# 在 AMD(gfx1100) 机器上跑一次"建索引 SQL → 首查"，并输出阶段耗时占比。
#
# 用法：
#   OB_BRIDGE=/work/bridge/libseekdb_cuvs_bridge.so \
#   OB_BUILD_DIR=build_release \
#   ./docs/gpu-vector-index-hipvs-cuvs/validation/run_phase_timing.sh \
#       --observer build_release/observer/seekdb \
#       --base-dir /tmp/phase-timing \
#       --port 2981 \
#       -- sql-file.sql
#
# 说明：
#   * 只负责启动 observer、打开 OB_CUVS_PHASE_TIMING、执行给定 SQL、抓取阶段日志；
#   * 不改变任何建图参数，也不参与评分；
#   * 结束后打印 gpu_build / snapshot_serialize / snapshot_load 的毫秒数与占比。
set -u

OBSERVER=""
BASE_DIR="/tmp/seekdb-phase-timing"
PORT="2981"
SQL_FILE=""
BRIDGE="${OB_BRIDGE:-/work/bridge/libseekdb_cuvs_bridge.so}"

while [ $# -gt 0 ]; do
  case "$1" in
    --observer) OBSERVER="$2"; shift 2 ;;
    --base-dir) BASE_DIR="$2"; shift 2 ;;
    --port)     PORT="$2"; shift 2 ;;
    --bridge)   BRIDGE="$2"; shift 2 ;;
    --)         shift; SQL_FILE="${1:-}"; shift || true ;;
    *)          echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ -z "$OBSERVER" ] || [ ! -x "$OBSERVER" ]; then
  echo "usage: $0 --observer <seekdb binary> [--base-dir DIR] [--port PORT] -- <sql file>" >&2
  exit 2
fi
if [ -z "$SQL_FILE" ] || [ ! -f "$SQL_FILE" ]; then
  echo "missing SQL file (pass it after --)" >&2
  exit 2
fi

mkdir -p "$BASE_DIR/data" "$BASE_DIR/log"
LOG="$BASE_DIR/log/observer.log"

echo "== 启动 observer =="
OB_CUVS_PHASE_TIMING=1 \
LD_LIBRARY_PATH="$(dirname "$BRIDGE"):/opt/hipvs/lib:/opt/rocm/lib:${LD_LIBRARY_PATH:-}" \
  "$OBSERVER" \
    --base-dir "$BASE_DIR/data" \
    --log-dir "$BASE_DIR/log" \
    --port "$PORT" \
    --data-dir "$BASE_DIR/data" \
    > "$BASE_DIR/log/stdout.log" 2>&1 &
OBSERVER_PID=$!

cleanup() {
  kill "$OBSERVER_PID" 2>/dev/null || true
  wait "$OBSERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# 等待端口就绪（最多 120 秒）
for _ in $(seq 1 120); do
  if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then break; fi
  sleep 1
done

echo "== 执行建索引 SQL（计时）=="
START_MS=$(date +%s%3N)
# 这里调用调用方自己的 SQL 客户端；没有则退化为 mysql 客户端。
if command -v mysql >/dev/null 2>&1; then
  mysql -h 127.0.0.1 -P "$PORT" -uroot < "$SQL_FILE"
else
  echo "未找到 mysql 客户端，请把执行 SQL 的命令补在此脚本中" >&2
  exit 3
fi
END_MS=$(date +%s%3N)

TOTAL_MS=$((END_MS - START_MS))

echo
echo "== 阶段耗时 =="
python3 - "$LOG" "$TOTAL_MS" <<'PY'
import re
import sys
from collections import defaultdict

log_path, total_ms = sys.argv[1], float(sys.argv[2])
pattern = re.compile(r'stage="([^"]+)"\s+ms=([0-9.]+)')
sums = defaultdict(float)
counts = defaultdict(int)
with open(log_path, errors="replace") as handle:
    for line in handle:
        if "[OBVSAG][cuVS][phase]" not in line:
            continue
        match = pattern.search(line)
        if not match:
            continue
        sums[match.group(1)] += float(match.group(2))
        counts[match.group(1)] += 1

if not sums:
    print("没有抓到阶段日志：确认构建时加了 -DOB_BUILD_CUVS=ON，且运行时设了 OB_CUVS_PHASE_TIMING=1")
    sys.exit(0)

print(f"SQL 总耗时(含客户端): {total_ms:.1f} ms")
attributed = 0.0
for name in sorted(sums, key=lambda key: -sums[key]):
    ms = sums[name]
    attributed += ms
    share = 100.0 * ms / total_ms if total_ms > 0 else 0.0
    print(f"  {name:<20} {ms:10.1f} ms  {share:5.1f}%  ({counts[name]} 次)")
print(f"  {'未归因(扫描/准备/其它)':<18} {total_ms - attributed:10.1f} ms  "
      f"{100.0 * (total_ms - attributed) / total_ms if total_ms > 0 else 0:5.1f}%")
PY

echo
echo "日志：$LOG"
