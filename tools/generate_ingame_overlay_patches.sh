#!/usr/bin/env bash
# generate_ingame_overlay_patches.sh
#
# Generate (or regenerate) the numbered .patch files in
# tools/ingame_overlay_patches/ by diffing the current patched tarball
# against the unmodified upstream source at the commit recorded in SOURCE.txt.
#
# Run from the repository root:
#   ./tools/generate_ingame_overlay_patches.sh
#
# Requirements: git, tar
#
# After running, review the generated .patch files, then commit them.
# The premake5-deps.lua build will apply them automatically on the next
# --ext-ingame_overlay / --all-ext run.

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

# ── copy patched files into upstream tree and stage ──────────────────────────
echo "Computing diffs ..."
rsync -a --existing "$TMP_DIR/patched/include/" "$TMP_DIR/upstream/include/" 2>/dev/null || \
    cp -a "$TMP_DIR/patched/include" "$TMP_DIR/upstream/"
rsync -a --existing "$TMP_DIR/patched/src/"     "$TMP_DIR/upstream/src/"     2>/dev/null || \
    cp -a "$TMP_DIR/patched/src"     "$TMP_DIR/upstream/"
git -C "$TMP_DIR/upstream" add -A

mkdir -p "$PATCHES_DIR"

# ── patch 01: GetDXGISwapChain ─────────────────────────────────────────────────
git -C "$TMP_DIR/upstream" diff --cached -- \
    'src/Windows/DX10Hook.h' 'src/Windows/DX10Hook.cpp' \
    'src/Windows/DX11Hook.h' 'src/Windows/DX11Hook.cpp' \
    'src/Windows/DX12Hook.h' 'src/Windows/DX12Hook.cpp' \
    > "$PATCHES_DIR/01-get-dxgi-swapchain.patch" || true

# ── patch 02: sRGB format detection ────────────────────────────────────────────────
# RendererHook.h sRGB enum additions + DX11/DX12/Vulkan detection cases
git -C "$TMP_DIR/upstream" diff --cached -- \
    'include/InGameOverlay/RendererHook.h' \
    'src/Windows/VulkanHook.cpp' \
    > "$PATCHES_DIR/02-srgb-format-detection.patch" || true

# ── patch 03: FP16 texture upload ───────────────────────────────────────────────────
git -C "$TMP_DIR/upstream" diff --cached -- \
    'include/InGameOverlay/RendererResource.h' \
    'src/RendererResourceInternal.h' 'src/RendererResourceInternal.cpp' \
    'src/RendererHookInternal.h' \
    'src/Windows/DX11Hook.cpp' 'src/Windows/DX12Hook.cpp' \
    'src/Windows/VulkanHook.cpp' 'src/Windows/OpenGLHook.cpp' \
    > "$PATCHES_DIR/03-fp16-texture-upload.patch" || true

echo ""
echo "Patch files written to $PATCHES_DIR:"
ls -lh "$PATCHES_DIR"/*.patch 2>/dev/null || echo "  (none produced)"
echo ""
echo "Review the patches, then commit tools/ingame_overlay_patches/ to the PR branch."
