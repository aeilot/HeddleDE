#!/usr/bin/env bash
# 冒烟测试：在开发容器里运行（docker run --privileged，bwrap 需要用户命名空间）。
#   docker run --rm --privileged -v "$PWD":/src heddle-dev tests/smoke.sh
set -euo pipefail
cd "$(dirname "$0")/.."
B=${BUILD:-/tmp/heddle-build}
meson setup "$B" compositor >/dev/null 2>&1 || true
ninja -C "$B" >/dev/null
export XDG_RUNTIME_DIR=$(mktemp -d); chmod 700 "$XDG_RUNTIME_DIR"
HR=$XDG_RUNTIME_DIR/heddle
export PYTHONPATH=$PWD/control
H="python3 -m heddle --control $HR/control"
fail() { echo "FAIL: $*"; cat "$XDG_RUNTIME_DIR/heddle.log" | tail -30; exit 1; }
pass() { echo "ok - $*"; }

"$B/heddle" -R "$HR" -a a0 -a a1 -s 1024x768 -p observe -g 1000 2>"$XDG_RUNTIME_DIR/heddle.log" &
HP=$!
trap 'kill $HP 2>/dev/null; pkill -f "foot" 2>/dev/null || true' EXIT
for _ in $(seq 50); do [[ -f $HR/ready ]] && break; sleep 0.1; done
[[ -f $HR/ready ]] || fail "heddle did not start"
pass "heddle started with two agents"

# 1. 全局对象过滤：每个 agent 只看到一个 seat、一个输出，且 seat 名是自己的
for a in a0 a1; do
  info=$(WAYLAND_DISPLAY=$HR/$a/wayland-0 wayland-info 2>/dev/null)
  ns=$(grep -c "interface: 'wl_seat'" <<<"$info"); no=$(grep -c "interface: 'wl_output'" <<<"$info")
  [[ $ns == 1 && $no == 1 ]] || fail "$a sees $ns seats / $no outputs"
  grep -q "name: $a" <<<"$info" || fail "$a seat name"
  ! grep -qE "zwlr_screencopy|zwp_virtual_keyboard|zwlr_virtual_pointer|zwlr_data_control" <<<"$info" || fail "$a sees snooping protocols"
done
pass "global filter: one seat + one output per agent, no screencopy/virtual input"

# 2. 隔离启动终端
STATE=$XDG_RUNTIME_DIR/state
for a in a0 a1; do
  scripts/heddle-run --agent $a --state "$STATE" --socket "$HR/$a/wayland-0" -- \
    foot -o "colors.background=101010" sh >"$XDG_RUNTIME_DIR/foot-$a.log" 2>&1 &
done
for _ in $(seq 100); do
  n0=$($H -a a0 windows | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')
  n1=$($H -a a1 windows | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')
  [[ $n0 == 1 && $n1 == 1 ]] && break; sleep 0.1
done
[[ $n0 == 1 && $n1 == 1 ]] || { cat "$XDG_RUNTIME_DIR"/foot-*.log; fail "windows: a0=$n0 a1=$n1"; }
$H -a a0 windows
pass "one foot window per agent (inside bwrap)"

# 3. 输入只进对应 agent：在 a0 里敲命令，文件只出现在 a0 的 /tmp
$H -a a0 type 'echo heddle-$((6*7)) > /tmp/probe; echo $HEDDLE_AGENT >> /tmp/probe' >/dev/null
$H -a a0 key Return >/dev/null
for _ in $(seq 50); do [[ -f $STATE/agents/a0/tmp/probe ]] && break; sleep 0.1; done
[[ "$(cat $STATE/agents/a0/tmp/probe 2>/dev/null)" == $'heddle-42\na0' ]] || fail "typed command did not run in a0"
[[ ! -e $STATE/agents/a1/tmp/probe ]] || fail "a1 got a0's input"
pass "typed text reaches only a0 (shell ran: heddle-42, HEDDLE_AGENT=a0)"

# 4. 截图：稳定、尺寸对、两个 agent 画面不同
$H -a a0 screenshot "$XDG_RUNTIME_DIR/a0.png" | tee /dev/stderr | grep -qE '"status": *"stable"' || fail "a0 screenshot not stable"
$H -a a1 screenshot "$XDG_RUNTIME_DIR/a1.png" >/dev/null || fail "a1 screenshot"
python3 - "$XDG_RUNTIME_DIR/a0.png" "$XDG_RUNTIME_DIR/a1.png" <<'PY' || fail "screenshot content"
import sys
from PIL import Image
a, b = (Image.open(p).convert("RGB") for p in sys.argv[1:3])
assert a.size == (1024, 768), a.size
assert len(set(a.getdata())) > 2, "a0 screenshot is flat"
assert a.tobytes() != b.tobytes(), "a0 and a1 identical"
PY
cp "$XDG_RUNTIME_DIR/a0.png" "${SHOT_OUT:-/tmp}/heddle-smoke-a0.png" 2>/dev/null || true
pass "screenshots stable, 1024x768, differ between agents"

# 5. 按观测出帧：放行期过后不再合成
$H -a a0 gate >/dev/null; sleep 0.3
f1=$($H -a a0 stats --reset | python3 -c 'import json,sys; print(json.load(sys.stdin)["frames_rendered"])')
$H -a a0 type 'x' >/dev/null      # 动作 -> 放行 1 s
sleep 2.5
s=$($H -a a0 stats --reset)
fr=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["frames_rendered"])' "$s")
run=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["running"])' "$s")
[[ $fr -ge 1 && $run == False ]] || fail "after action: frames=$fr running=$run"
sleep 2
fr2=$($H -a a0 stats | python3 -c 'import json,sys; print(json.load(sys.stdin)["frames_rendered"])')
[[ $fr2 == 0 ]] || fail "frames rendered while gated: $fr2"
pass "observe policy: frames during grace ($fr), none while gated"

# 6. pyautogui 替身
$H -a a0 pyautogui 'pyautogui.hotkey("ctrl","shift","c"); pyautogui.press("enter"); pyautogui.click(100, 100)' >/dev/null || fail "pyautogui shim"
pass "pyautogui shim executes"

echo "ALL OK"
