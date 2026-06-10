#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""扫描指定目录，拦截可能误入仓库的火山/Xiaozhi 凭据字符串。

退出码：
    0 - 未发现匹配
    1 - 至少一处疑似命中
"""

from __future__ import annotations
import os
import re
import sys
from pathlib import Path

# 与 docs/PORTING.md §8.4 同步
RULES = {
    "volc_product_secret": re.compile(r"\b[a-f0-9]{24}\b"),
    "volc_bot_id":         re.compile(r"\bbot[A-Za-z0-9]{8,16}\b"),
    "volc_ak":             re.compile(r"\bAKLT[A-Za-z0-9+/=]{40,}\b"),
    "volc_appkey":         re.compile(r"\b[a-f0-9]{32}\b"),
}

EXCLUDE_DIRS = {
    ".git",
    "build",
    "managed_components",
    "node_modules",
    "upstream",
    "mooncake",
    "mooncake_log",
    "smooth_ui_toolkit",
    "xiaozhi-esp32",
}
EXCLUDE_NAMES = {"sdkconfig", "sdkconfig.old", "sdkconfig.local"}
EXCLUDE_EXT  = {".bin", ".elf", ".map", ".o", ".a", ".png", ".jpg", ".jpeg", ".webp",
                ".pem", ".key", ".glb", ".pdf", ".woff", ".woff2", ".ttf", ".otf",
                ".mp4", ".mov", ".ogg", ".wav", ".mp3"}


def iter_files(root: Path):
    for p, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        for fn in files:
            if fn in EXCLUDE_NAMES or fn.startswith("sdkconfig.local."):
                continue
            fp = Path(p) / fn
            if fp.suffix.lower() in EXCLUDE_EXT:
                continue
            yield fp


def scan_file(fp: Path) -> list[tuple[str, int, str]]:
    hits: list[tuple[str, int, str]] = []
    try:
        text = fp.read_text(errors="ignore")
    except OSError:
        return hits
    for lineno, line in enumerate(text.splitlines(), start=1):
        for tag, rx in RULES.items():
            for m in rx.finditer(line):
                hits.append((tag, lineno, m.group(0)))
    return hits


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: scan_secrets.py <root>", file=sys.stderr)
        return 2

    root = Path(sys.argv[1]).resolve()
    if not root.exists():
        print(f"error: {root} not found", file=sys.stderr)
        return 2

    total = 0
    for fp in iter_files(root):
        for tag, lineno, snippet in scan_file(fp):
            print(f"{fp}:{lineno}: [{tag}] {snippet}")
            total += 1

    if total:
        print(f"\n{total} suspect secret(s) found.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
