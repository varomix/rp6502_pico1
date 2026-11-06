#!/usr/bin/env python3
"""Pico 1 port: find code reachable from RAM-required roots that sits in flash.

Usage: hotpath.py <objdump> <nm> <elf> root_regex [root_regex ...]

Disassembles the ELF, builds a direct call graph (bl / b.w / b tail calls to
other functions, plus literal-pool words that point at functions, which is how
M0+ code loads function pointers), and walks it from every function whose
name matches a root regex. Prints reachable functions that live in flash
(0x10xxxxxx), largest first, with the chain that reached them.
"""
import re, subprocess, sys

objdump, nm, elf, *roots = sys.argv[1:]
syms = {}
for line in subprocess.run([nm, "-S", "-n", elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 4 and p[2] in "tTwWdDrRbB":
        a, s = int(p[0], 16) & ~1, int(p[1], 16)
        syms[a] = (p[3], s)
addrs = sorted(syms)
import bisect
def owner(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i >= 0:
        s = addrs[i]; n, sz = syms[s]
        if s <= a < s + max(sz, 2): return s
    return None

edges = {a: set() for a in addrs}
cur = None
dis = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf], capture_output=True, text=True).stdout
for line in dis.splitlines():
    m = re.match(r"^([0-9a-f]{8}) <(.+)>:$", line)
    if m:
        cur = owner(int(m.group(1), 16)); continue
    if cur is None: continue
    m = re.match(r"^\s*([0-9a-f]+):\s+(\S+)\s+(.*)$", line)
    if not m: continue
    op, args = m.group(2), m.group(3)
    t = None
    if op.startswith(("bl", "b.", "b")) and not op.startswith("bx") and not op.startswith("blx"):
        mm = re.match(r"([0-9a-f]+) <", args)
        if mm: t = int(mm.group(1), 16)
    elif op == ".word":
        mm = re.match(r"0x([0-9a-f]+)", args)
        if mm: t = int(mm.group(1), 16) & ~1
    if t is not None:
        o = owner(t)
        if o is not None and o != cur:
            edges[cur].add(o)

rootset = [a for a in addrs if any(re.fullmatch(r, syms[a][0]) for r in roots)]
seen = {a: None for a in rootset}
stack = list(rootset)
while stack:
    a = stack.pop()
    for b in edges[a]:
        if b not in seen:
            seen[b] = a; stack.append(b)
def chain(a):
    c = []
    while a is not None and len(c) < 6:
        c.append(syms[a][0]); a = seen[a]
    return " <- ".join(c)
flash = [a for a in seen if 0x10000000 <= a < 0x20000000]
tot = sum(syms[a][1] for a in flash)
ram = sum(syms[a][1] for a in seen if a >= 0x20000000)
print(f"roots={len(rootset)} reachable={len(seen)} in_flash={len(flash)} flash_bytes={tot} ram_bytes={ram}")
for a in sorted(flash, key=lambda a: -syms[a][1]):
    print(f"{syms[a][1]:7} {chain(a)}")
