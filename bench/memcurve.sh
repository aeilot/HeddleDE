#!/usr/bin/env bash
# 第 4 步内存曲线：N 个 agent × 应用 × 配置（shared / per-comp / per-usr）。
#   docker build -t heddle-apps -f dev/Dockerfile.apps dev
#   docker run --rm --privileged -v "$PWD":/src -w /src heddle-apps bench/memcurve.sh ["1 2 4 8"] [reps]
# per-usr 需要 N 份 /usr 拷贝（每份约 2 GiB，放在 /var/tmp/heddle-usr）。
set -euo pipefail
cd "$(dirname "$0")/.."
NS=${1:-"1 2 4 8"} REPS=${2:-1}
OUT=${OUT:-runs/memcurve.jsonl}
B=/tmp/heddle-build
meson setup "$B" compositor >/dev/null 2>&1 || true
ninja -C "$B" >/dev/null
mkdir -p runs
for rep in $(seq "$REPS"); do
  for n in $NS; do
    for app in textedit writer chromium; do
      for cfg in shared per-comp per-usr; do
        pkill -9 -f 'chromium|soffice|gnome-text-editor|dbus-daemon|heddle-build/heddle' 2>/dev/null || true
        sleep 2
        echo "== rep$rep n=$n $app $cfg $(date +%T)" >&2
        timeout 900 python3 bench/memcurve.py --heddle "$B/heddle" --app "$app" --config "$cfg" -n "$n" \
          --label "rep$rep" --out "$OUT" 2>>runs/memcurve.log || echo "   failed" >&2
        tail -1 runs/memcurve.log >&2
      done
    done
  done
done
python3 bench/memsummary.py "$OUT"
