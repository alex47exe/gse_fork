# ingame_overlay local patches

These `.patch` files capture the local source changes layered on top of the
upstream `ingame_overlay` commit recorded in
`third-party/deps/common/ingame_overlay/SOURCE.txt`.

`premake5-deps.lua` reapplies them automatically after `ingame_overlay` is
extracted, and `tools/refresh_ingame_overlay.sh` uses the same patch set when
rebuilding `third-party/deps/common/ingame_overlay/ingame_overlay.tar.gz`.

Current patch files:

- `01-srgb-format-detection.patch`
- `02-fp16-texture-upload.patch`
