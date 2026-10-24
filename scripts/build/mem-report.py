#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Where the memory goes in an ESP-IDF image (the T-Display P4's CI build).

    mem-report.py <elf> <map> <tool prefix, e.g. riscv32-esp-elf-> > report.txt

Sections, then every symbol by size grouped by the section it lives in (so
internal RAM, PSRAM and flash each get their own list), then the totals by
library and by object file for each of them. Uses only binutils and the map.
"""
import collections, re, subprocess, sys

elf, mp, tp = sys.argv[1], sys.argv[2], sys.argv[3]

def run(*a):
    return subprocess.run(list(a), capture_output=True, text=True).stdout

# Sections: name, size, address.
secs = []
for l in run(tp + "objdump", "-h", elf).splitlines():
    m = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+[0-9a-f]+", l)
    if m:
        secs.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
alloc = [s for s in secs if s[2] and not s[0].startswith(".debug") and s[1]]

def kind(name):
    n = name.lower()
    if "dummy" in n: return "other"   # one memory seen through another bus (S3)
    if "ext_ram" in n or "psram" in n: return "PSRAM"
    if n.startswith(".flash") or "rodata" in n and "dram" not in n: return "flash"
    if any(k in n for k in ("dram", "iram", "rtc", "tcm", "lp_", ".noinit", ".data", ".bss")): return "internal RAM"
    return "other"

print("sections (allocated)")
tot = collections.Counter()
for n, sz, a in sorted(alloc, key=lambda s: -s[1]):
    print(f"{sz:10} {n} @0x{a:08x} [{kind(n)}]")
    tot[kind(n)] += sz
print()
for k, v in tot.most_common():
    print(f"total {k}: {v} B")
print()

def sec_of(addr):
    for n, sz, a in alloc:
        if a <= addr < a + sz: return n
    return None

syms = collections.defaultdict(list)
for l in run(tp + "nm", "-S", "--size-sort", "-C", elf).splitlines():
    p = l.split(None, 3)
    if len(p) < 4: continue
    try:
        addr, sz = int(p[0], 16), int(p[1], 16)
    except ValueError:
        continue
    s = sec_of(addr)
    if s: syms[kind(s)].append((sz, p[3], s))
for k in ("internal RAM", "PSRAM", "flash"):
    lst = sorted(syms.get(k, []), reverse=True)
    print(f"largest symbols in {k}")
    for sz, name, s in lst[:250 if k != "flash" else 400]:
        print(f"{sz:8} {name}  ({s})")
    print()

# Input sections from the map: which library/object fills which memory.
by_lib = collections.defaultdict(collections.Counter)
by_obj = collections.defaultdict(collections.Counter)
cur_out = None
pend = None
for l in open(mp, errors="replace"):
    m = re.match(r"^(\.\S+)\s", l)
    if m and not l.startswith(" "):
        cur_out = m.group(1)
    m = re.match(r"^ (\.\S+)\s*$", l)
    if m:
        pend = m.group(1); continue
    m = re.match(r"^ (\.\S+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", l)
    if not m or not cur_out: continue
    sz = int(m.group(3), 16)
    if not sz: continue
    src = m.group(4)
    k = kind(cur_out)
    lib = re.sub(r"\(.*\)$", "", src).split("/")[-1]
    obj = src.split("/")[-1]
    by_lib[k][lib] += sz
    by_obj[k][obj] += sz
for k in ("internal RAM", "PSRAM", "flash"):
    print(f"by library: {k}")
    for name, n in by_lib[k].most_common(80):
        print(f"{n:10} {name}")
    print()
    print(f"by object: {k}")
    for name, n in by_obj[k].most_common(120):
        print(f"{n:10} {name}")
    print()
