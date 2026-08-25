## 2026/08/25

* **[alex47exe]** deps: updated `ingame_overlay` upstream baseline from `0f9305c10e572b51d17447c5e4d2872337b13ab5` to `eb0638034d29781f5e6678c4273546c9395a7639`; documented reapplication flow for local sRGB/FP16/DXGI patches
* **[alex47exe]** overlay: added debug-vs-release logging configuration examples and debug-build `ingame_overlay` spdlog/log-level premake options

---

## 2026/05/17

* **[alex47exe]** overlay: per-notification-type configurable WAV sound files in `steam_settings/sounds/`; full load-time fallback chain (`<type>.wav` → `notification.wav` → silence); example WAV files and `sounds/README.md` included
* **[alex47exe]** overlay: fixed 9 notification UX issues — non-intrusive when overlay open, no input stealing, interactive buttons still clickable, reduced flickering on friend status updates
* **[alex47exe]** CI: branch/commit selection inputs threaded through all build, deps, and release workflows
* **[alex47exe]** CI: 9 per-dep caches (`ssq`, `zlib`, `mbedtls`, `curl`, `protobuf`, `ingame_overlay`, `opus`, `portaudio`, `sdl`); per-dep force-rebuild boolean inputs
* **[alex47exe]** CI: cascade rebuild conditions — `zlib`/`mbedtls` changes trigger `curl` rebuild; `zlib` changes trigger `protobuf` rebuild
* **[alex47exe]** CI: fixed bash operator-precedence bug in `needs-build` check that silently prevented cache-miss builds
* **[alex47exe]** CI: Windows and Linux release builds run in parallel
* **[alex47exe]** build: split portaudio/SDL cmake flags by OS in deps build

---

## 2026/05/16

* **[NicknineTheEagle]** fixes for old interfaces
* **[onthebed]** fix gamepad: stop polling HID devices every callback, reducing CPU usage
* **[Detanup01]** update build toolchain to Visual Studio 2026
* **[Detanup01]** fix appid detection

---

## 2026/04/25

* **[NicknineTheEagle]** update old interface function signatures
* **[NicknineTheEagle]** `SteamGameServer_Init` old-version support + callback fix
* **[NicknineTheEagle]** fixes for Source query handler (store version name, protocol version, merge mod/game dir fields)
* **[universal963]** fail correctly if `SteamAPI_ManualDispatch_FreeLastCallback()` is not called before `SteamAPI_ManualDispatch_GetNextCallback()`
* **[NicknineTheEagle]** update pipe-index handling + fix interface generator for newer DLLs; bind flags on pipes
* **[Rustbeard86]** fix `SteamNetworkingSockets` lane propagation and `SendMessages` path for Enshrouded P2P

---

## 2026/04/17

* **[alex47exe]** overlay: live brightness/contrast/gamma adjustment sliders in the native overlay; dedicated sliders in the ReShade addon panel
* **[alex47exe]** overlay: debounced slider updates to avoid per-frame texture cache flush; native texture cache invalidation on adjustment change
* **[alex47exe]** overlay: FP16 HDR texture uploads with complete inline colour wrapping; colour transforms apply to images only (not UI/text)
* **[alex47exe]** overlay: R10G10B10A2 PQ vs SDR detection fix; linear HDR tone-mapping path for SDR displays

---

## 2026/04/15

* **[alex47exe]** overlay: full HDR/SDR color-space detection for native and ReShade addon — FP16, R10G10B10A2 PQ, SDR linear, and sRGB swap chains; sRGB image gamma correction
* **[alex47exe]** overlay: renderer/display info row added to the info panel with rich format strings and per-display HDR details
* **[alex47exe]** ReShade addon: D3D9 alt-tab crash fixed; coldloader bridge discovery added
* **[alex47exe]** build: premake5 workspace renamed from `gbe` to `gse`; all `.sln`/`.slnx` references updated
* **[alex47exe]** build: fixed zlib lib name for v1.3.2 (`zlibstatic` → `zs`); disabled `CURL_CA_FALLBACK` (only compatible with OpenSSL); fixed portaudio platform-suffixed lib names (`portaudio_static_x86`/`_x64`)
* **[alex47exe]** build: updated all third-party deps to latest (libssq MSVC build fix; ingame_overlay OpenGL-hook hang fix); updated abseil library list for abseil `20250512.1`

---

## 2026/04/14

* **[alex47exe]** overlay: optional notification disabling when ReShade or Special K is detected (configurable per-tool)
* **[alex47exe]** overlay: expanded proxy DLL scan list from 13 to 30 entries; ASI Loader detection added

---

## 2026/04/13

* **[alex47exe]** overlay: comprehensive third-party tool/overlay detection — Special K, ReShade, RTSS, OBS, and more; categorised by type
* **[alex47exe]** overlay: per-process renderer detection appended to process tree dump; process tree dump with command-line capture (Windows + Linux)
* **[alex47exe]** overlay: renderer proxy DLL detection with full paths; Vulkan layer enumeration included
* **[alex47exe]** emu: auto-inject Special K via SKIF on game launch (opt-in); `specialk_service_duration` config for games with launchers; skip SKIF service start if already running
* **[alex47exe]** emu: graceful unknown interface handling when Special K or other injectors are active; `exit_on_unknown_interface` config option
* **[alex47exe]** emu: enriched missing-interface diagnostic reports; sanitized user profile path in `EMU_MISSING_INTERFACE.txt`
* **[alex47exe]** overlay: fully rewritten FPS/frametime stats tracking — proper ring-buffer and accurate 1%/0.1% lows; display updates every 500 ms; granular stats settings window (graphs, percentiles, timeframe)
* **[alex47exe]** overlay: "Lobby created" notification fires only when the local user is the owner; "Has Lobby"/"In Lobby" friend status shown

---

## 2026/04/12

* **[alex47exe]** overlay: lobby chat window added; lobby/server created and destroyed notifications
* **[alex47exe]** overlay: Leave Lobby button and right-click context menu entry for non-owners; owner sees member management options
* **[alex47exe]** overlay: "Invite" and "Invite All" restricted to lobby owner; friends directly joinable when they have a lobby, no invite required
* **[alex47exe]** overlay: skip friend lobby notification if the user is already in that lobby
* **[alex47exe]** overlay: IP range shown per adapter in Networks panel; adapter names with local IPs in ReShade addon
* **[alex47exe]** network: multiple IPs tracked per peer; friends shown on all adapters matching their IP; subnet matching byte-order fix
* **[alex47exe]** overlay: fix lobby/server notifications not displaying when overlay initialises late
* **[alex47exe]** overlay: fix IP/port resolution for cross-app friends; fix Linux `ifreq` macro clash
* **[alex47exe]** overlay: disable docking and hide overlay when ReShade menu opens; fix mouse cursor lost when ReShade menu is open

---

## 2026/04/11

* **[alex47exe]** overlay: Networks panel showing all local adapters with connected users grouped by subnet; detected IP for local user and friends; Copy IP button
* **[alex47exe]** overlay: friends list redesigned to 3-line layout with 32 px avatars in both overlays; notifications include avatar + 3-line friend info (invite, message, join request, kick); notifications auto-sized (min 25% width)
* **[alex47exe]** overlay: kick/remove lobby member feature with notifications to all members
* **[alex47exe]** overlay: lobby join request response system with result notifications; `GSE_NotifAppearance` bridge for ReShade config access

---

## 2026/04/10

* **[alex47exe]** overlay: updated for ImGui 1.92 API (`IMGUI_DISABLE_OBSOLETE_FUNCTIONS`); fixed struct layout mismatch crash (imgui.cpp:10583); removed manual font atlas `Build()` call
* **[alex47exe]** overlay: friends list redesigned with status groups (In Game/Online/Away/Offline) and game names; Friends window restructured with local user header and dynamic buttons
* **[alex47exe]** overlay: configurable cross-app friend messaging (`crossapp_messaging` option); lobby/invite/join actions blocked for cross-app friends
* **[alex47exe]** overlay: lobby join request notifications with Accept/Decline buttons; chat redesigned as single tabbed window with 64 px friend header
* **[alex47exe]** overlay: achievement Groups tab for SteamHunters data; alphabetical and hidden sort toggles; Simulate/Reset buttons; fake progress simulation
* **[alex47exe]** network: `[DISCONNECT-DIAG]` logging added to all disconnect code paths

---

## 2026/04/09

* **[alex47exe]** overlay: chat refactored as tabbed window with avatars (native); avatar support added to native overlay; Chat button in both native and ReShade addon overlays
* **[alex47exe]** overlay: lobby status, connect string, and launch command shown in friend detail view; notifications always rendered on top (both overlays)
* **[alex47exe]** ReShade addon: swap chain and renderer info panel; FriendUpdate proto for real-time friend data sync; translation support; achievement grouping by DLC
* **[alex47exe]** CI: fixed deps cache not being saved (removed `lookup-only` flag); skip package install in Linux deps workflow on cache hit

---

## 2026/04/08

* **[alex47exe]** ReShade addon overlay: initial implementation with C ABI bridge (`reshade_addon_overlay` build target); block game input while overlay is open; software cursor support
* **[alex47exe]** overlay: info panel showing monitor gamut, transfer function, colour range, and per-display HDR metadata; emu build commit hash and date displayed
* **[alex47exe]** overlay bridge: `enable_experimental_bridge` setting; bridge v9 late-init and notification fixes; Linux build guards added
* **[alex47exe]** CI: use `git clone` + `submodule update` to respect pinned submodule commits; quoted `EMU_BUILD_STRING`/`EMU_BUILD_DATE_STRING` premake defines; `ingame_overlay` pinned to c03a8aa6

---

## 2026/04/07

* **[alex47exe]** overlay: SCE browser organised into tabs per category; AnimatedSticker and StartupMovie item types added; larger cells with uniform size; rich per-type item info displayed
* **[alex47exe]** overlay: sRGB decode and HDR10/R10G10B10A2 detection fixes; rich renderer display format strings

---

## 2026/04/06

* **[alex47exe]** overlay: SCE bulk asset downloader with per-type progress bars; download-progress window (25% wide) and asset browser window (≥50% wide)
* **[alex47exe]** overlay: SCE browser — card grid layout matching the SCE website style; lazy thumbnail loading with GPU texture cache; background thumbnail download and full-size preview popup; click-to-preview for all asset types; Prev/Next buttons and A/D keys for background preview navigation
* **[alex47exe]** overlay: achievement global % cached to `achievements_st.json` with configurable TTL; raw Steam API JSON stored; SteamCardExchange fetch/parse/cache for all 12 item types

