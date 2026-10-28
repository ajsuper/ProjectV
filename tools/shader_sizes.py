#!/usr/bin/env python3
"""Every compiled shader's size, for docs/plans/shader-library.md D8: a shader's size costs speed.

    tools/shader_sizes.py                        the table, Markdown, largest first
    tools/shader_sizes.py --save sizes.tsv       also write it as path<TAB>bytes
    tools/shader_sizes.py --against sizes.tsv    with each shader's change since that file

Reads what a build left behind: each renderer's .bin beside its source (projv_compile_shaders writes
them there) and the engine's embedded shaders (build/generated/engine_shaders/*.h).
"""
import argparse
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent


def compiled_shaders(root, build):
    sizes = {}
    for binary in sorted((root / "examples").rglob("*.bin")):
        # Only shaders: a .bin with its source beside it (scene data uses .bin too).
        sources = [binary.with_suffix(".frag"), binary.with_suffix(".sc")]
        if any(s.exists() for s in sources):
            sizes[str(binary.relative_to(root))] = binary.stat().st_size
    for header in sorted((build / "generated" / "engine_shaders").glob("*.h")):
        match = re.search(r"\[(\d+)\]", header.read_text(errors="replace"))
        if match:
            sizes["engine/" + header.stem] = int(match.group(1))
    return sizes


def read_tsv(path):
    sizes = {}
    for line in pathlib.Path(path).read_text().splitlines():
        if "\t" in line:
            name, value = line.split("\t", 1)
            sizes[name] = int(value)
    return sizes


def kb(n):
    return f"{n / 1024:.1f}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=str(ROOT), help="the source tree (default: this one; another worktree for a baseline)")
    parser.add_argument("--build", default=str(ROOT / "build"), help="the build tree (default: build/)")
    parser.add_argument("--save", help="write path<TAB>bytes to this file")
    parser.add_argument("--against", help="compare with a file written by --save")
    args = parser.parse_args()

    sizes = compiled_shaders(pathlib.Path(args.root), pathlib.Path(args.build))
    if args.save:
        pathlib.Path(args.save).write_text("".join(f"{k}\t{v}\n" for k, v in sorted(sizes.items())))

    before = read_tsv(args.against) if args.against else None
    names = sorted(set(sizes) | set(before or {}), key=lambda k: -max(sizes.get(k, 0), (before or {}).get(k, 0)))
    if before is None:
        print("| Shader | kB |\n|---|---:|")
        for name in names:
            print(f"| `{name}` | {kb(sizes[name])} |")
        print(f"| **total** ({len(sizes)}) | {kb(sum(sizes.values()))} |")
        return

    print("| Shader | before kB | after kB | change |\n|---|---:|---:|---:|")
    for name in names:
        a, b = before.get(name), sizes.get(name)
        if a == b:
            continue
        change = "new" if a is None else "gone" if b is None else f"{(b - a) / a * 100:+.1f}%"
        print(f"| `{name}` | {kb(a) if a else '-'} | {kb(b) if b else '-'} | {change} |")
    total_a, total_b = sum(before.values()), sum(sizes.values())
    unchanged = sum(1 for n in names if before.get(n) == sizes.get(n))
    print(f"| **total** ({unchanged} unchanged) | {kb(total_a)} | {kb(total_b)} | "
          f"{(total_b - total_a) / max(total_a, 1) * 100:+.1f}% |")


if __name__ == "__main__":
    main()
