# Heddle

Heddle is a headless Wayland desktop for agents. The name comes from the heddles on a loom, which separate the warp threads into groups and lift them on demand. Here, each agent is isolated, and its desktop is only lifted up and rendered when it is observed.

It is the companion prototype to the [SandWeaver](https://github.com/aeilot/SandWeaver) research. The design rationale, isolation approach and research method are in [docs](docs/README.md) (in Chinese).

## Status

The prototype code is written. It has been smoke-tested only in the dev container (Debian trixie, arm64, wlroots 0.18) with the foot terminal, plus one gate experiment with three desktop apps (see below). No measurements on OSWorld tasks have been made yet. The plan below is still a design proposal. Items marked "unverified" come from prior knowledge and have not yet been checked against upstream documentation or measured.

| Directory | Contents | Plan step |
|---|---|---|
| [compositor](compositor) | Headless compositor: per-agent sockets, outputs and seats, global filtering, control socket, `always`/`observe` frame policies | Steps 2, 3 |
| [control](control) | Python control side: client library, CLI, pyautogui shim, OSWorld-style HTTP server, window coordinate translation | Step 0 |
| [scripts/heddle-run](scripts/heddle-run) | bubblewrap launcher: per-agent HOME, XDG dirs, runtime dir, /tmp and D-Bus; PID/IPC/UTS namespaces; network off by default, optional pasta | Step 4 |
| [bench](bench) | Measurement scripts: app probe (pure Wayland or Xwayland), an "action → wait → screenshot" loop collecting per-process CPU, memory, screenshot latency, stale frames and frames per step; desktops under test can be sway, sway with output power-off, or Heddle with each policy | Step 1, gate, step 3 |
| [tests/smoke.sh](tests/smoke.sh) | Smoke tests | |
| [dev](dev) | Dev container | |

Verified by the smoke tests (2026-11-16, in the container):

- Two agents each see exactly one seat and one output, and see no screencopy, virtual input or data-control globals.
- Two foot instances are launched through heddle-run, each in its own bwrap; a command typed into a0 only takes effect in a0's /tmp.
- Screenshots are judged stable, have the right size, and differ between the two agents.
- Under the observe policy, frames are composited during the grace period after an action and stop after it.
- The pyautogui shim can run `hotkey`, `press` and `click`.

Usage:

```bash
docker build -t heddle-dev dev
```

```bash
docker run --rm --privileged -v "$PWD":/src heddle-dev tests/smoke.sh
```

```bash
python3 bench/run.py --desktop heddle-observe --heddle build/heddle --app 'foot sh' --steps 5 --wait 24 --action 'type:ls' --action key:Return --check-stale
```

```bash
python3 bench/summarize.py runs/results.jsonl
```

## Goals

- Headless: no GPU or display (DRM/KMS) and no real input devices (libinput); frames are composited in memory only.
- Per-agent screenshots, input and seats: each agent has its own focus, pointer, clipboard and output.
- Frames on observation: apps are normally not allowed to redraw; frames are released only when an agent asks for a screenshot, and the screenshot is taken once the picture is stable. See "Frame-on-observation policy".
- App isolation: each agent's apps run in bubblewrap namespaces with their own HOME, XDG dirs, `XDG_RUNTIME_DIR`, D-Bus session bus, accessibility bus, PIDs and network.
- Semantic interface: keep the AT-SPI widget tree, and have the compositor export window positions for coordinate translation.
- Memory sharing: all agents share a read-only `/usr`, so file pages are stored once.

Out of scope: panels, desktop icons, notifications, power management, screensavers. Whether X11 apps are supported is under "Open".

## Plan

The research method is to first port existing approaches as they are, measure the gain, and then find the new problems the port introduces. Every step records both the gains (memory, CPU, latency) and the harm (hung apps, stale frames, failed tasks).

### Step 0: Workload and control

Before step 1, decide what drives the workload and how to judge correctness.

- The public OSWorld trajectories were recorded on GNOME at 1080p. On a different desktop the click coordinates are off, so they cannot be replayed directly. Following the fidelity ladder in `docs/research.md`, there are three levels: VM, the same GNOME userland in a container, and a minimal desktop. The minimal-desktop level needs an online model or widget-locating scripts to drive it.
- The control server inside the OSWorld VM relies on pyautogui and X11 screenshots (unverified) and has to be rewritten for Wayland: screenshots via screencopy, input via the virtual keyboard and virtual pointer protocols.
- Under Wayland, AT-SPI only gives window-relative coordinates (unverified). Agents that click by widget-tree coordinates need window positions to translate them.
- Correctness is judged by the OSWorld evaluators.

### Step 1: Baseline

Run the OSWorld apps (Chrome, LibreOffice, GIMP, VS Code, Thunderbird, VLC) in the headless mode of an unmodified off-the-shelf compositor.

- Compositor: sway. It is tiling, unlike GNOME's stacking windows; labwc is stacking and also wlroots-based, as a fallback.
- Rendering is fixed to pixman plus client-side software rendering so the numbers are comparable.
- Measure: which apps run on pure Wayland and which need Xwayland; memory per instance; per-process CPU aftermath after an action.

### Gate: confirm frames on observation are worth it

The estimate in `docs/research.md` is that memory becomes the density bottleneck before CPU, while frames on observation save CPU. Before writing a compositor, run a cheap check with off-the-shelf sway:

- Power off the output between observations (`output <name> power off`) and power it on before a screenshot. Expect no frame callbacks while it is off (unverified).
- Record the CPU saved, apps that hang, and screenshot latency.
- If the aftermath is mostly app logic and rendering is a small share, step 3 becomes optional and the focus moves to memory.
- Observed (2026-11-16, dev container, sway 1.10.1, wlroots headless backend, pixman): after `power off` then `power on`, sway logs `Page-flip failed on output HEADLESS-1`, and grim screenshots never return. Running foot for 2 steps, both screenshots timed out. So the gate cannot be run with sway as originally planned: either find and work around this (another renderer, another sway version, or forcing damage after the output is powered on), or compare Heddle's own `always` and `observe` directly.
- Done (2026-11-16, Heddle `always` vs `observe`, details in [docs/gate.md](docs/gate.md)): Chromium's aftermath is unrelated to frames. Under observe, composited frames drop from 42 to 16 while CPU stays at about 0.02 cores. The aftermath of Text Editor and Writer is mainly cursor blinking; observe saves 70–84%, but at most about 0.03 cores per app. No apps hung and no stale frames were seen. Tentative conclusion: step 3 becomes optional and the focus moves to memory. The workload was light and VS Code was not tested; this needs to be rechecked on x86 with VS Code and tasks that load pages.

### Step 2: Custom headless compositor

- Provide a control interface for per-agent screenshots and input injection. Input goes from the control interface straight into the right seat, not through the virtual keyboard protocol.
- One Wayland socket per agent. The compositor filters globals per client, so an app only sees its own agent's seat and output. This sidesteps incomplete multi-seat support in toolkits.
- Count how many frames each client commits per step.
- Starting point: written from scratch following the structure of tinywl; see "Decided".

### Step 3: Frames on observation

Add the policy from the next section, and measure the CPU saved, apps that hang, how often a screenshot catches a stale frame, and screenshot latency.

### Step 4: Multiple agents and isolation

- Multiple seats plus bubblewrap isolation, running several VS Code instances and other apps at once, and measuring the memory curve.
- Control group: one separate minimal compositor per agent. `docs/research.md` estimates a shared compositor saves only tens of MiB; this control shows whether sharing is worth it.
- Network: after isolating the network namespace there is no connectivity by default; use pasta or slirp4netns for egress and add an egress policy.
- Local capacity: WSL currently has 7.9 GB of memory. At 0.5–1 GiB per VS Code that fits only 6–8 instances, so `.wslconfig` needs to be raised first.

## Frame-on-observation policy

Sending a single frame callback and compositing once is not enough. The reasons and what to do about each:

- Menu animations and page loads need many consecutive frames. On observation, start a frame clock and run it until no new damage arrives for a while, or until a timeout; the screenshot reply says whether it was stable or timed out. Cursor blinking would keep the picture from ever settling, so set a threshold on damage area.
- App logic driven by the frame clock gets postponed until observation, turning idle model-wait time into screenshot latency. After an action, release frames for a few seconds before stopping.
- Clients that get no frame callbacks may hang. Besides withholding `wl_surface.frame` callbacks, the xdg-shell `suspended` state can tell an app its window is not visible (toolkit support unverified). Test both.
- Whether the widget tree keeps updating while frames are stopped needs to be measured.

## Known risks

- The shared compositor is in the trusted computing base of every agent; if it crashes, all agents are affected.
- A frozen client does not read its socket. If the compositor keeps sending events, the buffer fills up and the connection is dropped.
- Xwayland apps and video or animations with their own timers are not bound by frame callbacks.
- Whether Chromium's own sandbox works inside bubblewrap needs to be tested.

## Decided

- Base library: wlroots (C). Reason: mature, ships a headless backend and screencopy, and sway's multi-seat support is built on it.
- Starting point for step 2: written from scratch following the structure of tinywl. cage is a single-app kiosk, and labwc carries full window management for humans; both need more rework than adding protocols to the tinywl structure. Reasons in [compositor](compositor/README.md#结构).

## Open

- X11 apps: GIMP 2.10 and VLC 3 in the OSWorld image only run under X11 (unverified). Either add Xwayland and have the compositor double as an X11 window manager, or upgrade to GIMP 3 and VLC 4 and diverge from the OSWorld environment.
- How to drive the minimal desktop: an online model, or widget-locating scripts.
- License: wlroots is MIT; staying with MIT is simplest.
