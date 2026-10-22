#!/usr/bin/env python3
"""Turns the addresses of a crash or hang report (logs/crash-*.txt,
logs/hang-*.txt, src/platform/diag.h) into functions and source lines.

    python tools/symbolize.py REPORT [--exe build/release/reunion.exe]

Windows reports give frames as "reunion.exe+0x1a2b3": the offset from where
the program was loaded. addr2line wants the address the linker gave, so the
image base from objdump is added. Needs binutils (MSYS2: mingw-w64-ucrt-x86_64-gcc
brings them) and the very build that wrote the report: the RelWithDebInfo
release keeps its debug info. Linux and macOS reports have names already.
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

FRAME = re.compile(r"#(\d+)\s+0x[0-9a-fA-F]+\s+(\S+?)\+0x([0-9a-fA-F]+)")


def tool(name):
    for candidate in (name, f"C:/msys64/ucrt64/bin/{name}.exe"):
        if shutil.which(candidate) or Path(candidate).exists():
            return candidate
    sys.exit(f"{name} not found (install binutils, e.g. MSYS2's mingw-w64-ucrt-x86_64-gcc)")


def image_base(objdump, exe):
    out = subprocess.run([objdump, "-p", str(exe)], capture_output=True, text=True, check=True).stdout
    m = re.search(r"^ImageBase\s+([0-9a-fA-F]+)", out, re.M)
    return int(m.group(1), 16) if m else 0x140000000


def main():
    here = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("report", type=Path)
    ap.add_argument("--exe", type=Path, default=here / "build/release/reunion.exe")
    args = ap.parse_args()

    lines = args.report.read_text(encoding="utf-8", errors="replace").splitlines()
    frames = [(i, FRAME.search(line)) for i, line in enumerate(lines)]
    ours = [(i, m) for i, m in frames if m and m.group(2).lower() == args.exe.name.lower()]
    if not ours:
        print("\n".join(lines))
        return
    base = image_base(tool("objdump"), args.exe)
    addrs = [hex(base + int(m.group(3), 16)) for _, m in ours]
    out = subprocess.run([tool("addr2line"), "-f", "-C", "-p", "-e", str(args.exe), *addrs],
                         capture_output=True, text=True, check=True).stdout.splitlines()
    where = {i: text for (i, _), text in zip(ours, out)}
    for i, line in enumerate(lines):
        print(f"{line}   {where[i]}" if i in where else line)


if __name__ == "__main__":
    main()
