#!/usr/bin/env bash
# refresh_ingame_overlay.sh
#
# Fetch the ingame_overlay upstream source at a given commit, apply all local
# .patch files from third-party/deps/common/ingame_overlay/patches/, and
# repack as ingame_overlay.tar.gz.
#
# Usage:
#   ./tools/refresh_ingame_overlay.sh [--commit SHA] [--upstream URL] [--dry-run]
#
# Options:
#   --commit SHA     upstream commit / branch / tag to check out
#                    (defaults to the commit recorded in SOURCE.txt)
#   --upstream URL   upstream git URL
#                    (defaults to https://github.com/Nemirtingas/ingame_overlay)
#   --dry-run        clone and patch but do NOT overwrite the tarball
#
# After the script completes successfully:
#   1. third-party/deps/common/ingame_overlay/ingame_overlay.tar.gz is replaced
#      with the freshly built archive.
#   2. Update SOURCE.txt if you changed the upstream commit.
#   3. Update PATCH.txt / PATCHES.txt if patches needed conflict resolution.
#   4. Commit the updated submodule branch and update the submodule pointer.
#
# Requirements: git, tar (on Linux/macOS) or a tar-compatible tool on Windows.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

COMMON_DIR="$REPO_ROOT/third-party/deps/common"
OVERLAY_DIR="$COMMON_DIR/ingame_overlay"
PATCHES_DIR="$REPO_ROOT/tools/ingame_overlay_patches"
SOURCE_TXT="$OVERLAY_DIR/SOURCE.txt"
TARBALL="$OVERLAY_DIR/ingame_overlay.tar.gz"

DEFAULT_UPSTREAM="https://github.com/Nemirtingas/ingame_overlay"
UPSTREAM_URL="$DEFAULT_UPSTREAM"
UPSTREAM_COMMIT=""
DRY_RUN=0

# ── argument parsing ──────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --commit)
            UPSTREAM_COMMIT="$2"
            shift 2
            ;;
        --upstream)
            UPSTREAM_URL="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        --help|-h)
            sed -n '/^# /s/^# \{0,1\}//p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

# ── read commit from SOURCE.txt if not supplied ───────────────────────────────────
if [[ -z "$UPSTREAM_COMMIT" ]]; then
    if [[ ! -f "$SOURCE_TXT" ]]; then
        echo "error: SOURCE.txt not found at $SOURCE_TXT" >&2
        exit 1
    fi
    # SOURCE.txt contains a line like:
    #   VERSION: https://github.com/.../tree/<SHA> (branch: ...)
    UPSTREAM_COMMIT=$(grep -oP '(?<=/tree/)[0-9a-f]{7,40}' "$SOURCE_TXT" | head -1 || true)
    if [[ -z "$UPSTREAM_COMMIT" ]]; then
        echo "error: could not parse upstream commit from SOURCE.txt" >&2
        echo "  Set one explicitly with --commit <SHA>" >&2
        exit 1
    fi
    echo "Using upstream commit from SOURCE.txt: $UPSTREAM_COMMIT"
fi

# ── sanity checks ───────────────────────────────────────────────────────────────
if ! command -v git &>/dev/null; then
    echo "error: git is required" >&2
    exit 1
fi

# ── clone upstream into a temp directory ─────────────────────────────────────────────
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo ""
echo "Cloning $UPSTREAM_URL ..."
git clone --no-checkout --filter=blob:none "$UPSTREAM_URL" "$TMP_DIR/ingame_overlay_src" 2>&1
echo ""
echo "Checking out $UPSTREAM_COMMIT ..."
git -C "$TMP_DIR/ingame_overlay_src" checkout "$UPSTREAM_COMMIT" -- 2>&1

# ── apply patches ─────────────────────────────────────────────────────────────────────────────
if [[ -d "$PATCHES_DIR" ]]; then
    shopt -s nullglob
    mapfile -t patch_files < <(ls "$PATCHES_DIR"/*.patch 2>/dev/null | sort)
    shopt -u nullglob

    if [[ ${#patch_files[@]} -eq 0 ]]; then
        echo "No .patch files found in $PATCHES_DIR — skipping patch step."
        echo "Run tools/generate_ingame_overlay_patches.sh after making manual edits"
        echo "to produce machine-applicable .patch files."
    else
        echo ""
        echo "Applying ${#patch_files[@]} patch(es) ..."
        for patch_file in "${patch_files[@]}"; do
            echo "  applying: $(basename "$patch_file")"
            git -C "$TMP_DIR/ingame_overlay_src" apply --whitespace=nowarn "$patch_file"
        done
        echo "All patches applied."
    fi
else
    echo ""
    echo "warning: patches directory not found at $PATCHES_DIR"
    echo "  Apply patches manually, then run this script again with the patched source."
fi

# ── repack as tar.gz ─────────────────────────────────────────────────────────────────────────
PACKED="$TMP_DIR/ingame_overlay.tar.gz"
echo ""
echo "Repacking ..."
tar -czf "$PACKED" -C "$TMP_DIR" ingame_overlay_src --transform 's|ingame_overlay_src|ingame_overlay|'
echo "Packed: $PACKED  ($(du -sh "$PACKED" | cut -f1))"

# ── replace tarball ─────────────────────────────────────────────────────────────────────────────
if [[ $DRY_RUN -eq 1 ]]; then
    echo ""
    echo "DRY RUN — tarball NOT written to $TARBALL"
    echo "Packed archive is at: $PACKED"
else
    cp "$PACKED" "$TARBALL"
    echo ""
    echo "Replaced: $TARBALL"
    echo ""
    echo "Next steps:"
    echo "  1. Verify the build: run premake5-deps --all-ext --all-build"
    echo "  2. Update SOURCE.txt if you changed the upstream commit."
    echo "  3. Update PATCH.txt / PATCHES.txt if patches needed adaptation."
    echo "  4. Commit and push the updated third-party/deps/common branch."
fi
