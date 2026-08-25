# ingame_overlay — Local Patch Files

This directory holds the machine-applicable unified-diff patches that bring
the upstream `ingame_overlay` source in line with the GSE-fork requirements.
Place generated `.patch` files here; `premake5-deps.lua` applies them
automatically after tarball extraction.

## Patch naming convention

Patches are named with a two-digit numeric prefix so they are applied in a
consistent order:

```
01-get-dxgi-swapchain.patch       # expose IDXGISwapChain* via GetDXGISwapChain()
02-srgb-format-detection.patch    # add SRGB enum values + detection logic
03-fp16-texture-upload.patch      # RGBA16F texture support for HDR overlays
```

## How patches are applied automatically

`premake5-deps.lua` calls `apply_ingame_overlay_patches()` automatically
after tarball extraction (when `--ext-ingame_overlay` or `--all-ext` is
given).  Each `.patch` file is applied via:

```
git -C <deps_dir>/ingame_overlay apply --whitespace=nowarn <patch_file>
```

Patches are sorted alphabetically before application, so the numeric prefix
guarantees the right order.

## How to generate / regenerate patch files

Use `tools/refresh_ingame_overlay.sh --dry-run` to unpack a fresh upstream
checkout, then apply the changes described in
`third-party/deps/common/ingame_overlay/PATCH.txt` and
`third-party/deps/common/ingame_overlay/PATCHES.txt` manually and run:

```bash
git -C /tmp/ingame_overlay_upstream diff > tools/ingame_overlay_patches/01-get-dxgi-swapchain.patch
```

Alternatively, use `tools/generate_ingame_overlay_patches.sh` (once the
current tarball is patched) to diff the upstream tree against the extracted
tarball and split the result into numbered patch files.

## What the patches change

| # | File | Description |
|---|------|-------------|
| 01 | `include/InGameOverlay/RendererHook.h`, `src/Windows/DX1[012]Hook.*` | Add `GetDXGISwapChain()` virtual method so callers can query `IDXGISwapChain3` for colour-space info |
| 02 | `include/InGameOverlay/RendererHook.h`, `src/Windows/DX11Hook.cpp`, `src/Windows/DX12Hook.cpp`, `src/Windows/VulkanHook.cpp` | Add `R8G8B8A8_SRGB`, `B8G8R8A8_SRGB`, `B8G8R8X8_SRGB` enum values and swapchain detection logic |
| 03 | `include/InGameOverlay/RendererResource.h`, `src/Renderer*.{h,cpp}`, `src/Windows/*.cpp` | Add `RendererPixelFormat` enum + `AttachResource(…, format)` overload for FP16 (RGBA16F) HDR texture uploads |

Full prose descriptions are in `third-party/deps/common/ingame_overlay/PATCH.txt`
and `PATCHES.txt`.
