#!/usr/bin/env python3
"""SkunkCrafts Updater control-file generator."""
import argparse, fnmatch, os, zlib
from pathlib import Path

# Paths (relative to the plugin root) the updater should NOT manage. This
# project ships only the platform .xpl binaries, so the only things to skip are
# the generator's own outputs and macOS clutter.
IGNORE_GLOBS = [
    ".DS_Store", "**/.DS_Store",
    "skunkcrafts_updater_*.txt",
    "skunkcrafts_updater.cfg",
]
# Load-only-if-missing files (user-editable configs). None in this project.
ONCE_GLOBS = []

def crc32(fp: Path) -> int:
    c = 0
    with fp.open("rb") as f:
        while chunk := f.read(65536):
            c = zlib.crc32(chunk, c)
    return c & 0xFFFFFFFF

def matches(rel, globs): return any(fnmatch.fnmatch(rel, g) for g in globs)

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", required=True)
    ap.add_argument("--version", required=True)
    a = ap.parse_args()
    tree = Path(a.tree).resolve()
    template = (Path(__file__).parent / "skunkcrafts_updater.cfg.template").read_text()

    whitelist, sizes, once = [], [], []
    for dp, _d, files in os.walk(tree):
        for name in files:
            ap_ = Path(dp) / name
            rel = ap_.relative_to(tree).as_posix()
            if matches(rel, IGNORE_GLOBS):
                continue
            whitelist.append(f"{rel}|{crc32(ap_)}")
            sizes.append(f"{rel}|{ap_.stat().st_size}")
            if matches(rel, ONCE_GLOBS):
                once.append(rel)
    whitelist.sort(); sizes.sort(); once.sort()
    (tree / "skunkcrafts_updater_whitelist.txt").write_text("\n".join(whitelist) + "\n")
    (tree / "skunkcrafts_updater_sizeslist.txt").write_text("\n".join(sizes) + "\n")
    (tree / "skunkcrafts_updater_oncelist.txt").write_text("\n".join(once) + "\n")
    (tree / "skunkcrafts_updater.cfg").write_text(template.replace("@VERSION@", a.version))
    print(f"tracked {len(whitelist)} files, {len(once)} once-only -> {tree}")

if __name__ == "__main__":
    main()
