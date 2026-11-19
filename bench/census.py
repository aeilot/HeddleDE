"""一个 agent 的沙盒状态全量记账：每个 agent 一个 cgroup，合成器一个 cgroup。

内存（cgroup memory.stat，按 agent 平均）：
  anon          进程的匿名页（堆、栈、JIT、重定位后的库数据）
  shmem         共享内存与 tmpfs：wl_shm 窗口缓冲、memfd、/dev/shm 段、沙盒内的 tmpfs 文件
  file_cache    file - shmem：计入本 cgroup 的页缓存（私有文件，以及本 cgroup 首次读入的共享文件）
  kernel        内核为它分配的内存，细分为内核栈、页表、slab（可回收/不可回收）、套接字缓冲、percpu、vmalloc。
                OrbStack 以 cgroup.memory=nokmem,nosocket 启动，这几项不按 cgroup 记账（读出为 0），
                所以另用系统 /proc/meminfo 在启动前后的差值除以 N 估计（含合成器的份额），
                并逐进程累加 VmPTE 得到页表
另外统计：
  私有文件     各 agent 的 HOME、/tmp、运行目录在磁盘上的大小，及其中常驻页缓存（mincore）
  沙盒 tmpfs   沙盒内 /dev/shm、/run 的占用（经 /proc/PID/root 进入其挂载命名空间）
  内核对象     进程数、线程数、文件描述符、套接字、memfd、挂载点数
  合成器       合成器 cgroup 的用量 / N

  python3 bench/census.py --heddle /tmp/heddle-build/heddle --app chromium -n 4
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from memcurve import APPS, PAGES, ROOT, Heddle, kill_tree, wait  # noqa: E402

CG = "/sys/fs/cgroup"
KEYS = ("anon", "file", "shmem", "file_mapped", "file_dirty", "kernel", "kernel_stack", "pagetables",
        "slab_reclaimable", "slab_unreclaimable", "sock", "percpu", "vmalloc")
PAGE = 4096
libc = ctypes.CDLL("libc.so.6", use_errno=True)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
libc.mincore.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p]
MAP_FAILED = ctypes.c_void_p(-1).value


def resident_bytes(path):
    """文件在页缓存中常驻的字节数。"""
    try:
        size = os.path.getsize(path)
        if size == 0:
            return 0
        fd = os.open(path, os.O_RDONLY)
    except OSError:
        return 0
    try:
        addr = libc.mmap(None, size, 1, 1, fd, 0)  # PROT_READ, MAP_SHARED
        if addr in (None, MAP_FAILED):
            return 0
        n = (size + PAGE - 1) // PAGE
        vec = ctypes.create_string_buffer(n)
        ok = libc.mincore(addr, size, vec) == 0
        libc.munmap(addr, size)
        return sum(b & 1 for b in vec.raw) * PAGE if ok else 0
    finally:
        os.close(fd)


def tree_stats(root):
    """磁盘大小（按块）与常驻页缓存。"""
    disk = cache = files = 0
    for dp, _, fns in os.walk(root):
        for fn in fns:
            p = os.path.join(dp, fn)
            try:
                st = os.lstat(p)
            except OSError:
                continue
            if not os.path.isfile(p) or os.path.islink(p):
                continue
            files += 1
            disk += st.st_blocks * 512
            cache += resident_bytes(p)
    return {"disk": disk, "cache": cache, "files": files}


def du_bytes(path):
    try:
        out = subprocess.run(["du", "-sk", "-x", path], capture_output=True, text=True, timeout=30).stdout
        return int(out.split()[0]) * 1024
    except Exception:  # noqa: BLE001
        return None


def memstat(path):
    st = {}
    for line in open(os.path.join(path, "memory.stat")):
        k, v = line.split()
        st[k] = int(v)
    r = {k: st.get(k, 0) for k in KEYS}
    r["file_cache"] = r["file"] - r["shmem"]
    r["current"] = int(open(os.path.join(path, "memory.current")).read())
    return r


def objects(pids):
    o = {"procs": 0, "threads": 0, "fds": 0, "sockets": 0, "memfds": 0, "vmpte": 0}
    for pid in pids:
        try:
            for line in open(f"/proc/{pid}/status"):
                if line.startswith("Threads:"):
                    o["threads"] += int(line.split()[1])
                elif line.startswith("VmPTE:"):
                    o["vmpte"] += int(line.split()[1]) * 1024
            fds = os.listdir(f"/proc/{pid}/fd")
        except OSError:
            continue
        o["procs"] += 1
        o["fds"] += len(fds)
        for fd in fds:
            try:
                t = os.readlink(f"/proc/{pid}/fd/{fd}")
            except OSError:
                continue
            o["sockets"] += t.startswith("socket:")
            o["memfds"] += t.startswith("/memfd:")
    return o


MEMINFO_KEYS = ("Slab", "SReclaimable", "SUnreclaim", "KernelStack", "PageTables", "Shmem", "Percpu", "VmallocUsed")


def meminfo():
    r = {}
    for line in open("/proc/meminfo"):
        k, _, v = line.partition(":")
        if k in MEMINFO_KEYS:
            r[k] = int(v.split()[0]) * 1024
    return r


def make_cgroups(names):
    init = os.path.join(CG, "init")
    os.makedirs(init, exist_ok=True)
    for pid in open(os.path.join(CG, "cgroup.procs")).read().split():
        try:
            with open(os.path.join(init, "cgroup.procs"), "w") as f:
                f.write(pid)
        except OSError:
            pass
    with open(os.path.join(CG, "cgroup.subtree_control"), "w") as f:
        f.write("+memory")
    parent = os.path.join(CG, "heddle-census")
    remove_cgroups(parent)
    os.mkdir(parent)
    with open(os.path.join(parent, "cgroup.subtree_control"), "w") as f:
        f.write("+memory")
    paths = {}
    for n in names:
        paths[n] = os.path.join(parent, n)
        os.mkdir(paths[n])
    return parent, paths


def remove_cgroups(parent):
    if not os.path.exists(parent):
        return
    for d in os.listdir(parent):
        p = os.path.join(parent, d)
        if os.path.isdir(p):
            t0 = time.monotonic()
            while time.monotonic() - t0 < 10:
                try:
                    os.rmdir(p)
                    break
                except OSError:
                    time.sleep(0.2)
    try:
        os.rmdir(parent)
    except OSError:
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--heddle", required=True)
    ap.add_argument("--app", choices=sorted(APPS), required=True)
    ap.add_argument("-n", type=int, default=4)
    ap.add_argument("--settle", type=float, default=15)
    ap.add_argument("--after", type=float, default=10)
    ap.add_argument("--out", default="runs/census.jsonl")
    a = ap.parse_args()

    agents = [f"a{i}" for i in range(a.n)]
    parent, cgs = make_cgroups(["comp"] + agents)

    def joiner(path):
        def f():
            with open(os.path.join(path, "cgroup.procs"), "w") as fh:
                fh.write(str(os.getpid()))
        return f

    with open("/proc/sys/vm/drop_caches", "w") as f:
        f.write("2")  # 只回收 slab（dentry、inode），让前后差值更干净
    time.sleep(1)
    mi0 = meminfo()
    rt = tempfile.mkdtemp(prefix="heddle-census-")
    os.chmod(rt, 0o700)
    env = dict(os.environ, XDG_RUNTIME_DIR=rt)
    hr = os.path.join(rt, "hd")
    state = os.path.join(rt, "state")
    argv = [a.heddle, "-R", hr, "-s", "1920x1080", "-p", "observe"]
    for x in agents:
        argv += ["-a", x]
    comp = subprocess.Popen(argv, stdout=subprocess.DEVNULL, env=env, preexec_fn=joiner(cgs["comp"]),
                            stderr=open(os.path.join(rt, "heddle.log"), "wb"), start_new_session=True)
    if not wait(lambda: os.path.exists(os.path.join(hr, "ready")), 20):
        raise RuntimeError("heddle not ready")
    ctl = {x: Heddle(os.path.join(hr, "control"), x) for x in agents}
    launchers = {}
    for x in agents:
        cmd = [os.path.join(ROOT, "scripts", "heddle-run"), "--agent", x, "--fresh", "--state", state,
               "--socket", os.path.join(hr, x, "wayland-0"), "--ro", PAGES, "--"] + shlex.split(APPS[a.app])
        launchers[x] = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, preexec_fn=joiner(cgs[x]),
                                        stderr=open(os.path.join(rt, f"app-{x}.log"), "wb"),
                                        start_new_session=True)
        time.sleep(1.0)
    up = sum(wait(lambda x=x: len(ctl[x].windows()) > 0, 120) for x in agents)
    time.sleep(a.settle)
    for x in agents:
        ctl[x].type("Agent notes for the quarterly review")
        ctl[x].key("Return")
        ctl[x].screenshot(os.path.join(rt, f"{x}.png"))
    time.sleep(a.after)

    mi1 = meminfo()
    rec = {"app": a.app, "n": a.n, "windows_up": up, "agents": {},
           "kernel_delta_per_agent": {k: round((mi1[k] - mi0[k]) / a.n) for k in MEMINFO_KEYS}}
    rec["compositor"] = memstat(cgs["comp"])
    rec["compositor"].update(objects([int(p) for p in open(os.path.join(cgs["comp"], "cgroup.procs")).read().split()]))
    for x in agents:
        pids = [int(p) for p in open(os.path.join(cgs[x], "cgroup.procs")).read().split()]
        r = memstat(cgs[x])
        r.update(objects(pids))
        inner = next((p for p in pids if p != launchers[x].pid and os.path.exists(f"/proc/{p}/root/dev/shm")), None)
        if inner:
            r["sandbox_dev_shm"] = du_bytes(f"/proc/{inner}/root/dev/shm")
            r["sandbox_run"] = du_bytes(f"/proc/{inner}/root/run")
            try:
                r["mounts"] = sum(1 for _ in open(f"/proc/{inner}/mountinfo"))
            except OSError:
                pass
        for d in ("home", "tmp", "runtime"):
            t = tree_stats(os.path.join(state, "agents", x, d))
            r[f"{d}_disk"], r[f"{d}_cache"], r[f"{d}_files"] = t["disk"], t["cache"], t["files"]
        rec["agents"][x] = r

    for x in agents:
        ctl[x].close()
        kill_tree(launchers[x].pid)
    kill_tree(comp.pid)
    shutil.rmtree(rt, ignore_errors=True)
    remove_cgroups(parent)

    keys = sorted({k for r in rec["agents"].values() for k in r})
    rec["agent_avg"] = {k: round(sum(r.get(k) or 0 for r in rec["agents"].values()) / a.n, 1) for k in keys}
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "a") as f:
        f.write(json.dumps(rec) + "\n")
    M = 2 ** 20
    av = rec["agent_avg"]
    mib = lambda k: f"{(av.get(k) or 0) / M:.1f}"  # noqa: E731
    print(f"{a.app} n={a.n} up {up}/{a.n} per agent MiB: current {mib('current')} = anon {mib('anon')} + shmem "
          f"{mib('shmem')} + file_cache {mib('file_cache')} + kernel {mib('kernel')} "
          f"(stack {mib('kernel_stack')}, pagetables {mib('pagetables')}, slab {mib('slab_reclaimable')}+"
          f"{mib('slab_unreclaimable')}, sock {mib('sock')}, percpu {mib('percpu')}, vmalloc {mib('vmalloc')}); "
          f"disk home {mib('home_disk')} tmp {mib('tmp_disk')} runtime {mib('runtime_disk')}, cache of those "
          f"{(av.get('home_cache', 0) + av.get('tmp_cache', 0) + av.get('runtime_cache', 0)) / M:.1f}; "
          f"tmpfs /dev/shm {mib('sandbox_dev_shm')} /run {mib('sandbox_run')}; procs {av.get('procs')} threads "
          f"{av.get('threads')} fds {av.get('fds')} sockets {av.get('sockets')} memfds {av.get('memfds')} "
          f"mounts {av.get('mounts')}; compositor/N {rec['compositor']['current'] / a.n / M:.1f}", file=sys.stderr)
    kd = rec["kernel_delta_per_agent"]
    print("  kernel delta per agent MiB (system-wide): " + ", ".join(f"{k} {v / M:.1f}" for k, v in kd.items())
          + f"; VmPTE sum per agent {(av.get('vmpte') or 0) / M:.1f}", file=sys.stderr)


if __name__ == "__main__":
    main()
