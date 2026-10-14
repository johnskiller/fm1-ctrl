#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build fm1-ctrl: a fork of x0x's build (Felucca platform, same toolchain).

  tools/build.py            # build/ctrl.fwsc, identity FM-1_9299
  tools/build.py --release X.Y

The app is ONE unity unit (firmware/src/ctrl.c): no DSP/sequencer O2 units,
no drum-sample generation. The font is the pregenerated felucca_font.h
(vendored; regenerating needs Pillow). Everything else is x0x's build.py:
loader packaging, checks, the .fwsc container. x0X by Charles Vestal /
Hügelton; fm1-ctrl app by johnskiller.
"""
import argparse
import hashlib
import os
import platform
import re
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
FW = SRC / "firmware"
OUT = SRC / "build"
GEN = OUT / "gen"
LDR = OUT / "loader"
sys.path.insert(0, str(SRC / "tools"))
import fm1pkg_make  # noqa: E402
import lz4blk  # noqa: E402

APP_XIP = 0x02000120
APP_SLOT = fm1pkg_make.APP_SLOT
LOADER_LOAD = 0x01C0A800
LOADER_NAME = b"usb_hid_ota.bin"
CFLAGS = ["-Os", "-ffunction-sections", "-fno-builtin", "-Wall", "-Wno-unused-function"]
FPU = ["-mcpu=r3", "-mfprev1", "-ffp-contract=off"]
LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")

PRODUCT = "FM-1_9299"


def toolchain():
    tc = os.environ.get("JIELI_TOOLCHAIN")
    if not tc or not (Path(tc) / "pi32v2" / "bin" / "clang").exists():
        raise SystemExit("set JIELI_TOOLCHAIN (the pi32v2 toolchain checkout)")
    return Path(tc)


def use_docker():
    return platform.system() != "Linux" or platform.machine() != "x86_64" \
        or os.environ.get("JIELI_DOCKER") == "1"


def tc(tool, *args):
    """run a toolchain binary; 'cc' = clang -target pi32v2 (x0x's wrapper dodge)"""
    rel = [str(Path(a).resolve().relative_to(SRC)) if isinstance(a, Path) else a for a in args]
    if tool == "cc":
        tool, rel = "pi32v2/bin/clang", ["-target", "pi32v2", *rel]
    cmd = [str(toolchain() / tool), *rel]
    r = subprocess.run(cmd, cwd=SRC, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"build: {tool} failed")
    if r.stderr.strip():
        sys.stderr.write(r.stderr)
    return r.stdout


def tc_all(*cmds):
    with ThreadPoolExecutor(min(len(cmds), int(os.environ.get("FM1_JOBS", "4")))) as ex:
        return list(ex.map(lambda c: tc(*c), cmds))


def crc16(d, c=0):
    for b in d:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def build_app():
    flags = [*CFLAGS, *FPU, "-Ifirmware/hal", "-Ifirmware/src", "-Ifirmware", "-Ibuild/gen",
             "-DFELUCCA_UAC=0", "-DFELUCCA_CDC=0"]
    flags.append(f'-DFELUCCA_ID="{PRODUCT}"')
    units = [("cc", "-c", FW / "crt0.S", "-o", OUT / "crt0.o"),
             ("cc", "-c", FW / "hal" / "fm1_vec.S", "-o", OUT / "fm1_vec.o"),
             ("cc", "-c", FW / "hal" / "fm1_isr.S", "-o", OUT / "fm1_isr.o"),
             ("cc", "-c", FW / "hal" / "fm1_cpu1.S", "-o", OUT / "fm1_cpu1.o"),
             ("cc", *flags, "-c", FW / "src" / "ctrl.c", "-o", OUT / "ctrl.o")]
    objs = [OUT / "crt0.o", OUT / "fm1_vec.o", OUT / "fm1_isr.o", OUT / "fm1_cpu1.o", OUT / "ctrl.o"]
    tc_all(*units)
    elf = OUT / "ctrl.elf"
    tc("pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", FW / "app.ld", *objs, "-o", elf)
    for sect in ("text.bin", "data.bin", "ramtext.bin", "c1text.bin"):
        (OUT / sect).unlink(missing_ok=True)
    *_, syms, dis, rt = tc_all(("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, OUT / "text.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".data", elf, OUT / "data.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".ram_text", elf, OUT / "ramtext.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".c1_text", elf, OUT / "c1text.bin"),
                               ("common/bin/objdump", "-t", elf),
                               ("common/bin/objdump", "-d", elf),
                               ("common/bin/objdump", "-d", "-j", ".ram_text", elf))
    (OUT / "ctrl.dis").write_text(dis)

    def symv(name):
        return int(re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M).group(1), 16)
    img = bytearray((OUT / "text.bin").read_bytes())
    for sect, lname in (("ramtext.bin", "_rt_load"), ("c1text.bin", "_c1_load"), ("data.bin", "_data_load")):
        load = symv(lname)
        if load % 4:
            raise SystemExit(f"{lname} {load:#x} is not word aligned")
        blob = (OUT / sect).read_bytes() if (OUT / sect).exists() else b""
        if blob:
            if load - APP_XIP < len(img):
                raise SystemExit(f"{lname} overlaps the image")
            img += b"\xff" * (load - APP_XIP - len(img))
            img += blob
    img += b"\xff" * (-len(img) % 4)
    (OUT / "ctrl.bin").write_bytes(img)
    return bytes(img), syms, dis, rt


