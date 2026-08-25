#!/usr/bin/env bash
# generate_ingame_overlay_patches.sh
#
# Generate (or regenerate) the .patch files in tools/ingame_overlay_patches/
# by diffing the current patched tarball against the unmodified upstream
# source at the commit recorded in SOURCE.txt.
#
# A single combined patch file is produced:
#   tools/ingame_overlay_patches/01-gse-fork-patches.patch
#
# This file covers all three GSE-fork patch tracks:
#   - GetDXGISwapChain() virtual method (DX10/11/12 hooks)
#   - sRGB swapchain format detection (RendererHook.h, DX11/12, Vulkan)
#   - FP16 (RGBA16F) texture upload support (all backends)
#
# A single patch is required because several files (DX11Hook.cpp, DX12Hook.cpp,
# VulkanHook.cpp) carry changes from multiple tracks; splitting by file would
# produce overlapping hunks that cannot be applied independently.
#
# Run from the repository root:
#   ./tools/generate_ingame_overlay_patches.sh
#
# Requirements: git, tar
#
# After running, review the generated patch, then commit
# tools/ingame_overlay_patches/ to the PR branch.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

COMMON_DIR="$REPO_ROOT/third-party/deps/common"
OVERLAY_DIR="$COMMON_DIR/ingame_overlay"
PATCHES_DIR="$REPO_ROOT/tools/ingame_overlay_patches"
SOURCE_TXT="$OVERLAY_DIR/SOURCE.txt"
TARBALL="$OVERLAY_DIR/ingame_overlay.tar.gz"

DEFAULT_UPSTREAM="https://github.com/Nemirtingas/ingame_overlay"

# ── read upstream commit ──────────────────────────────────────────────────────
UPSTREAM_COMMIT=$(grep -oP '(?<=/tree/)[0-9a-f]{7,40}' "$SOURCE_TXT" | head -1 || true)
if [[ -z "$UPSTREAM_COMMIT" ]]; then
    echo "error: could not parse upstream commit from $SOURCE_TXT" >&2
    exit 1
fi
echo "Upstream commit: $UPSTREAM_COMMIT"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

# ── clone upstream (unpatched) ────────────────────────────────────────────────
echo "Cloning $DEFAULT_UPSTREAM ..."
git clone --no-checkout --filter=blob:none "$DEFAULT_UPSTREAM" "$TMP_DIR/upstream" 2>&1
git -C "$TMP_DIR/upstream" checkout "$UPSTREAM_COMMIT" -- 2>&1

# ── extract current (patched) tarball ─────────────────────────────────────────
echo "Extracting patched tarball ..."
mkdir -p "$TMP_DIR/patched"
tar -xzf "$TARBALL" -C "$TMP_DIR/patched" --strip-components=1

# ── overlay patched files onto upstream tree and stage ─────────────────────────
echo "Computing diff ..."
rsync -a --existing "$TMP_DIR/patched/include/" "$TMP_DIR/upstream/include/" 2>/dev/null || \
    cp -a "$TMP_DIR/patched/include" "$TMP_DIR/upstream/"
rsync -a --existing "$TMP_DIR/patched/src/"     "$TMP_DIR/upstream/src/"     2>/dev/null || \
    cp -a "$TMP_DIR/patched/src"     "$TMP_DIR/upstream/"
git -C "$TMP_DIR/upstream" add -A

mkdir -p "$PATCHES_DIR"

# ── single combined patch covering all three GSE-fork patch tracks ────────────────
# A single file is used because DX11Hook.cpp, DX12Hook.cpp and VulkanHook.cpp
# contain changes from multiple tracks; splitting would produce overlapping hunks.
git -C "$TMP_DIR/upstream" diff --cached \
    > "$PATCHES_DIR/01-gse-fork-patches.patch"

echo ""
echo "Patch file written to $PATCHES_DIR:"
ls -lh "$PATCHES_DIR"/*.patch 2>/dev/null || echo "  (none produced)"
echo ""
echo "Review the patch, then commit tools/ingame_overlay_patches/ to the PR branch."
