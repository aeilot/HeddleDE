#!/usr/bin/env bash
# 内存压力实验：N 个 agent 在 cgroup 内存上限下，共享 /usr 对照每 agent 一份 /usr。
#   docker run --rm --privileged -v "$PWD":/src -v heddle-usr:/var/tmp/heddle-usr -w /src heddle-apps \
#     bench/pressure.sh APP "LIMITS_MIB" [N] [reps]
# LIMITS 里的 0 表示不限（校准用）。/usr 拷贝放在命名卷里，重跑不必再拷。
set -euo pipefail
cd "$(dirname "$0")/.."
APP=$1 LIMITS=$2 N=${3:-8} REPS=${4:-1}
OUT=${OUT:-runs/pressure.jsonl}
B=/tmp/heddle-build
meson setup "$B" compositor >/dev/null 2>&1 || true
ninja -C "$B" >/dev/null
mkdir -p runs
for rep in $(seq "$REPS"); do
  for lim in $LIMITS; do
    for cfg in shared per-usr; do
      pkill -9 -x 'chromium|soffice.bin|oosplash|gnome-text-edit|dbus-daemon|heddle' 2>/dev/null || true
      sleep 2
      timeout 1200 python3 bench/pressure.py --heddle "$B/heddle" --app "$APP" --config "$cfg" -n "$N" \
        --limit "$lim" --label "rep$rep" --out "$OUT" 2>&1 | grep -v '^  copying' || echo "== failed $APP $cfg $lim" >&2
    done
  done
done
