#!/usr/bin/env python3
"""
Print a concise memory map for a Zephyr RP2040 ELF.

Usage (inside Docker):
    python3 scripts/elf_map.py app/build/zephyr/zephyr.elf

Or via the wrapper:
    docker compose run --rm zephyr ./scripts/memory_report.sh
"""

import subprocess
import sys
import re

SHORT_USAGE = """\
usage: elf_map.py [--short] <path/to/zephyr.elf>

  --short   print only the two-line flash/RAM summary (good as a post-build step)
"""

FLASH_BASE = 0x10000000
FLASH_END  = 0x10200000   # 2 MB
RAM_BASE   = 0x20000000
RAM_END    = 0x20042000   # 264 KB


def section_sizes(elf):
    """Return list of (name, vma, size) for non-empty, non-debug sections."""
    r = subprocess.run(["readelf", "-S", "--wide", elf],
                       capture_output=True, text=True, check=True)
    sections = []
    for line in r.stdout.splitlines():
        m = re.match(
            r"\s+\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)",
            line)
        if not m:
            continue
        name, addr_s, size_s = m.groups()
        size = int(size_s, 16)
        vma  = int(addr_s, 16)
        if size == 0:
            continue
        if name.startswith(".debug") or name in (".symtab", ".strtab", ".shstrtab"):
            continue
        sections.append((name, vma, size))
    return sections


def top_symbols(elf, n=20):
    """Return the n largest symbols as (name, addr, size)."""
    r = subprocess.run(["nm", "--print-size", "--size-sort", "--reverse-sort", elf],
                       capture_output=True, text=True, check=True)
    results = []
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) < 4:
            continue
        try:
            size = int(parts[1], 16)
            name = parts[3]
            if size > 0:
                results.append((name, size))
        except ValueError:
            continue
        if len(results) >= n:
            break
    return results


def bar(used, total, width=40):
    filled = int(width * used / total) if total else 0
    return "[" + "#" * filled + "." * (width - filled) + "]"


def human(n):
    if n >= 1024 * 1024:
        return f"{n / 1024 / 1024:.1f} MB"
    if n >= 1024:
        return f"{n / 1024:.1f} KB"
    return f"{n} B"


def print_short(flash_used, flash_total, ram_used, ram_total):
    w = 30
    def sbar(used, total):
        filled = int(w * used / total) if total else 0
        return "█" * filled + "░" * (w - filled)

    free_flash = flash_total - flash_used
    free_ram   = ram_total   - ram_used
    print()
    print(f"  FLASH  {sbar(flash_used, flash_total)}  "
          f"{human(flash_used):>8} used  {human(free_flash):>8} free  "
          f"({100*flash_used/flash_total:.1f}%)")
    print(f"  RAM    {sbar(ram_used,   ram_total)}  "
          f"{human(ram_used):>8} used  {human(free_ram):>8} free  "
          f"({100*ram_used/ram_total:.1f}%)")
    print()


def main():
    args = sys.argv[1:]
    short = "--short" in args
    args  = [a for a in args if a != "--short"]

    if not args or args[0] in ("-h", "--help"):
        print(SHORT_USAGE)
        sys.exit(0 if args else 1)

    elf = args[0]

    sections = section_sizes(elf)

    flash_sections = [(n, a, s) for n, a, s in sections
                      if FLASH_BASE <= a < FLASH_END]
    ram_sections   = [(n, a, s) for n, a, s in sections
                      if RAM_BASE  <= a < RAM_END]

    flash_used = sum(s for _, _, s in flash_sections)
    ram_used   = sum(s for _, _, s in ram_sections)
    flash_total = FLASH_END - FLASH_BASE
    ram_total   = RAM_END   - RAM_BASE

    if short:
        print_short(flash_used, flash_total, ram_used, ram_total)
        return

    print()
    print("╔══════════════════════════════════════════════════════════════╗")
    print("║                      Memory Map                             ║")
    print("╚══════════════════════════════════════════════════════════════╝")

    # ── FLASH ──────────────────────────────────────────────────────────
    print()
    print(f"  FLASH  {bar(flash_used, flash_total)}  "
          f"{human(flash_used)} / {human(flash_total)}  "
          f"({100*flash_used/flash_total:.1f}%)")
    print()
    flash_sections.sort(key=lambda x: x[1])  # sort by address
    for name, addr, size in flash_sections:
        print(f"    0x{addr:08x}  {name:<28}  {size:>9,} B  ({human(size)})")

    # ── RAM ────────────────────────────────────────────────────────────
    print()
    print(f"  RAM    {bar(ram_used, ram_total)}  "
          f"{human(ram_used)} / {human(ram_total)}  "
          f"({100*ram_used/ram_total:.1f}%)")
    print()
    ram_sections.sort(key=lambda x: x[1])
    for name, addr, size in ram_sections:
        print(f"    0x{addr:08x}  {name:<28}  {size:>9,} B  ({human(size)})")

    # ── Top symbols ────────────────────────────────────────────────────
    print()
    print(f"  Top symbols by size:")
    print(f"    {'Symbol':<44}  {'Size':>9}")
    print("    " + "-" * 56)
    for name, size in top_symbols(elf, n=15):
        print(f"    {name:<44}  {size:>9,} B  ({human(size)})")

    print()


if __name__ == "__main__":
    main()
