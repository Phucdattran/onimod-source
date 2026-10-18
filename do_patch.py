#!/usr/bin/env python3
"""
Patch libTool.so  -- remove key-check gate.
Usage:
  1. Put the Zygisk zip next to this script  (or adjust SRC_ZIP below)
  2. python3 do_patch.py
  3. Flash  patched_il2cpp_tool.zip  with Magisk / KernelSU
"""
import struct, zipfile, shutil, os, hashlib, tempfile

# -- adjust these paths if needed ------------------------------------------
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SRC_ZIP    = os.path.join(SCRIPT_DIR, "36f90196-ImGui-IL2CPP-Tool-v1.1.0-721-release-zygisk.zip")
OUT_ZIP    = os.path.join(SCRIPT_DIR, "patched_il2cpp_tool.zip")
# --------------------------------------------------------------------------

NOP64 = b'\x1f\x20\x03\xd5'   # ARM64 NOP

# ------------------------------------------------------------------
#  ARM64 decoder (minimal subset needed)
# ------------------------------------------------------------------
def decode_arm64(w, vma=0):
    if (w & 0x9f000000) == 0x90000000:
        rd = w & 0x1f
        immlo = (w >> 29) & 3; immhi = (w >> 5) & 0x7ffff
        imm = ((immhi << 2) | immlo) << 12
        if imm >= (1 << 32): imm -= (1 << 33)
        return ('ADRP', rd, (vma & ~0xfff) + imm)
    if (w & 0xffc00000) == 0x39400000:
        return ('LDRB', w&0x1f, (w>>5)&0x1f, (w>>10)&0xfff)
    if (w & 0xffc00000) == 0xb9400000:
        return ('LDR_W', w&0x1f, (w>>5)&0x1f, ((w>>10)&0xfff)<<2)
    for mask, shift, name in [
        (0xff000000, 0x34000000, 'CBZ_W'),
        (0xff000000, 0x35000000, 'CBNZ_W'),
        (0xff000000, 0xb4000000, 'CBZ_X'),
        (0xff000000, 0xb5000000, 'CBNZ_X'),
    ]:
        if (w & mask) == shift:
            imm=(w>>5)&0x7ffff
            if imm&(1<<18): imm-=(1<<19)
            return (name, w&0x1f, vma+imm*4)
    return None

# ------------------------------------------------------------------
#  Scan arm64 .so for flag-read → conditional-branch pairs
# ------------------------------------------------------------------
def find_flag_branches(data):
    TEXT_VMA = 0x330580; TEXT_END = TEXT_VMA + 0x6955a0
    BSS_VMA  = 0x9fb570; BSS_END  = BSS_VMA  + 0x5317c
    DATA_VMA = 0x9f8800; DATA_END = DATA_VMA  + 0x2d68
    BRANCHES = {'CBZ_W','CBNZ_W','CBZ_X','CBNZ_X'}
    def is_bss(v): return (BSS_VMA<=v<BSS_END) or (DATA_VMA<=v<DATA_END)

    from collections import Counter, defaultdict
    branch_cnt  = Counter()
    store_cnt   = Counter()
    branch_locs = defaultdict(list)
    adrp = {}; last = {}
    for vma in range(TEXT_VMA, TEXT_END, 4):
        w = struct.unpack_from('<I', data, vma)[0]
        d = decode_arm64(w, vma)
        if not d: continue
        op = d[0]
        if op == 'ADRP':
            adrp[d[1]] = d[2]
        elif op in ('LDRB','LDR_W'):
            _,rt,rn,imm = d
            if rn in adrp:
                gv = adrp[rn]+imm
                if is_bss(gv): last[rt] = (vma, gv)
        elif op in BRANCHES:
            rt = d[1]
            if rt in last:
                lv, gv = last[rt]
                if vma - lv <= 16:
                    branch_cnt[gv] += 1
                    branch_locs[gv].append((lv, vma, op))
    return branch_cnt, branch_locs

# ------------------------------------------------------------------
#  Patch arm64 libTool.so
# ------------------------------------------------------------------
KNOWN = [
    (0x522ec0, 'CBZ_W'),
    (0x527140, 'CBZ_W'),
    (0x5c0274, 'CBZ_W'),
    (0x606a64, 'CBZ_W'),
]

