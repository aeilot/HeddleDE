"""按会话（setsid 启动的应用及其全部子进程）采样 CPU 与内存，只读 /proc。"""
from __future__ import annotations

import os
import time

CLK = os.sysconf("SC_CLK_TCK")


def _stat(pid: int):
    try:
        with open(f"/proc/{pid}/stat") as f:
            s = f.read()
    except OSError:
        return None
    rp = s.rindex(")")
    comm = s[s.index("(") + 1:rp]
    fields = s[rp + 2:].split()
    # fields[0]=state, [3]=session, [11]=utime, [12]=stime
    return comm, int(fields[3]), int(fields[11]) + int(fields[12])


def session_pids(sid: int) -> dict[int, str]:
    out = {}
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        st = _stat(int(d))
        if st and st[1] == sid:
            out[int(d)] = st[0]
    return out


def cpu_ticks(pids) -> dict[int, tuple[str, int]]:
    out = {}
    for pid in pids:
        st = _stat(pid)
        if st:
            out[pid] = (st[0], st[2])
    return out


def mem_kib(pid: int) -> dict[str, int]:
    r = {}
    try:
        with open(f"/proc/{pid}/smaps_rollup") as f:
            for line in f:
                k, _, v = line.partition(":")
                if k in ("Rss", "Pss", "Pss_Anon", "Pss_File", "Pss_Shmem", "Private_Dirty"):
                    r[k] = int(v.split()[0])
    except OSError:
        pass
    return r


def session_mem(sid: int) -> dict:
    total: dict[str, int] = {}
    procs = []
    for pid, comm in session_pids(sid).items():
        m = mem_kib(pid)
        procs.append({"pid": pid, "comm": comm, **m})
        for k, v in m.items():
            total[k] = total.get(k, 0) + v
    return {"total_kib": total, "procs": sorted(procs, key=lambda p: -p.get("Pss", 0))}


class CpuSampler:
    """每 interval 秒记录一次会话内各进程的 CPU 核数。"""

    def __init__(self, sids: dict[str, int], interval: float = 1.0):
        self.sids = sids  # 名字 -> 会话 id
        self.interval = interval

    def sample(self, duration: float) -> list[dict]:
        rows = []
        prev = {name: cpu_ticks(session_pids(sid)) for name, sid in self.sids.items()}
        t0 = time.monotonic()
        last = t0
        while True:
            now = time.monotonic()
            remaining = duration - (now - t0)
            if remaining <= 0:
                break
            time.sleep(min(self.interval, remaining))
            now = time.monotonic()
            dt = now - last
            last = now
            row = {"t": round(now - t0, 3)}
            for name, sid in self.sids.items():
                cur = cpu_ticks(session_pids(sid))
                per = {}
                for pid, (comm, ticks) in cur.items():
                    p = prev[name].get(pid)
                    d = ticks - (p[1] if p else ticks)
                    if d > 0:
                        per[f"{comm}:{pid}"] = round(d / CLK / dt, 4)
                row[name] = {"cores": round(sum(per.values()), 4), "procs": per}
                prev[name] = cur
            rows.append(row)
        return rows


def session_cpu_seconds(sid: int) -> float:
    """会话内现存进程累计的 CPU 秒数（已退出进程不计）。"""
    return sum(t for _, t in cpu_ticks(session_pids(sid)).values()) / CLK
