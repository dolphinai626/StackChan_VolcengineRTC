#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# 拉取上游依赖到 ./upstream/，由 firmware/CMakeLists.txt 通过
# EXTRA_COMPONENT_DIRS 引入。upstream/ 不进入 git。

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPOS_JSON="$ROOT/repos.json"
DEST_ROOT="$ROOT/upstream"

mkdir -p "$DEST_ROOT"

if ! command -v jq >/dev/null 2>&1; then
    echo "error: jq required (brew install jq)"; exit 1
fi

count=$(jq '.repos | length' "$REPOS_JSON")
for i in $(seq 0 $((count-1))); do
    name=$(jq -r ".repos[$i].name" "$REPOS_JSON")
    url=$( jq -r ".repos[$i].url"  "$REPOS_JSON")
    ref=$( jq -r ".repos[$i].ref"  "$REPOS_JSON")
    dest=$(jq -r ".repos[$i].dest" "$REPOS_JSON")
    target="$ROOT/$dest"

    if [[ "$url" == *"<your-org>"* ]]; then
        echo "skip $name (placeholder url; update repos.json before fetching)"
        continue
    fi

    if [[ -d "$target/.git" ]]; then
        echo "[update] $name @ $ref"
        git -C "$target" remote set-url origin "$url"
        git -C "$target" fetch --depth=1 origin "$ref"
        git -C "$target" checkout -q "$ref"
        git -C "$target" reset --hard "origin/$ref"
    else
        echo "[clone]  $name <- $url ($ref)"
        git clone --depth=1 --branch "$ref" "$url" "$target"
    fi

    # 应用本仓库维护的补丁（patches/<name>.patch）。reset --hard 会清掉上次的
    # 应用结果，所以每次 fetch 后都要重打。
    patch_file="$ROOT/patches/$name.patch"
    if [[ -f "$patch_file" ]]; then
        if git -C "$target" apply --check "$patch_file" 2>/dev/null; then
            git -C "$target" apply "$patch_file"
            echo "[patch]  $name <- patches/$name.patch"
        elif git -C "$target" apply --check --reverse "$patch_file" 2>/dev/null; then
            echo "[patch]  $name already patched, skip"
        else
            echo "error: patches/$name.patch no longer applies to $name @ $ref" >&2
            exit 1
        fi
    fi
done

# 拉完后跑一次 secret 扫描
"$ROOT/scripts/scan_secrets.py" "$DEST_ROOT" || {
    echo "WARN: secret scan reported issues; review before committing."
}

echo "done. inspect $DEST_ROOT/"
