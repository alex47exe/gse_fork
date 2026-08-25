## 2026/08/25

* **[alex47exe]** overlay: added debug-vs-release logging configuration examples and debug-build `ingame_overlay` spdlog/log-level premake options
* **[alex47exe]** deps: added `apply_ingame_overlay_patches()` in `premake5-deps.lua` — auto-applies numbered `.patch` files from `tools/ingame_overlay_patches/` after tarball extraction
* **[alex47exe]** tools: added `tools/refresh_ingame_overlay.sh` — fetches upstream at pinned commit, applies patches, repacks tarball
* **[alex47exe]** tools: added `tools/generate_ingame_overlay_patches.sh` — diffs patched tarball vs upstream to produce numbered `.patch` files; added `tools/ingame_overlay_patches/README.md`

---

## 2026/05/17

* **[alex47exe]** overlay: per-notification-type configurable WAV sound files in `steam_settings/sounds/`; full load-time fallback chain (`<type>.wav` → `notification.wav` → silence); example WAV files and `sounds/README.md` included
* **[alex47exe]** overlay: fixed 9 notification UX issues — non-intrusive when overlay open, no input stealing, interactive buttons still clickable, reduced flickering on friend status updates
* **[alex47exe]** CI: branch/commit selection inputs threaded through all build, deps, and release workflows
* **[alex47exe]** CI: 9 per-dep caches (`ssq`, `zlib`, `mbedtls`, `curl`, `protobuf`, `ingame_overlay`, `opus`, `portaudio`, `sdl`); per-dep force-rebuild boolean inputs
* **[alex47exe]** CI: cascade rebuild conditions — `zlib`/`mbedtls` changes trigger `curl` rebuild; `zlib` changes trigger `protobuf` rebuild
* **[alex47exe]** CI: fixed bash operator-precedence bug in `needs-build` check that silently prevented cache-miss builds
* **[alex47exe]** CI: Windows and Linux release builds run in parallel
