#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OVERLAY_DIR="$REPO_ROOT/third-party/deps/common/ingame_overlay"
SOURCE_TXT="$OVERLAY_DIR/SOURCE.txt"
TARBALL="$OVERLAY_DIR/ingame_overlay.tar.gz"
PATCHES_DIR="$REPO_ROOT/tools/ingame_overlay_patches"

UPSTREAM_URL="https://github.com/Nemirtingas/ingame_overlay"
UPSTREAM_COMMIT=""
DRY_RUN=0

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
            cat <<'EOF'
Usage:
  ./tools/refresh_ingame_overlay.sh [--commit SHA] [--upstream URL] [--dry-run]

Options:
  --commit SHA     upstream commit / branch / tag to check out
  --upstream URL   upstream git URL
  --dry-run        rebuild the archive in a temp dir without replacing the tarball
EOF
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$UPSTREAM_COMMIT" ]]; then
    if [[ ! -f "$SOURCE_TXT" ]]; then
        echo "error: SOURCE.txt not found: $SOURCE_TXT" >&2
        exit 1
    fi

    UPSTREAM_COMMIT="$(grep -oE '/tree/[0-9a-f]{7,40}' "$SOURCE_TXT" | head -1 | cut -d/ -f3 || true)"
    if [[ -z "$UPSTREAM_COMMIT" ]]; then
        echo "error: could not parse upstream commit from $SOURCE_TXT" >&2
        exit 1
    fi
fi

if [[ ! -d "$PATCHES_DIR" ]]; then
    echo "error: patch directory not found: $PATCHES_DIR" >&2
    exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "Cloning $UPSTREAM_URL ..."
git clone --no-checkout --filter=blob:none "$UPSTREAM_URL" "$TMP_DIR/ingame_overlay_src" >/dev/null 2>&1

echo "Checking out $UPSTREAM_COMMIT ..."
git -C "$TMP_DIR/ingame_overlay_src" checkout "$UPSTREAM_COMMIT" >/dev/null 2>&1

mapfile -t PATCH_FILES < <(find "$PATCHES_DIR" -maxdepth 1 -type f -name '*.patch' | sort)
if [[ ${#PATCH_FILES[@]} -eq 0 ]]; then
    echo "error: no .patch files found in $PATCHES_DIR" >&2
    exit 1
fi

echo "Applying ${#PATCH_FILES[@]} patch(es) ..."
for patch_file in "${PATCH_FILES[@]}"; do
    echo "  $(basename "$patch_file")"
    git -C "$TMP_DIR/ingame_overlay_src" apply --whitespace=nowarn "$patch_file"
done

PACKED="$TMP_DIR/ingame_overlay.tar.gz"
echo "Packing tarball ..."
tar -czf "$PACKED" -C "$TMP_DIR" ingame_overlay_src --transform 's|ingame_overlay_src|ingame_overlay|'

if [[ $DRY_RUN -eq 1 ]]; then
    echo "Dry run complete: $PACKED"
    exit 0
fi

cp "$PACKED" "$TARBALL"
echo "Updated $TARBALL"