def check(img, syms, dis, rt):
    errors, notes = [], []
    m = re.search(r"^([0-9a-f]+) .*\s_start$", syms, re.M)
    if not m or int(m.group(1), 16) != APP_XIP:
        errors.append(f"_start is not at {APP_XIP:#x}")
    if img[:4] != bytes.fromhex("04818000"):
        errors.append(f"image starts with {img[:4].hex()}, not the entry stub")
    rt_calls = [ln for ln in rt.splitlines() if re.search(r"\bcall\b", ln)]
    if rt_calls:
        errors.append(f".ram_text contains calls: {rt_calls[:3]}")
    else:
        notes.append(f".ram_text: {len([ln for ln in rt.splitlines() if LINE.match(ln)])} insns, no calls")
    if len(img) > APP_SLOT:
        errors.append(f"image {len(img)} B exceeds the app slot")

    def sym(name):
        mm = re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M)
        return int(mm.group(1), 16) if mm else 0
    bss = sym("_bss_end") - 0x01C08000
    pool = sym("_pool_end") - sym("_pool_start")
    notes.append(f"image {len(img)} B; RAM .data+.bss {bss} B of 98304; pool {pool} B of {0x54000}")
    if bss > 96 * 1024:
        errors.append("RAM region overflow")
    return errors, notes


def build_loader():
    """x0x's loader, unchanged: Felucca's update path (x0x build.py build_loader)"""
    LDR.mkdir(parents=True, exist_ok=True)
    src = FW / "loader"
    flags = [*CFLAGS, "-Ifirmware/hal", "-Ifirmware/src"]
    tc_all(("cc", "-c", src / "crt0_ldr.S", "-o", LDR / "crt0_ldr.o"),
           ("cc", *flags, "-c", src / "loader.c", "-o", LDR / "loader.o"))
    elf = LDR / "loader.elf"
    tc("pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", src / "loader.ld",
       LDR / "crt0_ldr.o", LDR / "loader.o", "-o", elf)
    _, dis, hdr, syms = tc_all(("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, LDR / "loader.bin"),
                               ("common/bin/objdump", "-d", elf),
                               ("common/bin/objdump", "-h", elf),
                               ("common/bin/objdump", "-t", elf))
    (LDR / "loader.dis").write_text(dis)
    for ln in hdr.splitlines():
        p = ln.split()
        if len(p) > 4 and p[1].startswith(".") and int(p[3], 16) >= 0x02000000 and int(p[2], 16):
            raise SystemExit(f"loader: section {p[1]} at {p[3]} is outside RAM")
    image = (LDR / "loader.bin").read_bytes()
    if not image or len(image) > 0x14000:
        raise SystemExit("loader: image empty or too big")
    return ldr_wrap(image)


def ldr_wrap(image):
    """x0x build.py ldr_head/ldr_wrap: LDR1 header + lz4 blocks"""
    out = bytearray()
    out += struct.pack("<4sHHII", b"LDR1", 1, crc16(image), LOADER_LOAD, len(image))
    i = 0
    while i < len(image):
        blk = image[i:i + 4096]
        comp = lz4blk.compress(blk)
        if len(comp) >= 4096:
            raise SystemExit("loader: block does not compress below 4096 B")
        out += struct.pack("<HHH", 0x4C42, len(comp), len(blk)) + comp
        i += 4096
    out += struct.pack("<HH", 0, 0)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--release", metavar="X.Y[.Z]", help="identity FM-1_9XXYYZZ")
    a = ap.parse_args()
    global PRODUCT
    if a.release:
        m = re.fullmatch(r"(\d{1,2})\.(\d{1,2})(?:\.(\d{1,2}))?", a.release)
        if not m:
            raise SystemExit("--release X.Y[.Z]")
        PRODUCT = "FM-1_9%02d%02d%02d" % (int(m[1]), int(m[2]), int(m[3] or 0))
    name = "ctrl.fwsc"
    toolchain()
    GEN.mkdir(parents=True, exist_ok=True)
    # the font: vendored pregenerated header (gen_font.py needs Pillow)
    (GEN / "felucca_font.h").write_bytes((FW / "gen_font_pregen.h").read_bytes())
    with ThreadPoolExecutor(2) as ex:
        ldr = ex.submit(build_loader)
        img, syms, dis, rt = build_app()
        ota = ldr.result()
    errors, notes = check(img, syms, dis, rt)
    for n in notes:
        print("  ok   ", n)
    for e in errors:
        print("  FAIL ", e)
    if errors:
        raise SystemExit("build: checks failed")
    pkg = fm1pkg_make.ufw(fm1pkg_make.flash_image(img, fm1pkg_make.KEY), ota, PRODUCT)
    (OUT / name).write_bytes(pkg)
    print(f"app      {OUT / 'ctrl.bin'}  {len(img)} B")
    print(f"loader   {LDR / 'ota.bin'}  {len(ota)} B")
    print(f"package  {OUT / name}  {len(pkg)} B, identity {PRODUCT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
