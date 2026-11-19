"""N 个同类 agent 的匿名页里有多少内容相同（KSM 能合并的上限）。

不开 KSM（OrbStack 内核没有编进 KSM，开了也会影响整个虚拟机），而是直接读页内容统计：

1. 启动合成器和 N 个 agent（每个一个应用，经 heddle-run），全部放进一个 cgroup。
2. 冻结 cgroup，对每个进程用 PAGEMAP_SCAN 找出常驻的匿名页（不含文件页、shmem、零页），
   从 /proc/PID/pagemap 取物理页号去掉写时复制共享的页，从 /proc/PID/mem 读内容做哈希。
3. 解冻，每个 agent 再做一步动作，等待后再扫一次。两次扫描之间内容没变的页算"稳定"：
   KSM 只合并在两轮扫描之间不变的页，所以稳定页上的重复才是实际能合并的。

统计：物理匿名页数 P、内容为全零的页、不同内容数 D。全部合并能省 P - D 页；
其中多少靠同一 agent 内部合并就能得到，多少必须跨 agent 合并。

  python3 bench/ksmscan.py --heddle /tmp/heddle-build/heddle --app chromium -n 8
"""
from __future__ import annotations

import argparse
import ctypes
import fcntl
import hashlib
import json
import os
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from memcurve import APPS, PAGES, ROOT, Heddle, descendants, kill_tree, proc_table, wait  # noqa: E402
from pressure import setup_cgroup  # noqa: E402
from compress import PAGE as _P, Zstd, lz4_size, zram_size  # noqa: E402,F401

PAGE = 4096
ZERO = bytes(PAGE)
# linux/fs.h
PAGE_IS_FILE = 1 << 2
PAGE_IS_PRESENT = 1 << 3
PAGE_IS_PFNZERO = 1 << 5
PAGE_IS_HUGE = 1 << 6
PM_SCAN_ARG = struct.Struct("12Q")
PAGEMAP_SCAN = (3 << 30) | (PM_SCAN_ARG.size << 16) | (ord("f") << 8) | 16
PFN_MASK = (1 << 55) - 1
VEC_LEN = 4096
ACTIONS = ["type:More notes after a pause", "key:Return"]


class Region(ctypes.Structure):
    _fields_ = [("start", ctypes.c_uint64), ("end", ctypes.c_uint64), ("categories", ctypes.c_uint64)]


def anon_regions(pm_fd, end):
    """常驻、非文件的页区间（含零页，由调用方按 categories 排除）。"""
    vec = (Region * VEC_LEN)()
    start = 0
    while start < end:
        arg = bytearray(PM_SCAN_ARG.pack(
            PM_SCAN_ARG.size, 0, start, end, 0, ctypes.addressof(vec), VEC_LEN, 0,
            PAGE_IS_FILE,                      # category_inverted：要求 FILE 位为 0
            PAGE_IS_PRESENT | PAGE_IS_FILE,    # category_mask
            0,
            PAGE_IS_PRESENT | PAGE_IS_FILE | PAGE_IS_PFNZERO | PAGE_IS_HUGE))
        n = fcntl.ioctl(pm_fd, PAGEMAP_SCAN, arg, True)
        for i in range(n):
            r = vec[i]
            yield r.start, r.end, r.categories
        walk_end = PM_SCAN_ARG.unpack(arg)[4]
        if walk_end <= start:
            break
        start = walk_end


def vma_names(pid):
    out = []
    with open(f"/proc/{pid}/maps") as f:
        for line in f:
            parts = line.split(maxsplit=5)
            lo, hi = (int(x, 16) for x in parts[0].split("-"))
            name = parts[5].strip() if len(parts) > 5 else ""
            out.append((lo, hi, name or "[anon]"))
    return out


def vma_kind(vmas, addr):
    for lo, hi, name in vmas:
        if lo <= addr < hi:
            if name.startswith("[anon:"):
                return name[6:-1].split()[0][:24]
            if name.startswith("["):
                return name
            if name.startswith("/"):
                return "file-private"
            return name[:24]
    return "?"