---

## 2026/04/05

* **[alex47exe]** overlay: achievement window completely redesigned — tabs, search bar, SteamHunters integration (Groups tab); progress bars on all stat-tracked achievements; total completion progress bar in header; Simulate/Reset debug buttons; in-progress symbol and shadow text on bar
* **[alex47exe]** overlay: fetch Steam global achievement % from Steam Web API; sort achievements by unlocked (recent first), by global %, hidden last
* **[alex47exe]** overlay: `disable_overlay_activated_callback` config option added

---

## 2026/03/28

* **[xan105]** add SteamInput controller-type override
* **[NicknineTheEagle]** SteamGameServer fixes; always trigger logoff on server shutdown; SteamID fix
* **[otavepto]** basic implementation for some mods functions from SDK 1.64
* **[otavepto]** allow changing overlay toggle keys via ini config file
* **[otavepto]** minor changes to build scripts
* **[NicknineTheEagle]** further work on Game Coordinator

---

## 2026/03/15

* **[NicknineTheEagle]** SteamInventory `items.json` fixes
* **[Detanup01]** bump to SDK 1.64
* **[Detanup01]** add check before trying to access `steam_settings` folder
* **[NicknineTheEagle]** initial work on Game Coordinator

---

## 2026/03/10

* **[NicknineTheEagle]** fire Steam2 deny callback for denied Steam2 auth requests
* **[NicknineTheEagle]** broadcast to 10 ports for peer discovery
* **[NicknineTheEagle]** organize old SDK functions
* **[NicknineTheEagle]** fully implement `Steam_GameServer::BGetUserAchievementStatus()`
* **[NicknineTheEagle]** implement `ISteamUserItems` and `ISteamGameServerItems`
* **[NicknineTheEagle]** SteamGameServer improvements; fix callbacks for pre-1.02x SDKs
* **[PaLaS0]** fix `SetConnectionPollGroup` removing connection from wrong poll group
* **[NotAndreh]** implementation of `Steam_AppTicket::GetAppOwnershipTicketData()`
* **[NicknineTheEagle]** various fixes; minimal socket fixes
* **[Detanup01]** AppTicket improvements

> ⚠️ **Breaking:** new ticket format is now **ON** by default; old ticket format `MIN_SIZE` raised to 24 — older clients cannot connect unless they are using the new ticket format

---

## 2026/02/19

* **[Detanup01]** Lobby-Connect fix & QoL improvements
* **[universal963]** restore `account_avatar_default`
* **[Rustbeard86]** add `purchased_keys.txt` support
* **[otavepto]** handle numeric achievement progress

---

## 2026/02/16

* **[alex47exe]** configs: updated example `configs.ini`
* **[alex47exe]** build: updated `ingame_overlay` dependency

---

## 2026/01/19

* **[universal963]** update to SDK 1.63
* **[GogoVang]** update `disable_achievement_progress` example in settings
* **[otavepto]** revert a breaking regression in P2P networking

---

## 2025/11/27

* **[otavepto]** add undocumented `ISteamClient` v022 and v023
* **[wunnr]** auto send invites to friends in game
* **[otavepto]** fix regression in P2P networking caused by a data race condition
* **[otavepto]** allow changing packet-sharing behavior in old P2P networking via ini config
* **[Detanup01]** fix `SteamInternal_CreateInterface` throwing

---

## 2025/11/05

* **[otavepto]** fix performance regression caused by voice chat; disable voice chat by default
* **[wunnr]** add `GseSavePath` environment variable for overriding the emu save path

---

## 2025/10/29

* **[universal963]** add `free_weekend` option for `Steam_Apps`
* **[otavepto]** new functionality to create cloud-save directories at startup
* **[otavepto]** invalidate context-init counter after any call to init/shutdown, fixing double-init/double-shutdown bugs

---

## 2025/09/13

* **[otavepto]** rewrite P2P networking code to allow sharing the connection pool between the gameserver and client
* **[NotAndreh]** fix playtime tick not being consistently called
* **[universal963]** fix stubs for `FilterText()` and `InitFilterText()`

---

## 2025/09/07

* **[alex47exe]** configs: updated docs to recommend gen_emu_cfg from gse_fork_tools for complete, auto-generated configurations

---

## 2025/08/29

* **[NotAndreh]** persistent playtime — playtime is now saved across sessions
* **[otavepto]** experimental emulation of old `Steam.dll` library
* fix `GetWebApiTicket`

---

## 2025/08/15

> ⚠️ **Breaking:** new ticket format is now **ON** by default; old ticket format `MIN_SIZE` raised to 24

* **[otavepto]** support more stub variants
* **[otavepto]** minor updates to build scripts
* **[otavepto]** fix old `Steam_Apps::FillProofOfPurchaseKey()`
* **[otavepto]** cold client loader: auto detect `steam_appid.txt` if AppId is empty
* **[suprovsky / notgitgit]** fix `SteamUser023` interface issue for `steamclient_experimental`
* **[NicknineTheEagle]** added legacy interfaces
* **[NicknineTheEagle]** revert `pid` registry value in the loader
* **[otavepto]** update steamclient stub return code + enforce `cdecl` calling convention
* **[otavepto]** inflate size of `gameoverlayrenderer` stub to avoid basic size detection
* **[otavepto]** add missing `__wrap_xxx` exports for Linux build
* **[otavepto]** avoid sending chat entry ID = 0 in `GetLobbyChatEntry()`
* **[otavepto]** fix return code for `CreateInterface()`
* **[NicknineTheEagle]** auth manager changes
* **[otavepto]** minor updates to steamclient loader script for Linux
* **[otavepto]** minor fix for Linux experimental build `.so` loading
* **[Detanup01]** more fixes on voice chat

> Voice chat is experimental; works mainly on Windows at this time

---

## 2025/07/22

* **[alex47exe]** emu: fix `SteamUser023` interface issue for `steamclient_experimental`
* **[alex47exe]** build: add exit code to `rebuild_win.bat`

---

## 2025/07/20

* **[GogoVang]** update and rename `stats.EXAMPLE.txt` to `stats.EXAMPLE.json`
* **[GogoVang]** `migrate_gse`: support for new `stats.json` format
* **[universal963]** allow returning an empty path in `Steam_Apps::GetAppInstallDir()`
* **[universal963]** various fixes to `Steam_UGC` functions
* **[universal963]** fix missing callback and result in `Steam_Networking_Sockets`
* **[universal963]** implement `Steam_User_Stats::GetUserStat()`
* **[NicknineTheEagle]** fix old `SteamGameServer` interfaces
* **[NicknineTheEagle]** added `.gitattributes`
* **[notgitgit]** add ticket functionality and encrypted savegames
* **[NicknineTheEagle]** implement `steamclient.dll` C exports
* **[universal963]** update deps in libs folder
* **[NicknineTheEagle]** various bug fixes; clamp callback result output instead of failing
* **[Detanup01]** voice chat implementation; fix `VoiceSystem` initialization; new deps premake file
* **[Edremon]** fix various building issues

---

## 2025/04/20

* minimum Ubuntu version for builds raised to 22.04

---

## 2025/04/19

* **[alex47exe]** build: fix Linux build

---

## 2025/04/18

> ⚠️ **Breaking:** `stats.txt` is replaced by `stats.json`

* **[Zekiu]** update Polish translation for overlay
* **[universal963]** fix `GetItemState()`
* **[universal963]** initial implementation of `GetGlobalStat()`
* **[ugurkahriman]** updated Turkish translation

---

## 2025/03/27

* **[universal963]** update to SDK 1.62

---

## 2025/03/13

* **[universal963]** patches to `steam_overlay.cpp`
* **[rcyggdra]** update `steam_overlay_translations.h`
* **[universal963]** attempt to fix Simplified Chinese character display in overlay
* **[mlabalabala]** fix missing `break;` statement
* **[rcyggdra]** add FPS cap option control for overlay

---

## 2025/02/15

* **[alex47exe]** gen_emu_cfg: moved to a dedicated repository (gse_fork_tools)

---

## 2025/02/08

* **[Bleibeidl]** mark optional steps in mods setup as such
* **[universal963]** update JSON format for `default_items.json`
* **[Detanup01]** achievement images fallback when the image file is missing

---

## 2025/01/09

* **[universal963]** update Simplified Chinese translation for overlay
* **[otavepto]** fix detection of broken bind
* **[otavepto]** attempt to fix some memory leaks
* **[otavepto]** `loadlib` fix for Linux
* **[universal963]** implement page logic for all UGC query APIs

> From this release onwards, builds are no longer marked experimental

---

## 2024/12/08

* **[alex47exe]** configs: updated default `configs.overlay.ini` and `mods.EXAMPLE.json`
* **[alex47exe]** configs: renamed `my_preview_image.jpg` to `preview.jpg`

---

## 2024/12/07 (experimental)

* **[otavepto]** minor updates
* **[Clone5030]** update `installed_app_ids.EXAMPLE.txt`
* **[otavepto]** fix for accessing mod details struct + querying tags count
* **[Edremon]** implement `GetUGCDetails()`
* **[Edremon]** fix broken emulator when `libsteam_api.so` is symlinked
* **[Edremon]** fix `total_files_sizes` fallback
* **[otavepto]** overlay: FPS/frametime/playtime display
* **[otavepto]** add some old interfaces
* **[otavepto]** fix gamestats interface; write to JSON instead of CSV
* **[otavepto]** handle local files via protocol `file:/` in HTTP
* **[otavepto]** implement some functions

---

## 2024/11/24 (experimental)

* **[otavepto]** support a bind variant from 2014
* **[Edremon]** fix building on Linux; link Linux experimental build with X11
* **[Anadius]** access duplicated keys in VDF correctly in `parse_controller_vdf.py`
* updated overlay and some dependencies
* added SDK 1.61

---

## 2024/11/11

* **[alex47exe]** gen_emu_cfg: integrated `appid_finder` — fixed VDF duplicate-key parsing, added missing `beautifulsoup4`/`lxml` dependencies
* **[alex47exe]** gen_emu_cfg: fix duplicate steam interfaces for RUNE ini

---

## 2024/11/09

