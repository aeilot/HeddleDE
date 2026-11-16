#!/usr/bin/env bash
# 门槛实验：同一应用在 Heddle always / observe / observe+suspended 下的 CPU、截图延迟、旧画面。
#   docker build -t heddle-apps -f dev/Dockerfile.apps dev
#   docker run --rm --privileged -v "$PWD":/src heddle-apps bench/gate.sh [steps] [wait_s] [reps]
set -euo pipefail
cd "$(dirname "$0")/.."
STEPS=${1:-6} WAIT=${2:-24} REPS=${3:-1}
OUT=${OUT:-runs/gate.jsonl}
B=/tmp/heddle-build
meson setup "$B" compositor >/dev/null 2>&1 || true
ninja -C "$B" >/dev/null
export XDG_RUNTIME_DIR=$(mktemp -d); chmod 700 "$XDG_RUNTIME_DIR"
. bench/gate-apps.sh
mkdir -p runs

run() { # label cmd desktop actions...
  local label=$1 cmd=$2 desk=$3; shift 3
  local acts=(); for x in "$@"; do acts+=(--action "$x"); done
  pkill -9 -f 'chromium|soffice|gnome-text-editor|dbus-daemon' 2>/dev/null || true; sleep 1
  rm -rf /tmp/chromium-profile /root/.config/libreoffice /root/.local/share/org.gnome.TextEditor || true
  echo "== $label $desk $(date +%T)" >&2
  timeout 900 python3 bench/run.py --desktop "$desk" --heddle "$B/heddle" --app "$cmd" --label "$label" \
    --warmup 20 --steps "$STEPS" --wait "$WAIT" --grace 2000 --settle 300 --timeout 5000 \
    --check-stale --out "$OUT" "${acts[@]}" 2>>runs/gate.log || echo "   failed" >&2
}

for rep in $(seq "$REPS"); do
  for desk in heddle-always heddle-observe heddle-observe-suspend; do
    run chromium "$CHROMIUM" "$desk" "type:Travel to Berlin" key:Tab "type:Hotel and train tickets" key:Next key:Prior "type: for May"
    run textedit "$TEXTEDIT" "$desk" "type:Meeting notes for Monday" key:Return "type:Budget review and hiring plan" key:Return key:ctrl+Home "type:Draft: "
    run writer "$WRITER" "$desk" "type:Quarterly report" key:Return "type:Revenue grew in all regions" key:Return key:ctrl+Home "type:Draft: "
  done
done
python3 bench/summarize.py "$OUT"
