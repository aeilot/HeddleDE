"""匿名内存里是什么：按进程角色、内存区、页内容特征拆分（供 ksmscan --anatomy 使用）。

页内容特征（按顺序判定，取第一个符合的）：
  zero        全零
  sparse      至少 75% 的字节为零
  text        至少 60% 的字节是可打印 ASCII（含空白）
  pointer     至少 25% 的 8 字节对齐字指向本进程已映射的地址
  incompress  lz4 压不到 3/4 页（图像、已压缩数据、随机数）
  other       其余（整数、浮点、32 位压缩指针等）

指针判定只认 64 位原始指针；V8 开启指针压缩后堆里是 32 位偏移，会落进 other。
"""
from __future__ import annotations

import bisect
import os
import struct
from collections import Counter, defaultdict

from compress import PAGE, lz4_size

PRINTABLE = bytes(range(0x20, 0x7f)) + b"\t\n\r"
WORDS = struct.Struct(f"{PAGE // 8}Q")
M = 2 ** 20


def process_info(pid):
    """角色名与映射表。Chromium 按 --type 区分浏览器、渲染、GPU、工具进程。"""
    try:
        with open(f"/proc/{pid}/comm") as f:
            comm = f.read().strip()
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            argv = f.read().split(b"\0")
        vmas = []
        with open(f"/proc/{pid}/maps") as f:
            for line in f:
                parts = line.split(maxsplit=5)
                lo, hi = (int(x, 16) for x in parts[0].split("-"))
                vmas.append((lo, hi, parts[5].strip() if len(parts) > 5 else ""))
    except OSError:
        return None
    role = comm
    if comm.startswith("chrom"):
        # Chromium 会改写进程标题，整条命令行可能变成一个带空格的字符串
        args = b" ".join(argv).decode(errors="replace").split()
        t = next((a.split("=", 1)[1] for a in args if a.startswith("--type=")), None)
        sub = next((a.split("=", 1)[1] for a in args if a.startswith("--utility-sub-type=")), None)
        if t is None:
            role = "chromium browser"
        elif t == "utility" and sub:
            role = "chromium utility " + sub.split(".")[-1]
        else:
            role = "chromium " + t
    vmas.sort()
    return {"role": role, "vmas": vmas, "starts": [v[0] for v in vmas]}


def mapping_kind(info, addr):
    i = bisect.bisect_right(info["starts"], addr) - 1
    if i < 0:
        return "?"
    lo, hi, name = info["vmas"][i]
    if addr >= hi:
        return "?"
    if name == "[heap]":
        return "malloc heap ([heap])"
    if name == "[stack]":
        return "main stack"
    if name.startswith("/"):
        return "relocated data " + os.path.basename(name)[:28]
    if name.startswith("[anon:"):
        return name[1:-1][:32]
    if name:
        return name[:32]
    size = hi - lo
    if size >= 1 << 30:
        return "anon reservation >=1 GiB"
    if 60 << 20 <= size <= 64 << 20:
        return "anon 64 MiB (malloc arena)"
    if size >= 16 << 20:
        return "anon 16 MiB-1 GiB"
    if size >= 1 << 20:
        return "anon 1-16 MiB"
    return "anon <1 MiB"


def is_pointer_factory(info):
    starts, vmas = info["starts"], info["vmas"]
    lo_all, hi_all = vmas[0][0], vmas[-1][1]

    def is_ptr(v):
        if v < lo_all or v >= hi_all:
            return False
        i = bisect.bisect_right(starts, v) - 1
        return i >= 0 and v < vmas[i][1]
    return is_ptr


def content_class(page, is_ptr):
    if page is None:
        return "zero", 0
    zeros = page.count(0)
    c = lz4_size(page)
    if zeros >= PAGE * 3 // 4:
        return "sparse", c
    if sum(page.count(b) for b in b" etaoinsrhl") >= PAGE // 4 and \
            len(page.translate(None, PRINTABLE)) <= PAGE * 4 // 10:
        return "text", c
    words = WORDS.unpack(page)
    if sum(1 for w in words if w and is_ptr(w)) >= len(words) // 4:
        return "pointer", c
    if c > PAGE * 3 // 4:
        return "incompress", c
    return "other", c


def anatomy(pages, infos, agents, sample_every=4):
    """pages: pfn -> [hash, group, comm, kind, stable, content, pid, addr]。
    返回每个 agent 平均的 MiB（按角色、内存区、内容特征），以及各内容特征的 lz4 压缩比。
    内容特征按每 sample_every 页抽一页统计后放大。"""
    n = len(agents)
    by_role, by_map = Counter(), Counter()
    cls_pages, cls_comp = Counter(), Counter()
    role_cls = defaultdict(Counter)
    ptr_fn = {}
    k = 0
    for p in pages.values():
        if p[1] not in agents:
            continue
        info = infos.get(p[6])
        if info is None:
            continue
        role = info["role"]
        by_role[role] += 1
        by_map[mapping_kind(info, p[7])] += 1
        k += 1
        if k % sample_every:
            continue
        if p[6] not in ptr_fn:
            ptr_fn[p[6]] = is_pointer_factory(info)
        cls, c = content_class(p[5], ptr_fn[p[6]])
        cls_pages[cls] += sample_every
        cls_comp[cls] += c * sample_every
        role_cls[role][cls] += sample_every
    per = lambda c: {k: round(v * PAGE / M / n, 1) for k, v in c.most_common()}  # noqa: E731
    return {
        "by_role_mib": per(by_role),
        "by_mapping_mib": per(by_map),
        "by_content_mib": per(cls_pages),
        "content_lz4_ratio": {k: round(cls_pages[k] * PAGE / cls_comp[k], 1) if cls_comp[k] else None
                              for k in cls_pages},
        "role_content_mib": {r: per(c) for r, c in role_cls.items()},
    }