* **[universal963]** implement `ISteamUGC019`
* **[otavepto]** implement `ISteamMasterServerUpdater` as a proxy for `ISteamGameServer`
* updated `ingame_overlay` dependency

---

## 2024/11/05

* **[universal963]** initial implementation of `ISteamFriends001` to `ISteamFriends002`
* **[otavepto]** support older version of mod details struct; add missing callback/call-result for failure
* **[otavepto]** trigger callback + call-result for failure in `SendQueryUGCRequest()`
* **[detiam]** fix credentials in `generate_emu_config`

---

## 2024/10/25

* **[universal963]** initial implementation of `ISteamUser004` to `ISteamUser008`
* **[Detanup01]** `ISteamUtils001` implementation

---

## 2024/10/21

* **[alex47exe]** gen_emu_cfg: fix `-rel` argument

---

## 2024/10/20

* **[otavepto]** implement `ISteamAppDisableUpdate001`
* **[universal963]** fix possible memory leaks in `network.cpp`

---

## 2024/10/14

* **[alex47exe]** emu: `is_beta_branch` now correctly parsed from `app::general` instead of `main::general`
* **[alex47exe]** gen_emu_cfg: fix login (thanks to Sak32009); copy `_DEFAULT` folders on Linux; replace `%outdir%` with `$outdir` in Linux build scripts; update `top_owners_ids.txt`
* **[alex47exe]** gen_emu_cfg: fix controller config errors for appid 427520 and 1477940
* **[alex47exe]** gen_emu_cfg: fix steam ID for generated codex ini
* **[alex47exe]** CI: release workflow improvements

---

## 2024/10/06

* **[universal963]** correct undocumented API in `ISteamNetworkingSockets010` and `ISteamNetworkingSockets011`
* **[otavepto]** support older bind variants + auto unload on success or timeout
* add more interfaces to `generate_interfaces`

---

## 2024/09/15

* **[M4RCK5]** lobby connect improvements + MBTL fix
* **[Detanup01]** update stubdrm

---

## 2024/09/06

* **[otavepto]** implementation for some missing client functions
* **[otavepto]** dramatically decrease startup locking/halt time when overlay is enabled
* **[Edremon]** fix building with wine-wrapped MSVC
* **[Sak32009]** update third-party build/win; fix MSBuild warnings; improve `generate_interfaces.cpp`
* **[Sak32009]** varied fixes and improvements; fix MSBuild warnings
* **[universal963]** more accurate API behaviors fix
* **[Sak32009]** compile `stb_image_resize2` as a static library
* **[Detanup01]** update CI runner versions

---

## 2024/08/20

* **[otavepto]** add some missing implementations
* **[otavepto]** implement `Steam_User_Stats::GetAchievementIcon()`
* **[otavepto]** update `migrate_gse` to write branch info in the correct file
* **[otavepto]** fixes for `Steam_Http` class
* **[Sak32009]** update third-party and libs deps; improve `package_win_release.bat` and `build_win_premake.bat`; add `generate_credits.bat`
* **[otavepto]** allow saving stats from `ISteamGameStats` to CSV files

---

## 2024/08/17

* **[otavepto]** random fixes/changes; fixes for some crashes + behavior enhancements
* **[otavepto]** allow disabling the internal achievement-progress reporting for stats tied to achievements
* **[DogancanYr]** README update

---

## 2024/08/03

* **[alex47exe]** gse_acw_helper: switched archive format to `.zip`
* **[alex47exe]** gse_debug_switch: added missing `steam_api64`/`steamclient64` entries
* **[alex47exe]** CI: fix compatibility with upstream fork, re-enabling auto-merge

---

## 2024/07/29

* **[alex47exe]** major overhaul of `generate_emu_config` - custom configs, proper ini parsing, better logging and error handling, helper tools:

    * add `-def1` ... `-def5` arguments, which can be used to generate your preferred custom config
        if no `-def` argument is provided, `-def1` will be used by default, to automatically copy from the following folders:
        * `./_DEFAULT/0` ............... essential emu files, like latest GSE dlls (`steam_api.dll` and `steam_api64.dll`)
        * `./_DEFAULT/1` ............... other GSE files and folders, including default ini files
        * `./_DEFAULT/<appid>` ... other GSE files and folders, but only for the current `<appid>`, if the folder exists
    * (Windows only) add some useful helper tools, written in `AutoIt3`:

        * `gse_acw_helper.exe` - add the required achievements schema db files for `Achievement Watcher`, if `./steam_misc/extra_acw/extra_acw.zip` file exists (if generated by `generate_emu_config.exe -acw <appid>`)
        * `gse_debug_switch.exe` - automatically switch between release and debug versions of the emulator, if `steam_api.7z` / `steam_api64.7z` file exists (or `steamclient.7z` / `steamclient64.7z`, if you use the steamclient version)
          paths to release and debug files inside 7z, can be customized in `./steam_misc/tools/au3/scripts/gse_debug_switch.ini`
        * `gse_generate_interfaces.exe` - simple x64-x86 launcher for `generate_interfaces.exe`
          it also writes all found steam interfaces to CODEX `steam_emu.ini` (if generated by `generate_emu_config -cdx <appid>`)
          make sure to name your original dll to one of these formats, so it can automatically find its interfaces:
          * `valve_api.dll / valve_api64.dll`
          * `steam_api.dll.bak / steam_api64.dll.bak` or `steam_api.dll.org / steam_api64.dll.org`
          * `steam_api.bak / steam_api64.bak` or `steam_api.org / steam_api64.org`
          * `steam_api_orig.dll / steam_api64_orig.dll` or `steam_api_legit.dll / steam_api64_legit.dll`
        * `gse_lobby_connect.exe` - simple x64-x86 launcher for `lobby_connect.exe`
    * new folder structure, compatible with current and future helper tools --- default arguments are `-acw -cdx -clr <appid>`
      NEVER delete `./steam_misc/app_backup`, `./steam_misc/app_info`, `./steam_misc/tools` and `./steam_settings` folders
      MIGHT need `./steam_misc/extra_acw` and `./steam_misc/extra_cdx` for compatibility with Achievement Watcher and CODEX
    * add `-scx` argument to automatically download images / videos for trading cards, backgrounds, badges, emoticons and other tradable items
        unfortunately I couldn't find any direct steam api method to download the files, so I had to write a rudimentary web scrapper to extract the download links from a third-party website, hence the `scx_gen.py` script might need updating in the future if the website design changes
    * download screenshots and videos:

        * download thumbnails for both screenshots and videos, and compress them to `.zip` files
        * screenshots and videos are now numbered from first to last published, as in the Steam store page
        * add `-vids_low` / `-vids_max` arguments to download all videos, in low and / or high quality
    * create / update `./top_owners_ids.txt` when `./top_owners_ids.html` is present
    * generate controller action sets txt files for all found controller vdf configs, and zip them inside `./steam_misc/app_backup/app_backup.zip`
        by default, the emu supports only `xboxone` and `xbox360` controller configs, though if there are any issues with the default supported controller action sets inside `./steam_settings/controller/` folder, you could try to unpack and overwrite action sets for other unsupported controller configs
    * (Windows only) add `AdvancedRun` launchers (cmd console + silent) for `.bat` files and `.py` scripts
* **[alex47exe]** major overhaul of `migrate_gse` - uses the same `./_DEFAULT/0` and `./_DEFAULT/1` folder structure for default configs
  it can convert old `.txt` format to `.ini` format, minus `branches.json`, which would require using `top_owners_ids.txt` and some login code from `generate_emu_config`, which should actually be used to properly generate the config files, instead of converting from the old `.txt` format
* **[alex47exe]** `generate_interfaces.exe` - find all Steam Interfaces instead of only old ones
the emu will ignore the ones it doesn't require, while we'll have the complete list to write it to CODEX `steam_emu.ini`
* **[alex47exe]** `lobby_connect.exe` - improve cmd console text alignment
* **[alex47exe]** `mods_img` instead of `mod_images` (better folder consistency), better example for `mods_img`, minor tweaks to `.ini` and `.md` files

---

## 2024/07/28

* **[Detanup01]** fix GetISteamGenericInterface() when asking for Interface `STEAMTIMELINE_INTERFACE_V001`
* **[KGHTW]** fix Steam Datagram Error

---

## 2024/07/07

* **[Detanup01]** implement SDK v1.60, also thanks to **[universal963]** for the help in testing some functions
* **[Detanup01]** add implementation for new interfaces:
  - `ISteamUGC020`
  - `ISteamVideo007`
* **[qingchunnh]** update Chinese translations for the overlay
* update the tool `generate_emu_config` to generate a new file `steam_settings/branches.json` which contain all info about the branches of the game, needed by SDK v1.60
  this json file could be put inside the global settings folder, but **not recommended**, it is meant to be generated per-game basis
* deprecate the setting `build_id` in `configs.app.ini`, the user selected branch (`branch_name`) is now used to grab the build id of that branch from `branches.json`
  if no `branch_name` is specified, the emu will use the default branch called `public`
  if `branches.json` is missing the `public` branch, the emu will force add it in memory with a default build id = 10
* add new properties to `mods.json`:
  - `min_game_branch`
  - `max_game_branch`
  - `total_files_sizes`

  unclear how they're used for now, but they're introduced in SDK v1.60
* add a somewhat useless/stub implementation for the new interface `ISteamTimeline001` (introduced in SDK v1.60), could be extended later to interact with the built-in overlay or save the info to disk for external applications to listen to events
* add a new option `allow_unknown_stats` in `configs.main.ini` to allow games to change unknown stats
  the emu by default rejects any changes to a stat not mentioned inside `steam_settings/stats.txt`, this option allows these changes
* add a new option `save_only_higher_stat_achievement_progress` in `configs.main.ini` and enable it by default
  this option will prevent the emu from updating the progress of any achievement due to a stat change/update, if the new value is less than or equal the current one - this solves an overlay spam problem and avoids *some* useless disk write operations

  unfortunately some games abuse stats and update them a lot during gameplay with useless and disposable values, this will cause a lot of disk write operations and cannot be avoided unless you remove the definition of that stat from `steam_settings/stats.txt`, or avoiding that definition file altogether and forget about stats
* fix conditions for app/DLCs ownership APIs:
  - `Steam_Apps::BIsSubscribedApp()`
  - `Steam_Apps::BIsDlcInstalled()`
  - `Steam_Apps::BIsAppInstalled()`

  allowing more games/apps to work
