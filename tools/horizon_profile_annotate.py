#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-3.0+

"""Annotates a Switch profile report for reading.

Usage: horizon_profile_annotate.py profile.txt [--elf build/switch-vk/switch/app/armsx2.elf] > annotated.txt
"""

import argparse
import collections
import os
import re
import struct
import subprocess
import sys
import tempfile

DEVKITA64_BIN = "/opt/devkitpro/devkitA64/bin"
MODULE_LABEL = re.compile(r"\bm:([0-9a-f]+)\b")
SELF_LINE = re.compile(r"^\s+([0-9.]+)%\s+(\d+)\s+(\S+)$")
BLOCK_LINE = re.compile(r"^(\s+\+0x[0-9a-f]+\s+\d+\s+)([0-9a-f]{8})$")


def tool(name):
    path = os.path.join(DEVKITA64_BIN, "aarch64-none-elf-" + name)
    return path if os.path.exists(path) else "aarch64-none-elf-" + name


def symbolize(elf, offsets):
    if not offsets:
        return {}
    ordered = sorted(offsets)
    result = subprocess.run([tool("addr2line"), "-f", "-C", "-e", elf], input="\n".join(hex(o) for o in ordered),
                            capture_output=True, text=True, check=True)
    lines = result.stdout.splitlines()
    names = {}
    for i, offset in enumerate(ordered):
        name = lines[2 * i] if 2 * i < len(lines) else "??"
        names[offset] = name if name != "??" else "?"
    return names


def disassemble(encodings):
    if not encodings:
        return []
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "block.bin")
        with open(path, "wb") as f:
            f.write(b"".join(struct.pack("<I", e) for e in encodings))
        result = subprocess.run([tool("objdump"), "-D", "-b", "binary", "-m", "aarch64", path], capture_output=True,
                                text=True, check=True)
    text = {}
    for line in result.stdout.splitlines():
        m = re.match(r"^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(.*)$", line)
        if m:
            text[int(m.group(1), 16) // 4] = m.group(2).strip()
    return [text.get(i, "") for i in range(len(encodings))]


def annotate(lines, names):
    out = []
    in_self = False
    by_function = collections.Counter()
    total = 0
    block = []

    def flush_self():
        if not by_function:
            return
        out.append("Self samples by C++ function:")
        for name, count in by_function.most_common():
            out.append(f"  {100.0 * count / total:5.2f}%  {count:7}  {name}")

    def flush_block():
        for (prefix, encoding), text in zip(block, disassemble([int(e, 16) for _, e in block])):
            out.append(f"{prefix}{encoding}  {text}")
        block.clear()

    for line in lines:
        block_match = BLOCK_LINE.match(line)
        if block_match:
            block.append((block_match.group(1), block_match.group(2)))
            continue
        if block:
            flush_block()

        if line.startswith("Self PCs while running"):
            in_self, total = True, 0
            by_function.clear()
        elif in_self:
            m = SELF_LINE.match(line)
            if m:
                count = int(m.group(2))
                total += count
                label = MODULE_LABEL.fullmatch(m.group(3))
                if label:
                    by_function[names.get(int(label.group(1), 16), "?")] += count
            else:
                flush_self()
                in_self = False

        out.append(MODULE_LABEL.sub(lambda m: f"m:{m.group(1)}<{names.get(int(m.group(1), 16), '?')}>", line))

    if block:
        flush_block()
    if in_self:
        flush_self()
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("report")
    parser.add_argument("--elf", default="build/switch-vk/switch/app/armsx2.elf",
                        help="the armsx2.elf matching the NRO that produced the report")
    args = parser.parse_args()

    with open(args.report, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()

    offsets = {int(m.group(1), 16) for line in lines for m in MODULE_LABEL.finditer(line)}
    names = symbolize(args.elf, offsets)
    sys.stdout.write("\n".join(annotate(lines, names)) + "\n")


if __name__ == "__main__":
    main()