def patch_arm64(path):
    with open(path, 'rb') as f: data = bytearray(f.read())
    print("[arm64] Scanning...")
    branch_cnt, branch_locs = find_flag_branches(data)

    dynamic = set()
    for gv, cnt in branch_cnt.items():
        if 1 <= cnt <= 4:
            for lv, bv, op in branch_locs[gv]:
                if bv >= 0x500000:
                    dynamic.add((bv, op))

    patch_set = set(KNOWN) | dynamic
    applied = []
    for bv, op in sorted(patch_set):
        w = struct.unpack_from('<I', data, bv)[0]
        d = decode_arm64(w, bv)
        if d and d[0] == op:
            data[bv:bv+4] = NOP64
            applied.append(bv)
            print(f"  PATCHED {op} @0x{bv:08x}  was=0x{w:08x}")
        else:
            print(f"  SKIP   @0x{bv:08x}  (expected {op}, found {d})")

    with open(path, 'wb') as f: f.write(data)
    print(f"[arm64] {len(applied)} locations patched.")
    return applied

def patch_arm32(path):
    try:
        with open(path, 'rb') as f: data = bytearray(f.read())
    except FileNotFoundError:
        print(f"[arm32] not found, skipping: {path}"); return []
    NOP_T = b'\x00\xbf'
    applied = []
    i = 0
    while i < len(data) - 4:
        h = struct.unpack_from('<H', data, i)[0]
        if (h & 0xFD00) == 0xB100:
            for back in range(2, 8, 2):
                if i < back: break
                prev = struct.unpack_from('<H', data, i-back)[0]
                if (prev & 0xF800) == 0x7800:
                    data[i:i+2] = NOP_T
                    applied.append(i); break
        i += 2
    with open(path, 'wb') as f: f.write(data)
    print(f"[arm32] {len(applied)} patched.")
    return applied

def patch_x86(path, bits=32):
    try:
        with open(path, 'rb') as f: data = bytearray(f.read())
    except FileNotFoundError:
        print(f"[x86-{bits}] not found, skipping: {path}"); return []
    applied = []
    i = 0
    while i < len(data) - 6:
        b = data[i]
        if b in (0x74, 0x75):
            for back in range(2, 20):
                if i < back: break
                if data[i-back:i-back+2] == b'\x0f\xb6':
                    data[i] = 0x90; data[i+1] = 0x90
                    applied.append(i); break
            i += 2
        elif b == 0x0f and i+1 < len(data) and data[i+1] in (0x84, 0x85):
            for back in range(3, 24):
                if i < back: break
                if data[i-back:i-back+2] == b'\x0f\xb6':
                    data[i:i+6] = b'\x90'*6
                    applied.append(i); break
            i += 6
        else:
            i += 1
    with open(path, 'wb') as f: f.write(data)
    print(f"[x86-{bits}] {len(applied)} patched.")
    return applied

# ------------------------------------------------------------------
#  Main
# ------------------------------------------------------------------
def main():
    if not os.path.exists(SRC_ZIP):
        print(f"ERROR: source zip not found: {SRC_ZIP}")
        print("Put the original Zygisk zip next to this script and retry.")
        return

    work = tempfile.mkdtemp(prefix="iltool_patch_")
    print(f"Extracting to {work} ...")
    with zipfile.ZipFile(SRC_ZIP) as zf:
        zf.extractall(work)

    a64 = os.path.join(work, "lib/arm64-v8a/libTool.so")
    a32 = os.path.join(work, "lib/armeabi-v7a/libTool.so")
    x86 = os.path.join(work, "lib/x86/libTool.so")
    x64 = os.path.join(work, "lib/x86_64/libTool.so")

    print("\n=== arm64-v8a ===");  patch_arm64(a64)
    print("\n=== armeabi-v7a ==="); patch_arm32(a32)
    print("\n=== x86 ===");         patch_x86(x86, 32)
    print("\n=== x86_64 ===");      patch_x86(x64, 64)

    print(f"\nRepacking -> {OUT_ZIP}")
    with zipfile.ZipFile(OUT_ZIP, 'w', compression=zipfile.ZIP_DEFLATED) as zf:
        for root, dirs, files in os.walk(work):
            for fname in files:
                fp = os.path.join(root, fname)
                zf.write(fp, os.path.relpath(fp, work))

    shutil.rmtree(work)
    print(f"\nDone.  Flash:  {OUT_ZIP}")

if __name__ == "__main__":
    main()