* fix a problem when searching for the user selected language during the initialization of the overlay, leading to invalid language selection in some cases
* fix a potential problem when searching for the user selected language in the achievements schema, which might lead to invalid language selection in some cases
* fix a problem with `utf-8` path handling in ColdClientLoader for Windows, also fix more `utf-8` related problems in the emu and use the library `utfcpp` to convert `utf8` <---> `utf16` strings instead of using Win32 APIs
* in `Steam_User_Stats::ResetAllStats()` reset the achievement `progress` only if it was defined in the original schema in `steam_settings/achievements.json`
* implement a new debug logger to fix a problem where the debug log wasn't being generated when the path contained non-Latin characters.
* re-wrote the code of ColdClientLoader for Windows to use the library `SimpleIni` like the emu instead of relying on Win32 APIs, much easier and intuitive
* save achievement progress/max progress as `uint32` instead of `float`

---

## 2024/06/21

* fix the conditions for achievement progress indication when a game updates a stat which is tied to an achievement
  now the user achievements will be updated and saved, and an overlay notification will be triggered
  works with **[Achievement Watcher by xan105](https://github.com/xan105/Achievement-Watcher)** and the built-in overlay
  you need `stats.txt` and `achievements.json` inside your local `steam_settings` folder for this feature to work properly
* fix an old problem where games would crash on exit if the overlay was enabled, more prominent in `DirectX 12` games, also set the overlay hook procedure to an empty function before cleaning up the overlay
* remove an invalid condition when resetting stats, only write to disk and share values with any gameserver if the stat value isn't already the default
* add a try-catch block when initializing `current progress` and `max progress` during user achievements construction since they throw exception for achievements without progress
* always trigger `UserStatsStored_t` and `UserAchievementStored_t` callbacks in `Steam_User_Stats::IndicateAchievementProgress()` even if the value wasn't updated, games my halt otherwise
* return false in `Steam_User_Stats::GetAchievementProgressLimits()` if the achievement has no progress
* remove invalid code in the overlay which made it ignore the background transparency/alpha set by the user (`Background_A=0.55`) in `configs.overlay.ini`, also fixed some internal defaults
* fix a mistake in the overlay where the achievement description wasn't being set before posting the notification, the notification message/string is needed to calculate the height dynamically
* remove an irritating transparency effect in the overlay which was added to all popup windows (settings, achievements list, etc...) making the text blended with the game's scene and unclear
* allow specifying various notifications durations for the overlay, these are the new values in `configs.overlay.ini`
  ```ini
  # duration of achievement progress indication
  Notification_Duration_Progress=6.0
  # duration of achievement unlocked
  Notification_Duration_Achievement=6.0
  # duration of friend invitation
  Notification_Duration_Invitation=8.0
  # duration of chat message
  Notification_Duration_Chat=4.0
  ```
  you can set these values in the global settings, just like all the other settings in all `.ini` files
  you can also override them per-game by modifying your local `steam_settings/configs.overlay.ini`
* for Windows `ColdClientLoader`: if the file `load_order.txt` is used, then only load the files mentioned with their respective order, otherwise load all valid PE files as usual
  this allows placing PE files (beside your target .dll) that are not supposed to be loaded early, or handled later by your .dll
* avoid setting an early/initial window size and position for the overlay, only at the relevant place (slight optimization)
* replace the overlay example font (in `steam_settings.EXAMPLE/fonts.EXAMPLE/`) with Google's `Roboto` medium, the built-in one is still the same!
* in the overlay show the warning for bad appid only once until the user closes this warning

---

## 2024/06/17

* **[Detanup01]** add more missing interfaces: `ISteamVideo`, `ISteamGameStats`
* upgrade python runtime used by the scripts (`generate_emu_config` and `migrate_gse`) to v3.12
  due to recent problems with SSL library (`libsssl`), also switched to `requests` vs `urllib`
  solving a problem when grabbing achievements icons
* wrap `prtotoc` generated files and external libraries headers to suppress compilation warnings,
  also refactored the structure a little

---

## 2024/06/12

* **[Detanup01]** add `premake` build scripts, allowing the project to be built with different toolsets with ease on different platforms
  for example the project could be built with `Visual Studio` on Windows, or via the `make` tool on Linux
* **[schmurger]** add progress bar to the achievements in the overlay, only for achievements that are not earned yet
  also implement notifications for these progress indications (whenever the game indicates a new progress)
  you can disable the achievement progress notifications via `disable_achievement_progress` inside `configs.overlay.ini`
* **[schmurger]** implement the function `Steam_User_Stats::GetAchievementProgressLimits()`
* **[Detanup01]** add missing interfaces `ISteamScreenshot` `v001` and `v002`
  also fix lots of build warnings in Visual Studio
* third-party dependencies could now be built with a `premake` script, offering more flexibility
  for example you can choose to extract or build certain libraries only, you can also build 32-bit or 64-bit separately

  ---

  **check the updated readme and re-clone the repo recursively again!**

  ---

* enable controller support by default for the regular API library
* fix an old buffer overrun bug in `Steam_User_Stats::UpdateAvgRateStat()`
* fix an old bug in the shutdown functions, now they will refuse incorrect requests like the original API library, solving a crash in some games
* restore a missing export `g_pSteamClientGameServer` for the API library, removed by mistake
* avoid overriding `SteamPath` environment variable in `SteamAPI_GetSteamInstallPath()`
* fix `gameid` decoding bug in matchmaking servers when using `libssq` (source server query)
* enhance the overlay shutdown sequence, making it able to handle rapid init/shutdown sequence, fixing a crash in some games
* for Windows `ColdClientLoader`: allow loading `.ini` file with the same name as the loader
  ex: if the loader is named `game_cold_loader.exe`, then it will first try to load `game_cold_loader.ini`,
  if that doesn't exist, it will fallback to `ColdClientLoader.ini`
* add missing callback in `Steam_UGC::RequestUGCDetails()`
* re-implement the way the background thread is spawned & terminated to fix its cleanup sequence + spawn it for gameservers as well
* corrected callback vs call result in `Steam_Apps::RequestAllProofOfPurchaseKeys()`
* the emu will now terminate the process and generate a file called `EMU_MISSING_INTERFACE.txt` (beside the library) if an app requested a missing interface
* reduce binaries sizes on Linux by avoiding `-Wl,--whole-archive` and using `-Wl,--start-group -lmylib1 -lmylib2 ... -Wl,--end-group` instead on all libraries,
  allowing the linker to go back and forth between them to resolve missing symbols
* restore accidentally removed flag for ipv6, for `SteamClient020`
* make the test achievement in the overlay include a random progress
* add new button to the overlay `toggle user info` to show/hide user info, also make user info hidden by default
* make all overlay popups toggle-able, clicking its button another time will hide or show the popup, depending on its previous state
* allow `Steam_User_Stats::ClearAchievement()` to reflect the status in the overlay
* initial support for building with `MSYS2` on Windows
  **This is still highly experimental and non-functional, this is more of tech demo at the moment**
  The original SDK is created as `MSVC` library, and all games on Windows link with it
  MinGW toolchain has a completely different **ABI** and the output binary will **not work**
* deprecated and removed the special Git branches `ci-build-*`, they were intended for automation but no longer maintained

---

## 2024/05/05

* **[Clompress]** update Turkish translation
* fixed a mistake where the interface `ISteamUser022` was not added to the list of supported versions
* increase polling of the run callbacks background thread to `300 ms`
* refactored all code inside `.h/.cpp` pair, all source code in `dll/` is no longer written inside `.h` files

---

## 2024/04/30

* **[schmurger]** added a sliding animation for the overlay notifications
  the duration of the animation could be changed using the new option `Notification_Animation` in `configs.overlay.ini`
* **[Detanup01]** fixed a bug which resulted in a crash when the generated auth ticket size exceeded the max buffer size
* use `std::filesystem::u8path` to support `utf-8` paths, suggested by **[Clompress]**
  this fixes a bug where non-ascii paths were not being recognized in many places
* fixed an undesired behavior where the steam pre-owned ids were being merged with user's dlc list or installed apps list,
  this option is now disabled by default and the option `disable_steam_preowned_ids` is deprecate in favor of the new `enable_steam_preowned_ids`
* fixed a bug where sanitizing paths in the settings parser would remove the colon ':' character,
  preventing the usage of absolute paths on Windows, like: `C:\aa\bb`
* corrected the size of the auth ticket used in `Steam_User::GetAuthTicketForWebApi()`
* added 2 new options to the overlay appearance `Notification_Margin_x` and `Notification_Margin_y` which allow specifying a small gap horizontally or vertically for the notifications
* added a new switch `-revert` for the tool `migrate_gse`, which allows converting `.ini` files back to `.txt` files,
  also added some common switches for the help page `/?`, `-?`, etc...
  **note that this option isn't 100% perfect**
* updated the built-in overlay appearance & the example overlay ini file with a darker look and feel + changed some defaults, inspired by additions of **[schmurger]**

---

## 2024/04/25 (hotfix 1)

* fixed mismatching push/pop for the overlay style, resulting in a crash when the default colors are changed

---

## 2024/04/25

* **[schmurger]** improved achievement notification:
  - added new overlay appearance option `Notification_Rounding` which allows increasing the roundness of the notifications corners
  - the overlay ini file now contains color scheme similar to the one used in steam for the notification background
* added a new button to the overlay `"Test achievement"` which triggers a test achievement, suggested by **[Kirius88]**
  note that the icon for this test achievement is selected randomly from the current list of achievements
* added a new overlay appearance option `Achievement_Unlock_Datetime_Format` which allows changing the date/time format of the unlocked achievements, suggested by **[Clompress]**
* removed the condition which disabled the overlay sounds when it is shown, suggested by **[Vlxst]**
* calculate all notifications heights dynamically

---

## 2024/04/23

* fixed local saving + ignore the global settings folder entirely when using the local save option for a full portable behavior
* reverted all changes made to `find_interfaces` tool and reverted the format back to the original one, which allows loading `steam_interfaces.txt`
* fixed a bug in the `settings_parser` which lead to unwanted disk write operations in the `lobby_connect` tool
* don't use global appdata path in `matchmaking` + `matchmaking_servers`, instead use current/active save directory, in case we're using local_save_path

---

## 2024/04/21

* **[Clompress]** corrected Turkish translation
* allow changing the name of the base/global folder used to store save data, suggested by **[Clompress]**
  by default it would be the new folder `GSE Saves` (instead of `Goldberg SteamEmu Saves`)
  this could be changed only by setting the option `saves_folder_name` inside the local file `steam_settings/configs.user.ini`, the global one will not work
* new switches for the `generate_emu_config` tool, suggested by **[M4RCK5]**
  - `-skip_ach`: skip downloading & generating achievements and their images
  - `-skip_con`: skip downloading & generating controller configuration files
  - `-skip_inv`: skip downloading & generating inventory data (`items.json` & `default_items.json`)

> ⚠️ **Breaking:** move most settings inside `.ini` files:
> - `configs.main.ini`: configurations for the emu itself
> - `configs.user.ini`: configurations specific to the user
> - `configs.app.ini`: configurations specific to the game/app
> - `configs.overlay.ini`: configurations of the overlay
>
> they could be placed inside the local `steam_settings` folder,
> or inside the new global settings folder `GSE Saves/settings`, located at `%appdata%\GSE Saves\settings\` on Windows for example
> you can create a global `.ini` file `GSE Saves/settings/config.xxx.ini` for the common options, and another local one `steam_settings/config.xxx.ini` for the game-specific options, and the emu will merge them
>
> To avoid confusion, the global saves folder is changed to be `GSE Saves` by default

* new tool `migrate_gse` to convert either your global `settings` folder, or your local `steam_settings` folder from the old format to the new one
  - run the tool without arguments to let it convert the global settings folder
  - run the tool and pass the target `steam_settings` or `settings` folder as an argument to convert the structure of that folder

  in both cases, the tool will create a new folder `steam_settings` in the current directory with all the results of the conversion

  check its own dedicated readme

> ⚠️ **Breaking:** changed the environment variable `SteamAppPath` to `GseAppPath`, which is used to override the program path detected by the emu

> ⚠️ **Breaking:** removed the setting `disable_account_avatar` in favor of the new one `enable_account_avatar`, this feature is now disabled by default

* introduced a new behavior in the emu, which makes it by default add a lot of Steam builtin and preowned IDs to the DLC list, and the emu's list of installed apps
  you can disable this via the option `disable_steam_preowned_ids` in `configs.main.ini`
* added a workaround for Steam Input, set `disable_steamoverlaygameid_env_var=1` inside `configs.main.ini`, might not work though
* reverted the changes to `Steam_Apps::BIsAppInstalled()`, now it will return true when the given app id is found in the DLC list, this function is also controlled via `installed_app_ids.txt`
* removed the limit on the amount of characters for local saves
* allow specifying absolute paths for local saves
* removed the warning for using `force_xxx.txt` files from the overlay, since it's no longer relevant, also removed the code which disables the user input when this warning was displayed
* increase run callbacks background thread polling time to `~200ms`
* changed the overlay title to give proper credits to its author
* set these env vars for a more accurate emulation:
  - `SteamAppUser`
  - `SteamUser`
  - `SteamClientLaunch`
  - `SteamEnv`
  - `SteamPath`

---

## 2024/04/11 (fix 2)

* **[Clompress]** Turkish translation for the overlay
* added callbacks alongside call results in various interfaces, allowing some games to work properly
* trigger additional `UserAchievementStored_t` callbacks in `Steam_User_Stats::StoreStats()` for all the unlocked achievements prior to calling this function
* trigger `UserStatsStored_t` callback in `Steam_User_Stats::IndicateAchievementProgress()` instead of a call result (call result is removed),
  might break stuff
* trigger `UserStatsReceived_t` callback as well as call result in `Steam_User_Stats::RequestUserStats()`, needed by some games
* trigger additional `PersonaStateChange_t` callback in `Steam_Friends::SetPersonaName()`
* trigger `SteamInventoryRequestPricesResult_t` callback as well as call result in `Steam_Inventory::RequestPrices()`
* trigger `SteamUGCQueryCompleted_t` callback as well as call result in `Steam_UGC::SendQueryUGCRequest()`
* trigger callback as well as call result in many places including the following classes:
  - `Steam_User_Stats`
  - `Steam_HTTP`
  - `Steam_HTMLsurface`

---

## 2024/04/11

> ⚠️ **Breaking:** load overlay audio from `sounds` subfolder, either from the local game settings folder `steam_settings/sounds`, or from the global settings folder `Goldberg SteamEmu Settings/settings/sounds`
* allow loading the overlay fonts from the global settings folder `Goldberg SteamEmu Settings/settings/fonts`
* added missing example overlay `.wav` file
* updated readme files + added some which were missing + removed invalid avatar example

---

## 2024/04/10

* properly implement `Steam_Apps::GetAvailableGameLanguages()`
* ensure current emu language is inside `supported_languages` list
* run the callbacks background thread earlier inside `Steam_Client::ConnectToGlobalUser()`
  since some games don't call `SteamAPI_RunCallbacks()` or `SteamAPI_ManualDispatch_RunFrame()` or `Steam_BGetCallback()`
  hence all run_callbacks() will never run, also networking callbacks won't run


> ⚠️ **Breaking:** introduced a new config file `enable_experimental_overlay.txt`, which deprecates the config file `disable_overlay.txt`
> in many occasions this feature was a source of crashes, so it's better to make it an opt-in option
> otherwise, the `experimental` and `Cold Client` builds of the emu will crash by default on startup for some apps/games


* decrease the periodicity of the background thread to `~100ms`, also prevent it from running if the callbacks are already running
* output each function name in the debug log
* imitate Windows resources of gameoverlayrenderer + add resources to networkingsocketslib
* force add gameserver if `always_lan_type` was specified, not necessary but just in case
* allow injecting id string during build via command line switch `+build_str <str>`

---

## 2024/04/03 (hotfix 1)

* load achievements strings before creating fonts, so that their glyphs ranges are taken into consideration

---

## 2024/04/03

* **[detiam]** fix linking errors when building on archlinux
* **[detiam]** optimize Linux deps build script:
  - new argument `-packages_skip`: allows skipping installation of distro packages, such as `build-essential`, `gcc-multilib`, etc...
  - the above command introduced the ability to run without root
  - if the script was ran without root, and `-packages_skip` wasn't specified,
    the script will attempt to detect and use the built-in tool `sudo` if it was available
* **[Detanup01]** Added Steamwork SDK version 159
* **[detiam]** added schinese and tchinese translations to the overlay
* **[detiam]** enhanced the overlay font
  - replace the builtin font with `Unifont`
  - allow loading a custom font whose location is defined in `overlay_appearance.txt`
    fonts with relative paths will be loaded from `steam_settings/fonts`
* allow sharing leaderboards scores with connected players, adjust players ranks locally, and sort entries as needed by the game, suggested by **[M4RCK5]**
  this will only work when people connected on the same network are playing the same game, once they disconnect their leaderboard entry will be lost (no data persistence for other players), also it doesn't work well with VPN clients
  this behavior could be enabled via `share_leaderboards_over_network.txt`
* implemented the missing interface `ISteamGameServerStats`, allowing game servers to exchange user stats & achievements with players
  could be disabled via `disable_sharing_stats_with_gameserver.txt`,
  you can also create `immediate_gameserver_stats.txt` to sync data immediately, but **not recommended**
* for windows: updated stub drm patterns and added a workaround for older variants,
  this increases the compatibility, but makes it easier to be detected
* for windows: new stub/mock dll `GameOverlayRenderer(64).dll` for the experimental cold client setup,
  some apps verify the existence of this dll, either on disk, or inside their memory space
  **not recommended** to ignore it
* separated the config file `disable_leaderboards_create_unknown.txt`, previously it was tied to `leaderboards.txt`,
  by default the emu will create any unknown leaderboards, you can disable this behavior with this file
  **not recommended** to disable this behavior
* for the tool `generate_emu_config`:
  - don't generate `disable_xxx` config files by default
  - new option `-de`: generate config files inside `steam_settings` folder to disable some extra features of the emu
    note that this option deprecates the option `-nd`
  - new option `-cve`: generate config files inside `steam_settings` folder to enable some convenient extra features of the emu
  - allow specifying the username and password via the environment variables `GSE_CFG_USERNAME` and `GSE_CFG_PASSWORD`,
  this will override the data specified in `my_login.txt`
* properly implement `Steam_User_Stats::ResetAllStats()`
* added missing example file `disable_lobby_creation.txt` in `steam_settings` folder + updated release `README`
* allow overlay invitations to obscure game input to be able to accept/reject the request
* fixed a problem in the overlay where players connected on the same network might be ignored during startup, resulting in an empty friend list
* set the minimum game server latency/ping to `2ms`
* added new function `rmCallbacks()` for the networking, to be able to cleanup callbacks on object destruction
* missing `delete` (cleanup) for `ugc_bridge` instance + reset pointers on client objects destruction
* for windows build script: prevent permissive language extensions via the compiler flag `/permissive-`

---

## 2024/03/17

* **[bitsynth]** Fix Age of Empires 2: Definitive Edition, the game expects the app itself to be an owned DLC,
  otherwise most options will be disabled

* `Steam_Apps::GetCurrentBetaName()` make sure the out buffer is null terminated

---

## 2024/03/16

* manage overlay cursor input/clipping and internal frame processing in a better way,
  should prevent more games from pausing to display notifications
* initially attempt to load the icons of all achievements, this will slow things down at startup,
  but avoids having to load the achievement icon during gameplay which causes micro-stutter
* avoid loading and resizing the achievement icon each time it's unlocked
* Local_Storage: avoid allocating buffers unless `stbi_load()` was successfull
* changed how manual callback dispatch is handled, now it won't run the background thread,
  this might break stuff
* removed an outdated example file for dll injection in the `ColdClientLoader`
* refactor/restructure `steam_utils` into a separate cpp file

---

## 2024/03/09

* prevent notifications that do not require interaction from stealing focus
* check for success when creating the overlay popup window
* make the backgrounds of notifications and popups less transparent (more visible), for easier visibility
* show hidden achievement description in the overlay if it was unlocked
* don't fail loading both achievement icons, locked and unlocked, if eihter one of them wasn't loaded but the other was

---

## 2024/03/08 (hotfix 1)

* don't allow posting overlay achievements notifications when the overlay isn't ready yet
* don't run overlay callback when it isn't ready yet
* don't initialize or setup the overlay when `disable_overlay.txt` is used

---

## 2024/03/08

* updated the ingame overlay project, suggested by **[CHESIRE721]**
  Thanks to **[Nemirtingas]** for the amazing project: https://github.com/Nemirtingas/ingame_overlay
* for Linux: new experimental build of the emu with overlay support
  currently only *some* 64-bit games using OpenGL will work
* use a new method to initialize the overlay on a separate thread with a configurable initialization delay and renderer detection timeout
  - the new config file `overlay_hook_delay_sec.txt` controls the initial delay (default = `0 seconds`)
  - the new config file `overlay_renderer_detector_timeout_sec.txt` controls the detection timeout (default = `15 seconds`)

  check the updated `README`
* added builtin fonts to properly render all overlay translations:
  - `NotoSansJP-SemiBold`: for japanese
  - `NotoSansKR-SemiBold`: for korean
  - `NotoSansSC-SemiBold`: for simplified chinese
  - `NotoSansTC-SemiBold`: for traditional chinese
  - `NotoSansThai-SemiBold`: for Thai
  - `Google-Roboto-Medium`: for other languages
* added 2 new entries for the config file `overlay_appearance.txt`
  - `Font_Glyph_Extra_Spacing_x`: controls the extra horizontal spacing of characters (default = 1.0)
  - `Font_Glyph_Extra_Spacing_y`: controls the extra vertical spacing of characters (default = 0.0)

  the extra horizontal spacing is especially needed for non-latin characters, otherwise they are squeezed
* removed the source files of the ingame overlay project, it is now a dependency,
  **rebuild your dependencies!**
* removed the code which locks the cursor inside the overlay window
* attempt to load the locked achievement icon from the json key `icongray` if the normal one failed, adding compatibility with older format
* cleanup/free overlay images on unhook
* free the detector instance once it's no longer needed
* use locks everywhere in the overlay + more debug messages
* fixed a bug in the settings parser where lines with 1 single character would be completely erased after trimming spaces
* fixed all compilation warnings produced by the overlay on Linux
* updated all build scripts

---

## 2024/02/29

* revert the changes to `steam_matchmaking_servers` and only enable them via the 2 new config files:
  - `matchmaking_server_list_actual_type.txt`: enable the behavior which allows steam matchmaking to use the actual type of the requestd server list, otherwise it's always LAN
  - `matchmaking_server_details_via_source_query.txt`: enable the behavior which allows steam matchmaking to use actual source server query to grab the server info

  thanks a lot to **[LuKeSt0rm]** for the help and testing

* added a new flag `-reldir` for the `generate_emu_config` script which allows it to generate temp files/folders, and expect input files, relative to the current working directory, suggested by **[ImportTaste]**

---

## 2024/02/24

* build the python scripts `achievements_gen.py` and `parse_controller_vdf.py` into binary form using `pyinstaller` for a more user friendly usage, suggested by **[DogancanYr]**
* change the scripts `achievements_gen.py` and `parse_controller_vdf.py` to accept multiple files

---

## 2024/02/23

* more accurately handle and download steamhttp requests in multi-threaded manner:
  - handle `GET`, `HEAD`, `POST`
  - properly set `POST` data (raw and parameterized), and `GET` parameters
  - properly set request headers

* new config file `force_steamhttp_success.txt` in `steam_settings` folder, which forces the API `Steam_HTTP::SendHTTPRequest()` to always succeed

> ⚠️ **Breaking:** deprecated the config file `http_online.txt` in favor of the new one `download_steamhttp_requests.txt`

---

## 2024/02/20

* `generate_emu_config`: allow setting the steam id of apps/games owners from an external file `top_owners_ids.txt` beside the script, suggested by **[M4RCK5]**
* `generate_emu_config`: support the new format for `supported_languages`
* `generate_emu_config`: update the code which parses controller inputs
* `generate_emu_config`: always use the directory of the script for: the data `backup` folder, the `login_temp` folder, the `my_login.txt` file

---

## 2024/02/13

* cold client loader: validate the PE signature before attempting to detect arch

---

## 2024/02/10

* a hacky fix for the overlay on directx12, currently very slow when loading images
* limit the attempts to load the achievements images, to prevent a never ending FPS drop

---

## 2024/02/07

* new persistent modes for cold client loader, mode 2 is a more accurate simulation and allows launching apps from their .exe
* allow setting the IP country via the file `ip_country.txt`

> ⚠️ **Breaking:** changed the ini sections of the cold client loader

---

## 2024/01/26

* **[Detanup01]** added a new command line option for the tool `generate_emu_config` to disable the generation of `disable_xxx.txt` files,
  suggested by **[Vlxst]**
* added new settings to the overlay which allow specifying the notifications positions, check the example file `overlay_appearance.EXAMPLE.txt`,
  suggested by **[ugurkahriman]**
* fixed a mistake when discarding the utf8 bom marker

---

## 2024/01/25

* added new options to the overlay to allow copying a friend's ID, plus current player ID, suggested by **[Vlxst]**
* added a new option to the overlay to invite all friends playing the same game, suggested by **[Vlxst]**
* added new `auto_accept_invite.txt` setting to automatically accept game/lobby invites from this list, each SteamID64 on a separate line
  also you can leave the file empty to accept invitations from anyone, check the updated release readme, suggested by **[Vlxst]**
* added new `disable_overlay_warning_*.txt` settings to disable certain or all warnings in the overlay, suggested by **[Vlxst]**
  * `disable_overlay_warning_forced_setting.txt`:
    - disable the warning for the usage of any file `force_*.txt` in the overlay
    - unlocks the settings menu, this may result in an undesirable output
  * `disable_overlay_warning_bad_appid.txt`: disable the warning for bad app ID (when app ID = 0) in the overlay
  * `disable_overlay_warning_local_save.txt`: disable the warning for using local save in the overlay
  * `disable_overlay_warning_any.txt`: all the above
* **deprecated** `disable_overlay_warning.txt` in `steam_settings` folder in favor of new the options/files
* added more Stub variants
* fixed the condition of `warn_forced_setting`, previously it may be reset back to `false` accidentally
* fixed a casting mistake when displaying friend ID
* avoid spam loading the achievements forever on failure, only try 3 times
* removed a debug flag in `UGC::GetItemState()` left by mistake

---

## 2024/01/20

* **[Detanup01]** added implementation for `Steam_Remote_Storage::EnumerateUserSubscribedFiles()` +
  mods files handles in `Steam_Remote_Storage::UGCDownload()` + `Steam_Remote_Storage::UGCDownloadToLocation()`
  which makes mods now work for many games
* **[Kola124]** enhanced the settings parser to detect primary and preview mod files sizes automatically +
  use the base Steam URL by default for workshop URL + auto calculate the mod `score` from up/down votes
  also thanks to **[BTFighter]** for providing logs
> ⚠️ **Breaking:** mod preview image file must exist in `steam_settings/mods_img/<MOD_ID>`
* an enhancement to the settings parser to attempt to auto detect mods when `mods.json` is not present, with the same behavior as when the json file was created
  this works for mods with only 1 primary file and only 1 preview file
* fixed the generated path of mod `preview_url`, previously it would contain back slashes `\` on Windows
* use last week epoch as the default time for mods dates (created, added, etc...)
* make sure the mod path is always normalized and absolute, required by some APIs
* `Steam UGC`: implement `SetUserItemVote()`, `GetUserItemVote()`, `AddItemToFavorites()`, `RemoveItemFromFavorites()`,
  favorite mods list are now saved in `favorites.txt` in the user save data folder
* cold client loader can now inject user dlls, and force inject the `steamclient(64).dll` library,
  also you can control the injection order via a file `load_order.txt`, check its readme and the provided example
* a new experimental dll (which must be injected first) to patch Stub drm v3.1 in memory, check the injection example of the cold client loader
* cold client loader will now treat relative paths as relative to its own path, previously it used the current active directory
* cold client loader no longer needs an explicit setting for the `ExeRunDir`, by default it would be the folder of the exe
* in cold client loader, the option `ResumeByDebugger` is now available for the release build
* cold client loader is now built for 32-bit and 64-bit separately, and will display a nag about architecture difference if for example the app was 32-bit and the loader was 64-bit, this could be disabled via the setting `IgnoreLoaderArchDifference=1`
* the cold client loader will output useful debug info when the debug build is used
* added a very basic crashes logger/printer, enabled by creating a file called `crash_printer_location.txt` inside the `steam_settings` folder, check README.realease.md for more details
* fixed a problem in the overlay which would cause a crash for the guest player when an invitation was sent
* `Steam UGC`: make sure returned mod folder from `GetItemInstallInfo()` is null terminated, previously some apps would get a bad malformed string because of this
* `Steam_RemoteStorage`: very basic implementation for `GetQueryUGCNumTags()`, `GetQueryUGCTag()`, `GetQueryUGCTagDisplayName()`
* new function in local storage to get list of folders at root level, given some path
* imitate how the DOS Stub is manipulated during/after the build
* some fixes to the win build script + use the undocumented linker flag `/emittoolversioninfo:no` to prevent adding the MSVC Rich Header
* debug messages are now mostly scoped, ex: `Steam_Ugc::XXX`
* added a bunch of helper functions, `common_helpers::XXX` + `pe_helpers::XXX`

---

## 2024/01/05

* **[Detanup01]** Fixed parsing of old Steam interfaces, reported by **[LuKeStorm]**: https://cs.rin.ru/forum/viewtopic.php?p=2971639#p2971639
* refactored the tool `find_interfaces` to search accurately for old interfaces

---

## 2024/01/03

* added a new option to the Windows version of the client loader to aid in debugging
the option is called `ResumeByDebugger`, and setting it to `1` will prevent the loader from
auto resuming the main app thread, giving you a chance to attach your debugger
* make the script `generate_emu_config` generate an empty `DLC.txt` if the app has no DLCs
* windows build: sign each file after build with a self-signed generated certificate + note in the release readme regarding false-positives
* windows build: note in readme about Windows SDK
* windows build: added vesion resource (.rc file)
* gen emu config: readme + icon attribution
* added anonymous login to gen emu script, these accounts have very limited access
* linux + win build scripts: introduce -verbose flag
* windows build script: ensure /MT when compiling
* output protoc generated in a subfolder in dll/ for easier code reference +
  don't cleanup protoc generated files, because VScode gets confused and cannot find files/types

---

## 2023/12/21 - 2023/12/27

* **[Detanup01]** added option to send auth token with new Ticket! + an option to include the GC token
  by default the emu will send the old token format for various APIs, like:
  * `Steam_GameServer::GetAuthSessionTicket()`
  * `Steam_User::GetAuthSessionTicket()`
  * `Steam_User::GetAuthTicketForWebApi()`

  this allows the emu to generate new ticket data, and additionally the GC token
  check the new config files `new_app_ticket.txt` and `gc_token.txt` in the `steam_settings` folder
* **[Detanup01]** fixed print issues in some places
* **[remelt]** use the `index` argument to grab the preview URL from UGC query result, fixed by: https://cs.rin.ru/forum/viewtopic.php?p=2964432#p2964432
* **[remelt]** allow overriding mod `path` & mod `preview_url` in the `mods.json` file, suggested by: https://cs.rin.ru/forum/viewtopic.php?p=2964432#p2964432
* allow setting the mod `score` in the `mods.json`
* when the mod `preview_url` is not overridden, don't set it automatically if `preview_filename` was empty, otherwise the `preview_url` will be pointing to the entire `mods_img` folder, like: `file://C:/my_game/steam_settings/mods_img/`
  instead set it to an empty string
* updated `mods.EXAMPLE.json`
* added 2 new config files `is_beta_branch.txt` and `force_branch_name.txt`
  by default the emu will report a `non-beta` branch with the name `public` when the game calls `Steam_Apps::GetCurrentBetaName()`
  these new config files allow changing that behavior, check the `steam_settings` folder
* refactored the `steamclient_loader` script for Linux + new options and enhancements to make it similar to the Windows version, check its new README!
* for steamclient loader (Windows + Linux): pass loader arguments to the target exe, allowing it to be used from external callers, example by the `lobby_connect` tool
* deprecated the `find_interface` scripts, now the executable is built for Windows & Linux!
* included the `steam_settings.EXAMPLE` for Linux build
* updated release READMEs!
* added a README for the repo with detailed build steps
---

* check for invalid data pointer in `GetAuthSessionTicket()`
* additional sanity check in `InitiateGameConnection()` + print input data address in debug build
* moved the example `app id` and `interfaces` files inside `steam_settings` folder, to avoid encouraging putting files outside

---

* fixed all debug build warnings for Linux & Windows (no more scary messages!)
* updated Linux & Windows build scripts to avoid removing the entire build folder before building + introduced `clean` flag
* added licenses & sources of all external libraries + added a new cryptography library `Mbed TLS`
  you have to rebuilt the deps
* deprecated the separate/dedicated cleanup script for Windows, it's now inlined in the main build script
* for Windows build script: deprecated `low perf` & `win xp` options
* for Linux build script: deprecated `low perf` option
* restored all original but unused repo files into their separate folder
* lots of refactoring and relocation in the source repo:
  - all build stuff will be inside `build` folder
  - restructured the entire repo
  - generate proto source files in the `build/tmp` folder instead of the actual source folder

---

* `settings_parser.cpp`:
  - cleanup the settings parser code by split it into functions
  - increase the buffer size for `account_name` to 100 chars
  - increase the buffer size for `language` to 64 chars
* `common_includes.h`:
  - refactor includes order
  - added new helper function to keep yielding the thread for a given amount of time (currently unused)
* build scripts:
  - in Linux build scripts don't use `-d` flag with `rm`
  - added global build stat message
  - use an obnoxious name for the file handle variable used if the PRINT_DEBUG macro to avoid collisions, in the caller has a variable with same name
* don't cache deps build when pushing tag or opening pull requests
* remove hardcoded repo path + remove Git LFS flag since it's no longer needed

---

## 2023/12/20

* fixed the implementation of `BIsAppInstalled()`, it must lock the global mutex since it is thread-safe, otherwise it will cause starvation and the current thread won't yield, which triggers some games

* more accurate behavior for `BIsAppInstalled()`, reject app ID if it was in the DLC list and isUnlockAllDlc was false

* basic implementation for `RequestAppProofOfPurchaseKey()` and `RequestAllProofOfPurchaseKeys()`

* a simple implementation for `GetEarliestPurchaseUnixTime()`

* more accurate implementation for `BGetSessionClientResolution()`, set both x & y to 0

* return false in `BIsDlcInstalled()` when the given app ID is the base game

* check for invalid app ID `uint32_max` in different places

* more accurate implementation for `BReleaseSteamPipe()`, return true if the pipe was released successfully

* lock the global mutex and the overlay mutex in different places just to be on the safe side, without it, some games suffer from thread starvation, might slow things down

* added missing env var `SteamOverlayGameId` to steam_client and client_loader

* added a startup timer + counter for reference, currently used to print timestamp in debug log

* consistent debug log location, for games that change cwd multiple times while running

* fixed error propagation in Windows build script, apparently set /a var+=another_var works only if another_var is a defined env var but NOT one of the "magic" builtins like errorlevel

---

## 2023/12/17
* More accurate implementation for BIsAppInstalled(), it now rejects uint32_max

* Allow behavior customization via `installed_app_ids.txt` config file

* Limit/Lock list of installed apps on an empty file (similar to dlc.txt)

* Changed the behavior of GetCurrentBetaName() to comply with the docs, might break stuff

* Allow customizing the behavior via ne config files: `is_beta_branch.txt` + `force_branch_name.txt`

* New script to generate native executable for `generate_emu_config` on Linux using pyinstaller

* Deprecate the old `RtlGenRandom()` in favor of the new `BCryptGenRandom()`

* Setup Github Worflows to:
  * Build `generate_emu_config` for `Linux` when you push code to a branch whose name matches the pattern `ci-build-gen-linux*`
  * Build `generate_emu_config` for `Windows` when you push code to a branch whose name matches the pattern `ci-build-gen-win*`
  * Build the emu for `Linux` when you push code to a branch whose name matches the pattern `ci-build-emu-linux*`
  * Build the emu for `Windows` when you push code to a branch whose name matches the pattern `ci-build-emu-win*`
  * Build everything when you push code to a branch whose name is `ci-build-all`
  * Build everything and create a release when you push a tag whose name matches the pattern `release*`

* Packaging scripts for both Windows & Linux, usable locally and via Github Workflows
  * For the emu:
    * First run `build_win_deps.bat` (Windows)
    or `sudo ./build_linux_deps.sh` (Linux)
    * Run `build_win.bat release` + `build_win.bat debug` (Windows)
    or `./build_linux.sh release` + `./build_linux.sh debug` (Linux)
    * Finally run `package_win.bat release` + `package_win.bat debug` (Windows)
    or `sudo ./package_linux.sh release` + `sudo ./package_linux.sh debug` (Linux)
  * The same goes for `generate_emu_config` (scripts folder) but the scripts do not take any arguments, so no `release` or `debug`

* Added all third-party dependencies as local branches in this repo + refer to these branches as submodules, making the repo self contained

---

## 2023/12/14
* based on cvsR4U1 by **[ce20fdf2]** from viewtopic.php?p=2936697#p2936697

* apply the fix for the Linux build (due to newer glibc) from this pull request by Randy Li: https://gitlab.com/Mr_Goldberg/goldberg_emulator/-/merge_requests/42/

* add updated translation of Spanish + Latin American to the overlay by dragonslayer609 from viewtopic.php?p=2936892#p2936892
* add updated translation of Russian to the overlay by GogoVan from viewtopic.php?p=2939565#p2939565

* add more interfaces to look for in the original steam_api by alex47exe from viewtopic.php?p=2935557#p2935557

* add fix for glyphs icons for xbox 360 controller by 0x0315 from viewtopic.php?p=2949498#p2949498

* bare minimum implementation for SDK 1.58a
  + backup the current version of the interface 'steam ugc'
    -  create new file: isteamugc017.h
        + copy the current version of the interface to this file
        + don't copy enums, structs, constants, etc..., just copy the pure virtual (abstract) class of the interface
        + rename the abstract class to include the current version number in its name, i.e. 'class ISteamUGC017'
        + create a file header guard containing the interface version in its name, i.e. 'ISTEAMUGC017_H'
        + if the file has '#pragma once', then guard this line with '#ifdef STEAM_WIN32' ... '#endif', I don't know why
  + isteamugc.h (this always contains the declaration of latest interface version)
    - declare the new API: GetUserContentDescriptorPreferences()
    - update the API: SetItemTags() to use the new argument
    - update the interface version to STEAMUGC_INTERFACE_VERSION018
  + steam_ugc.h (this always contains the implementation of ALL interfaces versions)
    - add the backed-up abstract class to the list of inheritance, i.e. 'public ISteamUGC017'
    - (needs revise) implement the new API: GetUserContentDescriptorPreferences()
    - add a new overload of the API: SetItemTags() which takes the new additional argument

  + backup the current version of the interface 'steam remote play'
    -  create new file: isteamremoteplay001.h
        + copy the current version of the interface to this file
        + don't copy enums, structs, constants, etc..., just copy the pure virtual (abstract) class of the interface
        + rename the abstract class to include the current version number in its name, i.e. 'class ISteamRemotePlay001'
        + create a file header guard containing the interface version in its name, i.e. 'ISTEAMREMOTEPLAY001_H'
        + if the file has '#pragma once', then guard this line with '#ifdef STEAM_WIN32' ... '#endif', I don't know why
  + isteamremoteplay.h (this always contains the declaration of latest interface version)
    - declare the new API: BStartRemotePlayTogether()
    - update the interface version to STEAMREMOTEPLAY_INTERFACE_VERSION002
    - fix file header guard from _WIN32 to STEAM_WIN32
  + steam_remoteplay.h (this always contains the implementation of ALL interfaces versions)
    - add the backed-up abstract class to the list of inheritance, i.e. 'public ISteamRemotePlay001'
    - (needs revise) implement the new API: BStartRemotePlayTogether()

  + steam_api.h
    - #include the backed-up interface files:
        + #include "isteamugc017.h"
        + #include "isteamremoteplay001.h"
    - declare the new API: SteamInternal_SteamAPI_Init()
    - add a new enum ESteamAPIInitResult
    - fix return type of SteamAPI_InitSafe() from bool to steam_bool (some stupid games read the whole EAX register)
    - add a useless inline implementation for the API: SteamAPI_InitEx(), not exported yet but just in case for the future
  + steam_gameserver.h
    - declare the new API: SteamInternal_GameServer_Init_V2()
    - fix return type of SteamGameServer_Init() from bool to steam_bool (some stupid games read the whole EAX register)
    - add a useless inline implementation for the API: SteamGameServer_InitEx(), not exported yet but just in case for the future
  + steam_api_common.h
    - declare a new type: SteamErrMsg
  + dll.cpp (this has the implementation of whatever inside steam_api.h + steam_gameserver.h)
    - (needs revise) implement the new API: SteamInternal_SteamAPI_Init()
    - (needs revise) implement the new API: SteamInternal_GameServer_Init_V2()
    - read some missing interfaces versions when parsing steam_interfaces.txt
    - initialize all interfaces versions with the latest ones available, instead of hardcoding them

  + steam_client.cpp
    - add a new version string for the interface getter GetISteamUGC()
    - add a new version string for the interface getter GetISteamRemotePlay()

  + isteamnetworkingsockets.h
    - fix the signatures of the APIs: (ISteamNetworkingConnectionCustomSignaling vs ISteamNetworkingConnectionSignaling)
        + ConnectP2PCustomSignaling()
        + ReceivedP2PCustomSignal()
  + isteamnetworkingsockets009.h
    - fix the signatures of the APIs: (ISteamNetworkingConnectionCustomSignaling vs ISteamNetworkingConnectionSignaling)
        + ConnectP2PCustomSignaling()
        + ReceivedP2PCustomSignal()
  + steam_networking_sockets.h
    - implement the missing overloads of the APIs: (ISteamNetworkingConnectionCustomSignaling vs ISteamNetworkingConnectionSignaling)
        + ConnectP2PCustomSignaling()
        + ReceivedP2PCustomSignal()

  + steam_api_flat.h
    ////////////////////
    - declare new interfaces getters:
        + SteamAPI_SteamUGC_v018()
        + SteamAPI_SteamGameServerUGC_v018()
    - declare the new API: SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences()
    - (needs revise) update signature of the API: SteamAPI_ISteamUGC_SetItemTags() to add the new argument
      this will potentially break compatibility with older version of the flat API
      ////////////////////
    - declare new interface getter: SteamAPI_SteamRemotePlay_v002()
    - declare the new API: SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether()
    ////////////////////
    - fix the signatures of the APIs: (ISteamNetworkingConnectionCustomSignaling vs ISteamNetworkingConnectionSignaling)
        + SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling()
        + SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal()

  + flat.cpp
    ////////////////////
    - implement new interfaces getters:
        + SteamAPI_SteamUGC_v018()
        + SteamAPI_SteamGameServerUGC_v018()
    - implement the new API: SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences()
    - (needs revise) update signature of the API: SteamAPI_ISteamUGC_SetItemTags() to use the new argument
      this will potentially break compatibility with older version of the flat API
      ////////////////////
    - implement new interface getter SteamAPI_SteamRemotePlay_v002()
    - implement the new API: SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether()
    ////////////////////
    - fix the signatures of the APIs: (ISteamNetworkingConnectionCustomSignaling vs ISteamNetworkingConnectionSignaling)
        + SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling()
        + SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal()

  + isteamfriends.h
    - (needs revise) add a missing (or new?) member m_dwOverlayPID to the struct GameOverlayActivated_t, hopefully this doesn't break stuff

  + steamnetworkingtypes.h
    - add new (or missing?) members to the enum ESteamNetworkingConfigValue:
        + k_ESteamNetworkingConfig_RecvBufferSize
        + k_ESteamNetworkingConfig_RecvBufferMessages
        + k_ESteamNetworkingConfig_RecvMaxMessageSize
        + k_ESteamNetworkingConfig_RecvMaxSegmentsPerPacket

  + add the file isteamdualsense.h, it isn't used currently but just in case for the future

  + update descriptions/comments or refactor/spacing
    - isteamapplist.h
    - isteamgamecoordinator.h
    - isteamps3overlayrenderer.h
    - isteamuserstats.h
    - isteamutils.h
    - isteamvideo.h
    - steamhttpenums.h
    - steamtypes.h

* use Unicode when sanitizing settings, mainly for local_save.txt config file
  + new dir "utfcpp": containg all the source/include files of this library: https://github.com/nemtrif/utfcpp
  + common_includes.h: include the new library "utfcpp"
  + settings.cpp: in Settings::sanitize(): convert to utf-32 first, do the sanitization, then convert back to std::string and return the result

* avoid locking the global mutex every time when getting the global steamclient instance
  + dll.cpp: in get_steam_client(): only lock when the instance is null and double check for null, should speed up things a little bit
* in different places, avoid locking gloal mutex if the relevant functionality was disabled
  + example in steam_user_stats.h: SetAchievement()
  + example in steam_overlay.cpp:
      + Steam_Overlay::AddMessageNotification()
      + Steam_Overlay::AddInviteNotification()

* explicitly use the ASCII version of Windows APIs to avoid conflict when building with define symbols UNICODE + _UNICODE
  - base.cpp: GetModuleHandleA()
  - steam_overlay.cpp: PlaySoundA()

* fix the implementation of RtlGenRandom stub:
  + return a number
  + use extern "C" if building in C++ mode

* add new build scripts for both Windows and Linux for a much easier dev/build experience,
  both Windows and Linux scripts will run parallel build jobs for a much faster build times,
  by default, the scripts will use 70% of the max available threads, but if the auto detection didn't work,
  you can pass for example `-j 10` to the scripts to use 10 parallel jobs

  on Linux, archives (.a files) of third party libraries are bundled wholly, and built statically via:
  `-Wl,--whole-archive -Wl,-Bstatic -lssq -lcurl ... -Wl,-Bdynamic -Wl,--no-whole-archive`
  this ensures that the final output binary (for example: libsteam.so) won't require these libraries at runtime

  + to build on Linux (I'm using latest Ubuntu on WSL)
    - run as `sudo ./build_linux_deps.sh`, this will do the following:
      + download and install the required build tools via `apt-install`
      + unpack the third party libraries (protobuf, zlib, etc...) from the folder `third-party` to `build-linux-deps`
      + build the unpacked libraries from `build-linux-deps`

      you only need this step once, additionally you can pass these arguments to the script:
      + `-verbose`: force cmake to display extra info
      + `-j <n>`: force cmake to use `<n>` parallel build jobs

    - without sudo, run `./build_linux.sh` and pass the argument `release` or `debug` to build the emu in the corresponding mode, this will build the emu inside the folder `build-linux`
      some additional arguments you can pass to the script:
      + `-lib-32`: prevent building 32-bit libsteam_api.so
      + `-lib-64`: prevent building 64-bit libsteam_api.so

      + `-client-32`: prevent building 32-bit steamclient.so
      + `-client-64`: prevent building 64-bit steamclient.so

      + `-tool-clientldr`: prevent copying the script steamclient_loader.sh
      + `-tool-itf`: prevent copying the script find_interfaces.sh
      + `-tool-lobby-32`: prevent building executable lobby_connect_x32
      + `-tool-lobby-64`: prevent building executable lobby_connect_x64

      + `+lowperf`: (UNTESTED) pass some arguments to the compiler to prevent emmiting instructions for: SSE4, popcnt, AVX

      + `-j <n>`: force build operations to use `<n>` parallel jobs

  + to build on Windows (just install Visual Studio 2019/2022)
    - without admin, run `build_win_deps.bat`, this will do the following:
       + unpack the third party libraries (protobuf, zlib, etc...) from the folder `third-party` to `build-win-deps`
       + build the unpacked libraries from `build-win-deps`



      you only need this step once, additionally you can pass these arguments to the script:
      + `-verbose`: force cmake to display extra info
      + `-j <n>`: force cmake to use `<n>` parallel build jobs

    - without admin, run `build_win.bat` and pass the argument `release` or `debug` to build the emu in the corresponding mode,
      this will build the emu inside the folder `build-win`
      some additional arguments you can pass to the script:
      + `-lib-32`: prevent building 32-bit steam_api.dll
      + `-lib-64`: prevent building 64-bit steam_api64.dll

      + `-ex-lib-32`: prevent building `experimental steam_api.dll`
      + `-ex-lib-64`: prevent building `experimental steam_api64.dll`

      + `-ex-client-32`: prevent building `experimental steamclient.dll`
      + `-ex-client-64`: prevent building `experimental steamclient64.dll`

      + `-exclient-32`: prevent building experimental `client steamclient.dll`
      + `-exclient-64`: prevent building experimental `client steamclient64.dll`
      + `-exclient-ldr`: prevent building experimental `client loader steamclient_loader.exe`

      + `-tool-itf`: prevent building executable `find_interfaces.exe`
      + `-tool-lobby`: prevent building executable `lobby_connect.exe`

      + `+lowperf`: (UNTESTED) for 32-bit build only, pass the argument `/arch:IA32` to the compiler

      + `-j <n>`: force build operations to use `<n>` parallel jobs

* added all required third-party libraries inside the folder `third-party`

* greatly enhanced the functionality of the `generate_emu_config` script + add a build script
  + run `recreate_venv.bat` to
     + create a python virtual environemnt
     + install all required packages inside this env
  + run `rebuild.bat` to produce a bootstrapped .exe built using `pyinstaller`
  + inside the folder of the built executable
     + create a file called `my_login.txt`, then add your username in the first line, and your password in the second line
     + run the .exe file without any args to display all available options

* revert the changes to `SetProduct()` and `SetGameDescription()`

* in `steam_overlay.cpp`, in `AddAchievementNotification()`: prefer original paths of achievements icons first, then fallback to `achievement_images/`

---

## Older changes
* add missing implementation of (de)sanitize_string when `NO_DISK_WRITE` is defined which fixes compilation of `lobby_connect`

* check for empty string in (de)sanitize_file_name() before accessing its items

* implement new API: `GetAuthTicketForWebApi()`
  + `base.h`: declare the new API: `getWebApiTicket()`
  + `base.cpp`: implement the new API: `Auth_Ticket_Manager::getWebApiTicket()`
  + `steam_user.h`: call the new API inside `GetAuthTicketForWebApi()`

* add an updated and safer impl for `Local_Storage::load_image_resized()` by RIPAciD from viewtopic.php?p=2884627#p2884627

* add missing note in ReadMe about `libssq`

* add new release 4 by **[ce20fdf2]** from viewtopic.php?p=2933673#p2933673
* add hotfix 3 by **[ce20fdf2]** from viewtopic.php?p=2921215#p2921215
* add hotfix 2 by **[ce20fdf2]**: viewtopic.php?p=2884110#p2884110
* add initial hotfix by **[ce20fdf2]**