def scan(pids, group_of, prev=None, keep=False):
    """返回 pages: pfn -> [hash, group, comm, kind, stable, 内容（keep 且非零时）]，以及 (pid, addr) -> hash。"""
    pages, by_addr = {}, {}
    errors = 0
    table = proc_table()
    for pid in pids:
        comm = table.get(pid, (0, "?"))[1]
        try:
            vmas = vma_names(pid)
            if not vmas:
                continue
            end = max(hi for _, hi, _ in vmas)
            pm_fd = os.open(f"/proc/{pid}/pagemap", os.O_RDONLY)
            mem_fd = os.open(f"/proc/{pid}/mem", os.O_RDONLY)
        except OSError:
            errors += 1
            continue
        try:
            for lo, hi, cats in anon_regions(pm_fd, end):
                if cats & PAGE_IS_PFNZERO:
                    continue
                kind = vma_kind(vmas, lo)
                addr = lo
                while addr < hi:
                    chunk = min(hi - addr, 4 << 20)
                    n = chunk // PAGE
                    try:
                        ents = struct.unpack(f"{n}Q", os.pread(pm_fd, n * 8, (addr // PAGE) * 8))
                        data = os.pread(mem_fd, chunk, addr)
                    except OSError:
                        errors += 1
                        addr += chunk
                        continue
                    for i in range(min(n, len(data) // PAGE)):
                        pfn = ents[i] & PFN_MASK
                        if not (ents[i] >> 63) or pfn == 0:
                            continue
                        page = data[i * PAGE:(i + 1) * PAGE]
                        h = b"0" if page == ZERO else hashlib.blake2b(page, digest_size=16).digest()
                        a = addr + i * PAGE
                        by_addr[(pid, a)] = h
                        stable = prev is not None and prev.get((pid, a)) == h
                        p = pages.get(pfn)
                        if p is None:
                            pages[pfn] = [h, group_of[pid], comm, kind, stable,
                                          page if keep and h != b"0" else None, pid, a]
                        elif stable:
                            p[4] = True
                    addr += chunk
        finally:
            os.close(pm_fd)
            os.close(mem_fd)
    return pages, by_addr, errors


def stats(pages, only_stable=False):
    rows = [p for p in pages.values() if p[4] or not only_stable]
    P = len(rows)
    if P == 0:
        return {}
    cnt = Counter(p[0] for p in rows)
    D = len(cnt)
    zeros = cnt.get(b"0", 0)
    per_group = defaultdict(list)
    for p in rows:
        per_group[p[1]].append(p[0])
    within = sum(len(v) - len(set(v)) for v in per_group.values())
    # 每个重复内容组里第一页之外的页都能省；按进程名、内存区类型统计这些可省的页
    seen = set()
    save_comm, save_kind = Counter(), Counter()
    for p in rows:
        if cnt[p[0]] > 1:
            if p[0] in seen:
                save_comm[p[2]] += 1
                save_kind[p[3]] += 1
            else:
                seen.add(p[0])
    # 只在一个 agent 内部重复、和跨 agent 重复的内容
    groups_of_hash = defaultdict(set)
    for p in rows:
        groups_of_hash[p[0]].add(p[1])
    cross_hashes = sum(1 for h, g in groups_of_hash.items() if len(g) > 1)
    # 非零页单独算：零页合并很平凡（内核有专门的零页），真正有意义的是非零内容的重复
    nz = [p for p in rows if p[0] != b"0"]
    nz_D = len({p[0] for p in nz})
    nz_group = defaultdict(list)
    for p in nz:
        nz_group[p[1]].append(p[0])
    nz_within = sum(len(v) - len(set(v)) for v in nz_group.values())
    M = PAGE / 2 ** 20
    return {
        "pages": P, "mib": round(P * M, 1),
        "zero_mib": round(zeros * M, 1),
        "distinct": D,
        "save_all_mib": round((P - D) * M, 1), "save_all_pct": round(100 * (P - D) / P, 1),
        "save_within_agent_mib": round(within * M, 1),
        "save_cross_agent_mib": round((P - D - within) * M, 1),
        "save_nonzero_mib": round((len(nz) - nz_D) * M, 1),
        "save_nonzero_within_mib": round(nz_within * M, 1),
        "save_nonzero_cross_mib": round((len(nz) - nz_D - nz_within) * M, 1),
        "cross_agent_contents": cross_hashes,
        "save_by_comm_mib": {k: round(v * M, 1) for k, v in save_comm.most_common(8)},
        "save_by_kind_mib": {k: round(v * M, 1) for k, v in save_kind.most_common(8)},
        "per_group_mib": {g: round(len(v) * M, 1) for g, v in sorted(per_group.items())},
    }


def compression(pages, agents, window_log=28, level=3):
    """同一批页在几种存法下的大小（MiB），只算 agent，不含合成器。零页一律按 0 计。

    raw          常驻匿名页（按物理页去重，即已有的写时复制共享）
    ksm          相同内容的页只存一份（零页并入内核零页）
    zram         每页单独用 lz4 压缩，压不到 3/4 页的按原样存（zram 的做法）
    ksm_zram     先按页去重，再对每个不同内容单独压缩
    stream       每个 agent 的全部非零页连成一个流，用 zstd 长窗口压缩（空闲 agent 整体压缩）
    delta        a0 作模板按 stream 存；其他 agent 与模板内容相同的页免费，其余页
                 以模板为参考数据压缩（zstd --patch-from）
    """
    M = 2 ** 20
    by_agent = {x: [] for x in agents}
    zeros = Counter()
    for p in pages.values():
        if p[1] not in by_agent:
            continue
        if p[0] == b"0":
            zeros[p[1]] += 1
        else:
            by_agent[p[1]].append(p)
    raw = {x: (len(v) + zeros[x]) * _P for x, v in by_agent.items()}
    zram = {x: sum(zram_size(lz4_size(p[5])) for p in v) for x, v in by_agent.items()}
    seen, ksm, ksm_zram = set(), Counter(), Counter()
    for x in agents:
        for p in by_agent[x]:
            if p[0] not in seen:
                seen.add(p[0])
                ksm[x] += _P
                ksm_zram[x] += zram_size(lz4_size(p[5]))
    z = Zstd(level, window_log)
    stream = {x: z.size(b"".join(p[5] for p in v)) if v else 0 for x, v in by_agent.items()}
    t = agents[0]
    tmpl = b"".join(p[5] for p in by_agent[t])
    tmpl_hashes = {p[0] for p in by_agent[t]}
    delta = {t: stream[t]}
    for x in agents[1:]:
        rest = b"".join(p[5] for p in by_agent[x] if p[0] not in tmpl_hashes)
        delta[x] = z.size(rest, prefix=tmpl) if rest else 0
    z.close()
    out = {}
    for name, d in (("raw", raw), ("ksm", ksm), ("zram", zram), ("ksm_zram", ksm_zram),
                    ("stream", stream), ("delta", delta)):
        others = [d[x] for x in agents[1:]]
        out[name] = {"total_mib": round(sum(d.values()) / M, 1),
                     "first_mib": round(d[t] / M, 1),
                     "other_avg_mib": round(sum(others) / len(others) / M, 1) if others else None}
    return out


def freeze(cg, on):
    with open(os.path.join(cg, "cgroup.freeze"), "w") as f:
        f.write("1" if on else "0")
    want = "frozen 1" if on else "frozen 0"
    wait(lambda: want in open(os.path.join(cg, "cgroup.events")).read(), 10)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--heddle", required=True)
    ap.add_argument("--app", choices=sorted(APPS), required=True)
    ap.add_argument("-n", type=int, default=8)
    ap.add_argument("--settle", type=float, default=15)
    ap.add_argument("--gap", type=float, default=20, help="两次扫描之间的等待")
    ap.add_argument("--anatomy", action="store_true", help="按进程角色、内存区、页内容拆分匿名内存")
    ap.add_argument("--no-compress", action="store_true", help="跳过压缩统计")
    ap.add_argument("--no-aslr", action="store_true",
                    help="用 setarch -R 关闭应用的地址随机化（指针相同的页才可能跨进程相同）")
    ap.add_argument("--label", default="")
    ap.add_argument("--out", default="runs/ksm.jsonl")
    a = ap.parse_args()

    agents = [f"a{i}" for i in range(a.n)]
    cg = setup_cgroup("heddle-ksm", 0)

    def enter_cg():
        with open(os.path.join(cg, "cgroup.procs"), "w") as f:
            f.write(str(os.getpid()))

    rt = tempfile.mkdtemp(prefix="heddle-ksm-")
    os.chmod(rt, 0o700)
    env = dict(os.environ, XDG_RUNTIME_DIR=rt)
    hr = os.path.join(rt, "hd")
    argv = [a.heddle, "-R", hr, "-s", "1920x1080", "-p", "observe"]
    for x in agents:
        argv += ["-a", x]
    comp = subprocess.Popen(argv, stdout=subprocess.DEVNULL, env=env, preexec_fn=enter_cg,
                            stderr=open(os.path.join(rt, "heddle.log"), "wb"), start_new_session=True)
    if not wait(lambda: os.path.exists(os.path.join(hr, "ready")), 20):
        raise RuntimeError("heddle not ready")
    ctl = {x: Heddle(os.path.join(hr, "control"), x) for x in agents}
    launchers = {}
    for x in agents:
        cmd = [os.path.join(ROOT, "scripts", "heddle-run"), "--agent", x, "--fresh",
               "--state", os.path.join(rt, "state"), "--socket", os.path.join(hr, x, "wayland-0"),
               "--ro", PAGES, "--"] + (["setarch", "-R"] if a.no_aslr else []) + shlex.split(APPS[a.app])
        launchers[x] = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, preexec_fn=enter_cg,
                                        stderr=open(os.path.join(rt, f"app-{x}.log"), "wb"),
                                        start_new_session=True)
        time.sleep(1.0)
    up = sum(wait(lambda x=x: len(ctl[x].windows()) > 0, 120) for x in agents)
    time.sleep(a.settle)
    for x in agents:
        ctl[x].type("Agent notes for the quarterly review")
        ctl[x].key("Return")
        ctl[x].screenshot(os.path.join(rt, f"{x}.png"))
    time.sleep(5)

    def groups():
        table = proc_table()
        g = {pid: "compositor" for pid in descendants(comp.pid, table)}
        for x in agents:
            for pid in descendants(launchers[x].pid, table):
                g[pid] = x
        return g

    g1 = groups()
    freeze(cg, True)
    t0 = time.monotonic()
    pages1, addr1, err1 = scan(list(g1), g1)
    scan1_s = time.monotonic() - t0
    freeze(cg, False)

    for x in agents:
        for act in ACTIONS:
            kind, _, arg = act.partition(":")
            (ctl[x].type if kind == "type" else ctl[x].key)(arg)
        ctl[x].screenshot(os.path.join(rt, f"{x}.png"))
    time.sleep(a.gap)

    g2 = groups()
    freeze(cg, True)
    pages2, _, err2 = scan(list(g2), g2, prev=addr1, keep=True)
    infos = {}
    if a.anatomy:
        from anatomy import process_info
        infos = {pid: process_info(pid) for pid in g2}
    freeze(cg, False)

    rec = {"app": a.app, "n": a.n, "aslr": not a.no_aslr, "label": a.label, "windows_up": up, "procs": len(g2),
           "scan_s": round(scan1_s, 1), "read_errors": [err1, err2],
           "snap1": stats(pages1), "snap2": stats(pages2), "snap2_stable": stats(pages2, only_stable=True)}
    del pages1, addr1

    for x in agents:
        ctl[x].close()
        kill_tree(launchers[x].pid)
    kill_tree(comp.pid)
    t0 = time.monotonic()
    if not a.no_compress:
        rec["compression"] = compression(pages2, agents)
    if a.anatomy:
        from anatomy import anatomy
        rec["anatomy"] = anatomy(pages2, infos, agents)
    rec["compression_s"] = round(time.monotonic() - t0, 1)
    del pages2
    shutil.rmtree(rt, ignore_errors=True)
    t0 = time.monotonic()
    while time.monotonic() - t0 < 10:
        try:
            os.rmdir(cg)
            break
        except OSError:
            time.sleep(0.2)

    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "a") as f:
        f.write(json.dumps(rec) + "\n")
    s, st = rec["snap2"], rec["snap2_stable"]
    print(f"{a.app} n={a.n} aslr={not a.no_aslr}: up {up}/{a.n}, anon {s['mib']} MiB, zero {s['zero_mib']} MiB, "
          f"mergeable {s['save_all_mib']} MiB ({s['save_all_pct']}%; nonzero within-agent "
          f"{s['save_nonzero_within_mib']}, nonzero cross-agent {s['save_nonzero_cross_mib']}); "
          f"stable mergeable {st.get('save_all_mib')} MiB; scan {rec['scan_s']} s, errors {rec['read_errors']}",
          file=sys.stderr)
    if "anatomy" in rec:
        an = rec["anatomy"]
        for key in ("by_role_mib", "by_mapping_mib", "by_content_mib", "content_lz4_ratio"):
            print(f"  {key}: " + ", ".join(f"{k} {v}" for k, v in list(an[key].items())[:8]), file=sys.stderr)
    c = rec.get("compression")
    if c:
        print("  per extra agent (MiB): " + ", ".join(f"{k} {v['other_avg_mib']}" for k, v in c.items())
          + f"; first agent stream {c['stream']['first_mib']}; took {rec['compression_s']} s", file=sys.stderr)


if __name__ == "__main__":
    main()
