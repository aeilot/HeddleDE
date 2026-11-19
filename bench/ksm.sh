#!/usr/bin/env bash
# 匿名页内容重复统计（KSM 上限）：每个应用 N 个 agent，开/关地址随机化各一次。
#   docker run --rm --privileged -v "$PWD":/src -w /src heddle-apps bench/ksm.sh [N] [reps]
set -euo pipefail
cd "$(dirname "$0")/.."
N=${1:-8} REPS=${2:-1}
OUT=${OUT:-runs/ksm.jsonl}
B=/tmp/heddle-build
meson setup "$B" compositor >/dev/null 2>&1 || true
ninja -C "$B" >/dev/null
mkdir -p runs
for rep in $(seq "$REPS"); do
  for app in textedit writer chromium; do
    for aslr in on off; do
      pkill -9 -x 'chromium|soffice.bin|oosplash|gnome-text-edit|dbus-daemon|heddle' 2>/dev/null || true
      sleep 2
      flag=(); [[ $aslr == off ]] && flag=(--no-aslr)
      timeout 900 python3 bench/ksmscan.py --heddle "$B/heddle" --app "$app" -n "$N" "${flag[@]}" \
        --label "rep$rep" --out "$OUT" || echo "== failed $app aslr=$aslr" >&2
    done
  done
done
