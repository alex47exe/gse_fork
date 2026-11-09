#ifdef EMU_OVERLAY

// if you're wondering about text like: ##PopupAcceptInvite
// these are unique labels (keys) for each button/label/text,etc...
// ImGui uses the labels as keys, adding a suffic like "My Text##SomeKey"
// avoids confusing ImGui when another label has the same text "MyText"

#include "overlay/steam_overlay.h"

#include <thread>
#include <string>
#include <sstream>
#include <cctype>
#include <utility>
#include <unordered_set>
#include <unordered_map>
#include <random>
#include <ctime>
#include <numeric>

#include <algorithm>
#include <sys/stat.h>
#include <cstdlib>  // std::system

#ifdef __WINDOWS__
#include <shellapi.h>
#endif

#include "InGameOverlay/RendererDetector.h"

#include "dll/dll.h"
#include "dll/settings_parser.h"
#include "dll/steam_app_ids.h"

#include "dll/screenshot_format.h"

// image loading for gallery thumbnails
// local_storage.cpp already defines the IMPLEMENTATION with _STATIC,
// so we must do the same here to avoid linker errors (each .obj gets its own private copy).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

// translation
#include "overlay/steam_overlay_translations.h"
// fonts
#include "fonts/unifont.hpp"
#include "overlay_bridge.h"

// forward declaration — defined later in this file
static void format_ip_address(uint32 ip, char *buf, size_t len);

#define URL_WINDOW_NAME "URL Window"

// Fallback in case the build system did not inject EMU_BUILD_STRING / EMU_BUILD_DATE_STRING.
#ifndef EMU_BUILD_STRING
  #define EMU_BUILD_STRING "unknown"
#endif
#ifndef EMU_BUILD_DATE_STRING
  #define EMU_BUILD_DATE_STRING "unknown"
#endif

// Swapchain colour-space classification for overlay gamma correction.
//
// The overlay renders into the game's back-buffer.  Depending on the swap chain
// format the GPU interprets pixel values differently, so we must pre-transform
// sRGB image bytes and ImGui colours to match.
//
// Classification:
//   SCS_UNKNOWN       (-1)  Not yet detected — treated as SDR (no transform).
//   SCS_LINEAR_HDR    ( 0)  FP16 scRGB / FP32 / 16-bit linear UNORM.
//                            Needs:  sRGB→linear decode  +  SDR-white scale.
//   SCS_SDR_UNORM     ( 1)  Standard 8- or 16-bit SDR (no hw sRGB encoding).
//                            Needs:  optional mild contrast boost.
//   SCS_HDR10_PQ      ( 2)  10-bit HDR10 with PQ (ST.2084) transfer.
//                            Needs:  sRGB→linear→PQ encode  +  nit scaling.
//   SCS_SDR_SRGB_RTV  ( 3)  _SRGB back-buffer (hw applies sRGB on RTV write).
//                            Needs:  sRGB→linear decode (NO SDR-white scale)
//                            so the hardware re-encodes it back correctly.
enum SwapchainColorSpace : int {
    SCS_UNKNOWN      = -1,
    SCS_LINEAR_HDR   =  0,   // FP16 scRGB, FP32, 16-bit linear UNORM
    SCS_SDR_UNORM    =  1,   // Standard SDR 8/16-bit
    SCS_HDR10_PQ     =  2,   // HDR10 PQ (ST.2084, R10G10B10A2)
    SCS_SDR_SRGB_RTV =  3,   // _SRGB back-buffer (hw sRGB encoding)
};
static SwapchainColorSpace s_swapchain_cs = SCS_UNKNOWN;
// File-scope pointer to the current Overlay_Appearance, set once per frame in overlay_render_proc.
static const Overlay_Appearance *s_ov_app = nullptr;

// Resolve the effective colour space, respecting the user's Swapchain_Override setting.
static SwapchainColorSpace effective_swapchain_cs()
{
    if (s_ov_app) {
        using SO = Overlay_Appearance::SwapchainOverride;
        switch (s_ov_app->swapchain_override) {
            case SO::LinearHDR: return SCS_LINEAR_HDR;
            case SO::HDR10PQ:   return SCS_HDR10_PQ;
            case SO::SrgbRTV:   return SCS_SDR_SRGB_RTV;
            case SO::SDR:       return SCS_SDR_UNORM;
            default: break; // Auto — fall through to detected
        }
    }
    return s_swapchain_cs;
}
static const char* s_swapchain_fmt_str   = "Detecting..."; // DXGI format name
static const char* s_swapchain_type_str  = "Detecting..."; // HDR/SDR type  |  gamma  |  colour gamut
// SDR white level scale for HDR colour correction.
//   s_sdr_white_scale = sdr_white_nits / 80.0f
//   1.0f = standard 80-nit SDR white (default / fallback)
//   e.g. 200 nits SDR white -> scale = 2.5 (overlay and ImGui colours boosted proportionally)
static float       s_sdr_white_scale     = 1.0f;
static bool        s_sdr_scale_queried   = false; // true once display info has been queried
static bool        s_pending_sdr_refresh = false; // set on Reset/Removing; drained in overlay_render_proc

// Per-image colour adjustments (refreshed each frame from settings->overlay_appearance).
static float       s_img_brightness = 1.0f;
static float       s_img_contrast   = 1.0f;
static float       s_img_gamma_adj  = 1.0f;

// Texture cache invalidation tracking — mirrors the addon's approach.
// When these change, all baked textures (achievement icons, avatars, SCE) must be re-uploaded.
static SwapchainColorSpace s_cached_tex_cs    = SCS_UNKNOWN;
static float               s_cached_tex_scale = 1.0f;
static float               s_cached_tex_brightness = 1.0f;
static float               s_cached_tex_contrast   = 1.0f;
static float               s_cached_tex_gamma_adj  = 1.0f;

// Periodic swapchain re-detection interval (seconds).
// When the overlay is open, re-arm the format detect this often to catch
// mid-session HDR/resolution changes the game makes without a Reset.
static constexpr float SWAPCHAIN_REDETECT_INTERVAL_SEC = 5.0f;
static float           s_swapchain_redetect_timer      = 0.0f;

// Forward declaration — defined after query_display_hdr_details() below.
struct DisplayHdrDetail_t;
static std::vector<DisplayHdrDetail_t> refresh_sdr_white_scale();
static ImVec4 adjust_imgui_color_for_swapchain(ImVec4 c);

static ImU32 adjust_imgui_color_u32_for_swapchain(ImU32 c);

// Shorthand macros for wrapping inline sRGB colours for the active swap-chain colour space.
// TC()   — transforms an ImVec4 (used with TextColored, PushStyleColor).
// TC32() — transforms an ImU32  (used with AddRectFilled, AddRect, AddText, draw helpers).
// HDR transforms are applied only to textures (via transform_pixels_to_fp16);
// InGameOverlay handles the ImGui colour-space conversion internally, so UI
// colours are passed through unchanged.
#define TC(c) (c)
#define TC32(c) (c)

// ---------------------------------------------------------------------------
// Heuristic: determine whether 10-bit R10G10B10A2 / A2R10G10B10 / A2B10G10R10
// pixel data is PQ (HDR10, ST.2084) encoded or plain SDR (sRGB / gamma 2.2).
//
// R10G10B10A2_UNORM is an ambiguous format — it can carry either PQ-encoded
// HDR10 content (game manages HDR, sets DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
// or ordinary sRGB/gamma SDR content in 10-bit precision (display-managed HDR,
// where the DWM applies SDR→HDR conversion after compositing).
//
// Without the DXGI colour-space flag (the InGameOverlay callback only reports
// pixel format), we sample the actual back-buffer pixels:
//
//   PQ reference levels (10-bit code values out of 1023):
//       80 nit (SDR white) → PQ ≈ 520        400 nit → PQ ≈ 681
//     1000 nit             → PQ ≈ 783       4000 nit → PQ ≈ 945
//
//   SDR levels: full white = 1023.  Bright UI and surfaces routinely exceed 900.
//
// If a significant fraction of sampled pixels have any colour channel above 896
// (≈ PQ 2500 nit — extremely rare even in bright HDR scenes), the content is
// almost certainly SDR.
// ---------------------------------------------------------------------------
static bool detect_10bit_content_is_pq(const InGameOverlay::ScreenshotCallbackParameter_t* sc)
{
    if (!sc || !sc->Data || sc->Width == 0 || sc->Height == 0)
        return true;  // unable to determine — fall back to PQ (legacy default)

    const uint8_t* base   = static_cast<const uint8_t*>(sc->Data);
    const uint32_t stride = sc->Pitch;                                // bytes per row
    const uint32_t step_x = (std::max)(sc->Width  / 32u, 1u);        // ~32 samples across
    const uint32_t step_y = (std::max)(sc->Height / 32u, 1u);        // ~32 samples down
    constexpr uint32_t kThreshold = 896;  // ~87.6 % of 1023 → PQ ≈ 2500 nit

    uint32_t above = 0;
    uint32_t total = 0;

    for (uint32_t y = 0; y < sc->Height; y += step_y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(base + (size_t)y * stride);
        for (uint32_t x = 0; x < sc->Width; x += step_x) {
            uint32_t px  = row[x];
            uint32_t ch0 =  px        & 0x3FFu;
            uint32_t ch1 = (px >> 10) & 0x3FFu;
            uint32_t ch2 = (px >> 20) & 0x3FFu;
            uint32_t mx  = (ch0 > ch1) ? ch0 : ch1;
            if (ch2 > mx) mx = ch2;
            if (mx > kThreshold) ++above;
            ++total;
        }
    }

    // If more than 5 % of sampled pixels have any channel above 896, classify
    // as SDR.  In real PQ content, >2500 nit pixels are <1 % of the image.
    return total > 0 && (above * 100u / total) < 5u;
}

// Used in both OverlayHookReady and the [Refresh] button to (re-)arm one-shot format detection.
static void arm_swapchain_format_detect(InGameOverlay::RendererHook_t* r)
{
    s_swapchain_cs = SCS_UNKNOWN;
    s_swapchain_fmt_str   = "Detecting...";
    s_swapchain_type_str  = "Detecting...";
    r->SetScreenshotCallback([](InGameOverlay::ScreenshotCallbackParameter_t const* sc, void* user) {
        using F = InGameOverlay::ScreenshotDataFormat_t;
        if (!sc || sc->Format == F::Unknown) {
            s_swapchain_fmt_str  = "Unknown";
            s_swapchain_type_str = "Unknown";
            s_swapchain_cs = SCS_SDR_UNORM;
        } else {
            switch (sc->Format) {
                // ---- HDR / linear float formats ------------------------------------
                case F::R16G16B16A16_FLOAT:
                    s_swapchain_fmt_str  = "R16G16B16A16_FLOAT";
                    s_swapchain_type_str = "HDR  |  scRGB / Linear  |  BT.709+";
                    s_swapchain_cs = SCS_LINEAR_HDR;
                    break;
                case F::R16G16B16A16_UNORM:
                    s_swapchain_fmt_str  = "R16G16B16A16_UNORM";
                    s_swapchain_type_str = "HDR  |  Linear UNORM  |  BT.2020";
                    s_swapchain_cs = SCS_LINEAR_HDR;
                    break;
                case F::R32G32B32A32_FLOAT:
                    s_swapchain_fmt_str  = "R32G32B32A32_FLOAT";
                    s_swapchain_type_str = "HDR  |  Linear FP32  |  wide gamut";
                    s_swapchain_cs = SCS_LINEAR_HDR;
                    break;
                // ---- 10-bit formats (PQ or SDR — determined by pixel heuristic) ------
                case F::R10G10B10A2:
                    s_swapchain_fmt_str  = "R10G10B10A2_UNORM";
                    if (detect_10bit_content_is_pq(sc)) {
                        s_swapchain_type_str = "HDR10  |  PQ (ST.2084)  |  BT.2020";
                        s_swapchain_cs = SCS_HDR10_PQ;
                    } else {
                        s_swapchain_type_str = "SDR  |  sRGB  |  10-bit (display-managed HDR)";
                        s_swapchain_cs = SCS_SDR_UNORM;
                    }
                    break;
                case F::A2R10G10B10:
                    s_swapchain_fmt_str  = "A2R10G10B10_UNORM";
                    if (detect_10bit_content_is_pq(sc)) {
                        s_swapchain_type_str = "HDR10  |  PQ (ST.2084)  |  BT.2020";
                        s_swapchain_cs = SCS_HDR10_PQ;
                    } else {
                        s_swapchain_type_str = "SDR  |  sRGB  |  10-bit (display-managed HDR)";
                        s_swapchain_cs = SCS_SDR_UNORM;
                    }
                    break;
                case F::A2B10G10R10:
                    s_swapchain_fmt_str  = "A2B10G10R10_UNORM";
                    if (detect_10bit_content_is_pq(sc)) {
                        s_swapchain_type_str = "HDR10  |  PQ (ST.2084)  |  BT.2020";
                        s_swapchain_cs = SCS_HDR10_PQ;
                    } else {
                        s_swapchain_type_str = "SDR  |  sRGB  |  10-bit (display-managed HDR)";
                        s_swapchain_cs = SCS_SDR_UNORM;
                    }
                    break;
                // ---- 8-bit SDR formats (_SRGB = hardware sRGB encoding on RTV) -----
                case F::R8G8B8A8_SRGB:
                    s_swapchain_fmt_str  = "R8G8B8A8_UNORM_SRGB";
                    s_swapchain_type_str = "SDR  |  sRGB (hw decode)  |  Rec.709";
                    s_swapchain_cs = SCS_SDR_SRGB_RTV;
                    break;
                case F::B8G8R8A8_SRGB:
                    s_swapchain_fmt_str  = "B8G8R8A8_UNORM_SRGB";
                    s_swapchain_type_str = "SDR  |  sRGB (hw decode)  |  Rec.709";
                    s_swapchain_cs = SCS_SDR_SRGB_RTV;
                    break;
                case F::B8G8R8X8_SRGB:
                    s_swapchain_fmt_str  = "B8G8R8X8_UNORM_SRGB";
                    s_swapchain_type_str = "SDR  |  sRGB (hw decode)  |  Rec.709  (no alpha)";
                    s_swapchain_cs = SCS_SDR_SRGB_RTV;
                    break;
                case F::R8G8B8A8:
                    s_swapchain_fmt_str  = "R8G8B8A8_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  Rec.709";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::B8G8R8A8:
                    s_swapchain_fmt_str  = "B8G8R8A8_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  Rec.709";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::B8G8R8X8:
                    s_swapchain_fmt_str  = "B8G8R8X8_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  Rec.709  (no alpha)";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::R8G8B8:
                    s_swapchain_fmt_str  = "R8G8B8";
                    s_swapchain_type_str = "SDR  |  sRGB  |  Rec.709  (24-bit packed)";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::X8R8G8B8:
                    s_swapchain_fmt_str  = "X8R8G8B8";
                    s_swapchain_type_str = "SDR  |  sRGB  |  Rec.709";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::A8R8G8B8:
                    s_swapchain_fmt_str  = "A8R8G8B8";
                    s_swapchain_type_str = "SDR  |  sRGB  |  Rec.709";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                // ---- 16-bit SDR formats (legacy/DX9) -------------------------------
                case F::R5G6B5:
                    s_swapchain_fmt_str  = "R5G6B5_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  16-bit RGB565";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::B5G6R5:
                    s_swapchain_fmt_str  = "B5G6R5_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  16-bit BGR565";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::X1R5G5B5:
                    s_swapchain_fmt_str  = "X1R5G5B5_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  16-bit 1555";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::A1R5G5B5:
                    s_swapchain_fmt_str  = "A1R5G5B5_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  16-bit 1555";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                case F::B5G5R5A1:
                    s_swapchain_fmt_str  = "B5G5R5A1_UNORM";
                    s_swapchain_type_str = "SDR  |  sRGB  |  16-bit 5551";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
                default:
                    s_swapchain_fmt_str  = "Unknown format";
                    s_swapchain_type_str = "Unknown";
                    s_swapchain_cs = SCS_SDR_UNORM;
                    break;
            }
        }
        auto* r2 = static_cast<InGameOverlay::RendererHook_t*>(user);
        r2->SetScreenshotCallback(nullptr, nullptr);
        r2->TakeScreenshot(InGameOverlay::ScreenshotType_t::None);
    }, r);
    r->TakeScreenshot(InGameOverlay::ScreenshotType_t::BeforeOverlay);
}

static constexpr int max_window_id = 10000;
static constexpr int base_notif_window_id  = 0 * max_window_id;
static constexpr int base_friend_window_id = 1 * max_window_id;
static constexpr int base_friend_item_id   = 2 * max_window_id;

// look for the column 'API language code' here: https://partner.steamgames.com/doc/store/localization/languages
static constexpr const char* valid_languages[] = {
    "english",
    "arabic",
    "bulgarian",
    "schinese",
    "tchinese",
    "czech",
    "danish",
    "dutch",
    "finnish",
    "french",
    "german",
    "greek",
    "hungarian",
    "italian",
    "japanese",
    "koreana",
    "norwegian",
    "polish",
    "portuguese",
    "brazilian",
    "romanian",
    "russian",
    "spanish",
    "latam",
    "swedish",
    "thai",
    "turkish",
    "ukrainian",
    "vietnamese",
    "croatian",
    "indonesian",
};


// ListBoxHeader() is deprecated and inlined inside <imgui.h>
// Helper to calculate size from items_count and height_in_items
static inline bool ImGuiHelper_BeginListBox(const char* label, int items_count) {
    int min_items = items_count < 7 ? items_count : 7;
    float height = ImGui::GetTextLineHeightWithSpacing() * (min_items + 0.25f) + ImGui::GetStyle().FramePadding.y * 2.0f;
    return ImGui::BeginListBox(label, ImVec2(0.0f, height));
}


void Steam_Overlay::overlay_run_callback(void* object)
{
    // PRINT_DEBUG_ENTRY();
    Steam_Overlay* _this = reinterpret_cast<Steam_Overlay*>(object);
    _this->steam_run_callback();
}

void Steam_Overlay::overlay_networking_callback(void* object, Common_Message* msg)
{
    Steam_Overlay* _this = reinterpret_cast<Steam_Overlay*>(object);
    _this->networking_msg_received(msg);
}

// Windows VK code for an ingame_overlay toggle key. Mirrors ToggleKeyToNativeKey()
// in the ingame_overlay dependency's WindowsHook.cpp, so the bridge reports the
// exact keys the native hotkey listens for.
//
// This translation unit is also built on Linux, where the VK_* virtual-key codes do
// not exist. The bridge field carrying them is read by the ReShade addon, which is
// Windows-only, so elsewhere the mapping degrades to 0 rather than pulling in a table
// of Windows constants that nothing on that platform would read.
static int toggle_key_to_vk(InGameOverlay::ToggleKey key)
{
#ifndef __WINDOWS__
    (void)key;
    return 0;
#else
    switch (key) {
        case InGameOverlay::ToggleKey::SHIFT: return VK_SHIFT;
        case InGameOverlay::ToggleKey::CTRL:  return VK_CONTROL;
        case InGameOverlay::ToggleKey::ALT:   return VK_MENU;
        case InGameOverlay::ToggleKey::TAB:   return VK_TAB;
        case InGameOverlay::ToggleKey::F1:    return VK_F1;
        case InGameOverlay::ToggleKey::F2:    return VK_F2;
        case InGameOverlay::ToggleKey::F3:    return VK_F3;
        case InGameOverlay::ToggleKey::F4:    return VK_F4;
        case InGameOverlay::ToggleKey::F5:    return VK_F5;
        case InGameOverlay::ToggleKey::F6:    return VK_F6;
        case InGameOverlay::ToggleKey::F7:    return VK_F7;
        case InGameOverlay::ToggleKey::F8:    return VK_F8;
        case InGameOverlay::ToggleKey::F9:    return VK_F9;
        case InGameOverlay::ToggleKey::F10:   return VK_F10;
        case InGameOverlay::ToggleKey::F11:   return VK_F11;
        case InGameOverlay::ToggleKey::F12:   return VK_F12;
        default: return 0;
    }
#endif
}

// Display name for a toggle key, used to build the bridge's human-readable label.
static const char *toggle_key_name(InGameOverlay::ToggleKey key)
{
    switch (key) {
        case InGameOverlay::ToggleKey::SHIFT: return "SHIFT";
        case InGameOverlay::ToggleKey::CTRL:  return "CTRL";
        case InGameOverlay::ToggleKey::ALT:   return "ALT";
        case InGameOverlay::ToggleKey::TAB:   return "TAB";
        case InGameOverlay::ToggleKey::F1:    return "F1";
        case InGameOverlay::ToggleKey::F2:    return "F2";
        case InGameOverlay::ToggleKey::F3:    return "F3";
        case InGameOverlay::ToggleKey::F4:    return "F4";
        case InGameOverlay::ToggleKey::F5:    return "F5";
        case InGameOverlay::ToggleKey::F6:    return "F6";
        case InGameOverlay::ToggleKey::F7:    return "F7";
        case InGameOverlay::ToggleKey::F8:    return "F8";
        case InGameOverlay::ToggleKey::F9:    return "F9";
        case InGameOverlay::ToggleKey::F10:   return "F10";
        case InGameOverlay::ToggleKey::F11:   return "F11";
        case InGameOverlay::ToggleKey::F12:   return "F12";
        default: return "?";
    }
}

void Steam_Overlay::parse_key_combo()
{
    static const std::unordered_map<InGameOverlay::ToggleKey, std::string_view> KEYS_MAP {
        { InGameOverlay::ToggleKey::SHIFT, "shift" },
        { InGameOverlay::ToggleKey::CTRL,  "ctrl"  },
        { InGameOverlay::ToggleKey::ALT,   "alt"   },
        { InGameOverlay::ToggleKey::TAB,   "tab"   },
        { InGameOverlay::ToggleKey::F1,    "fn1"   },
        { InGameOverlay::ToggleKey::F2,    "fn2"   },
        { InGameOverlay::ToggleKey::F3,    "fn3"   },
        { InGameOverlay::ToggleKey::F4,    "fn4"   },
        { InGameOverlay::ToggleKey::F5,    "fn5"   },
        { InGameOverlay::ToggleKey::F6,    "fn6"   },
        { InGameOverlay::ToggleKey::F7,    "fn7"   },
        { InGameOverlay::ToggleKey::F8,    "fn8"   },
        { InGameOverlay::ToggleKey::F9,    "fn9"   },
        { InGameOverlay::ToggleKey::F10,   "fn10"  },
        { InGameOverlay::ToggleKey::F11,   "fn11"  },
        { InGameOverlay::ToggleKey::F12,   "fn12"  },
    };

    std::unordered_set<InGameOverlay::ToggleKey> keys_combo{};
    bool use_default = false;
    if (settings->overlay_toggle_keys.empty()) {
        use_default = true;
    } else {
        for (const auto &key_name : settings->overlay_toggle_keys) {
            auto key_it = std::find_if(KEYS_MAP.cbegin(), KEYS_MAP.cend(), [&key_name](decltype(*KEYS_MAP.cbegin()) const &item) {
                return common_helpers::str_cmp_insensitive(item.second, key_name);
            });
            if (KEYS_MAP.cend() != key_it) {
                keys_combo.insert(key_it->first);
            } else {
                use_default = true;
                PRINT_DEBUG("[X] Unknown key '%s', using default key combo Shift + Tab", key_name.c_str());
                break;
            }
        }
    }

    if (use_default) {
        toggle_keys = {
            InGameOverlay::ToggleKey::SHIFT, InGameOverlay::ToggleKey::TAB
        };
    } else {
        toggle_keys = std::vector<InGameOverlay::ToggleKey>(keys_combo.begin(), keys_combo.end());
    }
}

void Steam_Overlay::parse_screenshot_key_combo()
{
    static const std::unordered_map<InGameOverlay::ToggleKey, std::string_view> KEYS_MAP {
        { InGameOverlay::ToggleKey::SHIFT, "shift" },
        { InGameOverlay::ToggleKey::CTRL,  "ctrl"  },
        { InGameOverlay::ToggleKey::ALT,   "alt"   },
        { InGameOverlay::ToggleKey::TAB,   "tab"   },
        { InGameOverlay::ToggleKey::F1,    "fn1"   },
        { InGameOverlay::ToggleKey::F2,    "fn2"   },
        { InGameOverlay::ToggleKey::F3,    "fn3"   },
        { InGameOverlay::ToggleKey::F4,    "fn4"   },
        { InGameOverlay::ToggleKey::F5,    "fn5"   },
        { InGameOverlay::ToggleKey::F6,    "fn6"   },
        { InGameOverlay::ToggleKey::F7,    "fn7"   },
        { InGameOverlay::ToggleKey::F8,    "fn8"   },
        { InGameOverlay::ToggleKey::F9,    "fn9"   },
        { InGameOverlay::ToggleKey::F10,   "fn10"  },
        { InGameOverlay::ToggleKey::F11,   "fn11"  },
        { InGameOverlay::ToggleKey::F12,   "fn12"  },
    };

    std::unordered_set<InGameOverlay::ToggleKey> keys_combo{};
    bool use_default = false;
    if (settings->overlay_screenshot_keys.empty()) {
        use_default = true;
    } else {
        for (const auto &key_name : settings->overlay_screenshot_keys) {
            auto key_it = std::find_if(KEYS_MAP.cbegin(), KEYS_MAP.cend(), [&key_name](decltype(*KEYS_MAP.cbegin()) const &item) {
                return common_helpers::str_cmp_insensitive(item.second, key_name);
            });
            if (KEYS_MAP.cend() != key_it) {
                keys_combo.insert(key_it->first);
            } else {
                use_default = true;
                PRINT_DEBUG("[X] Unknown screenshot key '%s', using default F12", key_name.c_str());
                break;
            }
        }
    }

    if (use_default) {
        screenshot_keys = {
            InGameOverlay::ToggleKey::F12
        };
    } else {
        screenshot_keys = std::vector<InGameOverlay::ToggleKey>(keys_combo.begin(), keys_combo.end());
    }
}

Steam_Overlay::Steam_Overlay(Settings* settings, Local_Storage *local_storage, SteamCallResults* callback_results, SteamCallBacks* callbacks, RunEveryRunCB* run_every_runcb, Networking* network, PlaytimeCounter* playtime_counter) :
    settings(settings),
    local_storage(local_storage),
    callback_results(callback_results),
    callbacks(callbacks),
    run_every_runcb(run_every_runcb),
    network(network),
    playtime_counter(playtime_counter),
    stats(Steam_Overlay_Stats(settings))
{
    // don't even bother initializing the overlay
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    // Only create renderer worker threads when the native overlay is enabled
    if (!settings->disable_overlay) {
    renderer_hook_init_thread = common_helpers::KillableWorker(
        [this](void *){ return renderer_hook_proc(); },
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(renderer_detector_polling_ms),
        [this] { return !setup_overlay_called; }
    );

    renderer_detector_delay_thread = common_helpers::KillableWorker(
        [this](void *){
            request_renderer_detector();
            set_renderer_hook_timeout();
            renderer_hook_init_thread.start();
            return true;
        },
        std::chrono::milliseconds(settings->overlay_hook_delay_sec * 1000),
        std::chrono::milliseconds(0),
        [this] { return !setup_overlay_called; }
    );
    } // !disable_overlay

    parse_key_combo();
    parse_screenshot_key_combo();
    strncpy(username_text, settings->get_local_name(), sizeof(username_text));

    // we need these copies to show the warning only once, then disable the flag
    // avoid manipulating settings->xxx
    this->warn_local_save =
        !settings->disable_overlay_warning_any && !settings->disable_overlay_warning_local_save && settings->overlay_warn_local_save;
    this->warn_bad_appid =
        !settings->disable_overlay_warning_any && !settings->disable_overlay_warning_bad_appid && settings->get_local_game_id().AppID() == 0;

    current_language = 0;
    const char *language = settings->get_overlay_language();

    show_user_info = settings->overlay_always_show_user_info;
    show_notification_history = settings->overlay_appearance.show_notification_history;
    show_achievements = settings->overlay_appearance.show_achievement_list;

    int i = 0;
    for (auto &lang : valid_languages) {
        if (common_helpers::str_cmp_insensitive(lang, language)) {
            current_language = i;
            break;
        }

        ++i;
    }

    this->network->setCallback(CALLBACK_ID_STEAM_MESSAGES, settings->get_local_steam_id(), &Steam_Overlay::overlay_networking_callback, this);
    this->run_every_runcb->add(&Steam_Overlay::overlay_run_callback, this);
}

Steam_Overlay::~Steam_Overlay()
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    UnSetupOverlay();

    this->network->rmCallback(CALLBACK_ID_STEAM_MESSAGES, settings->get_local_steam_id(), &Steam_Overlay::overlay_networking_callback, this);
    this->run_every_runcb->remove(&Steam_Overlay::overlay_run_callback, this);
}

void Steam_Overlay::request_renderer_detector()
{
    PRINT_DEBUG_ENTRY();
    // request renderer detection
    // DetectRenderer() is a stateful poller: this starts the detection, and it has to be
    // called again (see renderer_hook_proc()) until it reports that detection is done.
    InGameOverlay::DetectRenderer();
}

void Steam_Overlay::set_renderer_hook_timeout()
{
    renderer_hook_timeout_ctr = settings->overlay_renderer_detector_timeout_sec /*seconds*/ * 1000 /*milli per second*/ / renderer_detector_polling_ms;
}

void Steam_Overlay::cleanup_renderer_hook()
{
    InGameOverlay::StopRendererDetection();
    InGameOverlay::FreeDetector();
}

bool Steam_Overlay::renderer_hook_proc()
{
    // Drive the detection. It returns false while it still needs to run, and true once it
    // has finished, whether or not a renderer was found.
    const bool detection_done = InGameOverlay::DetectRenderer(/*restart*/ false);

    if (renderer_hook_timeout_ctr > 0 && !detection_done) {
        return false;
    }

    // Fetch the hook before releasing the detector: FreeDetector() destroys the detector
    // (and its hooks), so GetDetectedRenderer() has to be queried while it is still alive.
    InGameOverlay::RendererHook_t* detected_renderer =
        detection_done ? InGameOverlay::GetDetectedRenderer() : nullptr;

    // free detector resources and check for failure
    cleanup_renderer_hook();
    // exit on failure
    // again check for 'setup_overlay_called' to be extra sure that the overlay wasn't deinitialized
    if (!setup_overlay_called || !detection_done || renderer_hook_timeout_ctr <= 0) {
        PRINT_DEBUG("failed to detect renderer, ctr=%i, overlay was set up=%i",
            renderer_hook_timeout_ctr, (int)setup_overlay_called
        );
        return true;
    }

    // do a one time initialization
    // std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    _renderer = detected_renderer;
    if (!_renderer) { // is this even possible?
        PRINT_DEBUG("renderer hook was null!");
        return true;
    }
    PRINT_DEBUG("got renderer hook %p for '%s'", _renderer, _renderer->GetLibraryName());

    // note: make sure to load all relevant strings before creating the font(s), otherwise some glyphs ranges will be missing
    load_achievements_data();
    load_audio();
    create_fonts();

    // setup renderer callbacks
    auto overlay_toggle_callback = [this]() { open_overlay_hook(true); };
    _renderer->OverlayProc = [this]() { overlay_render_proc(); };
    _renderer->OverlayHookReady = [this](InGameOverlay::OverlayHookState state) {
        PRINT_DEBUG("hook state changed to <%i>", (int)state);
        bool is_ready = (state == InGameOverlay::OverlayHookState::Ready || state == InGameOverlay::OverlayHookState::Reset);
        overlay_state_hook(is_ready);

        bool clean_up = (state == InGameOverlay::OverlayHookState::Reset ||
                         state == InGameOverlay::OverlayHookState::Removing);
        if (clean_up) {
            // Defer the display-info query to the next rendered frame so we don't
            // block the render thread (which is inside ResizeBuffers/device-release)
            // with synchronous Windows display-config API calls.
            s_sdr_scale_queried   = false;
            s_pending_sdr_refresh = true;

            // Achievement icon GPU resources are now invalid.
            // The decoded-pixel caches hold pixels processed with the OLD LUT.  Clear them so
            // try_load_ach_icon() re-decodes from raw source data with the updated LUT on next use.
            for (auto &ach : achievements) {
                ach.icon_decoded_data.clear();
                ach.icon_gray_decoded_data.clear();
            }
            // Reset the paginated-upload cursor so re-uploads start from the first achievement.
            last_loaded_ach_icon = 0;

            // SCE asset textures: free the whole cache so they are re-loaded and re-decoded lazily.
            sce_textures_free_all();

            // Reset texture cache tracking so the per-frame invalidation check
            // in overlay_render_proc() doesn't trigger a redundant second flush.
            s_cached_tex_cs    = SCS_UNKNOWN;
            s_cached_tex_scale = 1.0f;
            s_cached_tex_brightness = 1.0f;
            s_cached_tex_contrast   = 1.0f;
            s_cached_tex_gamma_adj  = 1.0f;
        }

        if (is_ready && settings->overlay_appearance.image_gamma == Overlay_Appearance::SrgbDecode::Auto) {
            using RHT = InGameOverlay::RendererHookType_t;
            auto rtype = _renderer->GetRendererHookType();
            bool renderer_needs_decode = (rtype == RHT::DirectX10 || rtype == RHT::DirectX11 ||
                                          rtype == RHT::DirectX12 || rtype == RHT::Vulkan || rtype == RHT::Metal);
            if (renderer_needs_decode) {
                // Cancel any in-flight screenshot first, then re-arm.
                // The Reset fires BEFORE ingame_overlay destroys its render targets;
                // cancelling avoids a stale BeforeOverlay screenshot request crossing the resize.
                _renderer->SetScreenshotCallback(nullptr, nullptr);
                _renderer->TakeScreenshot(InGameOverlay::ScreenshotType_t::None);
                // One-shot screenshot to detect the actual back-buffer format.
                // BeforeOverlay fires before OverlayProc in the same frame,
                // so s_swapchain_cs is set before any AttachResource call.
                arm_swapchain_format_detect(_renderer);
            }
        }
    };

    bool started = _renderer->StartHook(overlay_toggle_callback, toggle_keys.data(), (int)toggle_keys.size(), &fonts_atlas);
    PRINT_DEBUG("started renderer hook (result=%i)", (int)started);

    // Register screenshot callback
    _renderer->SetScreenshotCallback(&Steam_Overlay::on_screenshot_captured, this);

    // Wire up TriggerScreenshot API to use the overlay renderer
    get_steam_client()->steam_screenshots->overlay_take_screenshot = [this]() {
        if (_renderer) _renderer->TakeScreenshot(InGameOverlay::ScreenshotType_t::BeforeOverlay);
    };

    return true;
}

// note: make sure to load all relevant strings before creating the font(s), otherwise some glyphs ranges will be missing
void Steam_Overlay::create_fonts()
{
    PRINT_DEBUG_ENTRY();

    // disable rounding the texture height to the next power of two
    // see this: https://github.com/ocornut/imgui/blob/master/docs/FONTS.md#4-font-atlas-texture-fails-to-upload-to-gpu
    fonts_atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

    float font_size = settings->overlay_appearance.font_size;
    float font_size_fps = settings->overlay_appearance.font_size_fps > 0.0f
        ? settings->overlay_appearance.font_size_fps
        : font_size;
    float font_size_ach_title = settings->overlay_appearance.font_size_ach_title > 0.0f
        ? settings->overlay_appearance.font_size_ach_title
        : font_size;
    float font_size_ach_desc = settings->overlay_appearance.font_size_ach_desc > 0.0f
        ? settings->overlay_appearance.font_size_ach_desc
        : font_size;

    font_cfg.FontDataOwnedByAtlas = false; // https://github.com/ocornut/imgui/blob/master/docs/FONTS.md#loading-font-data-from-memory
    font_cfg.PixelSnapH = true;
    font_cfg.OversampleH = 1;
    font_cfg.OversampleV = 1;
    font_cfg.SizePixels = font_size;
    // non-latin characters look ugly and squeezed without this horizontal spacing
    font_cfg.GlyphExtraAdvanceX = settings->overlay_appearance.font_glyph_extra_spacing_x;
    // Y-axis spacing removed: ImGui replaced GlyphExtraSpacing (ImVec2) with GlyphExtraAdvanceX (float) in 2025
    // font_cfg.GlyphExtraSpacing.x = settings->overlay_appearance.font_glyph_extra_spacing_x; 
    // font_cfg.GlyphExtraSpacing.y = settings->overlay_appearance.font_glyph_extra_spacing_y;

    for (const auto &ach : achievements) {
        font_builder.AddText(ach.title.c_str());
        font_builder.AddText(ach.description.c_str());
    }
    for (int i = 0; i < TRANSLATION_NUMBER_OF_LANGUAGES; i++) {
        font_builder.AddText(translationChat[i]);
        font_builder.AddText(translationCopyId[i]);
        font_builder.AddText(translationTestAchievement[i]);
        font_builder.AddText(translationInvite[i]);
        font_builder.AddText(translationInviteAll[i]);
        font_builder.AddText(translationJoin[i]);
        font_builder.AddText(translationInvitedYouToJoinTheGame[i]);
        font_builder.AddText(translationAccept[i]);
        font_builder.AddText(translationRefuse[i]);
        font_builder.AddText(translationSend[i]);
        font_builder.AddText(translationUserPlaying[i]);
        font_builder.AddText(translationTotalTime[i]);
        font_builder.AddText(translationTotalTimeText[i]);
        font_builder.AddText(translationRenderer[i]);
        font_builder.AddText(translationShowAchievements[i]);
        font_builder.AddText(translationSettings[i]);
        font_builder.AddText(translationHistory[i]);
		font_builder.AddText(translationScreenshots[i]);
        font_builder.AddText(translationFriends[i]);
        font_builder.AddText(translationNoNotification[i]);
        font_builder.AddText(translationClearAll[i]);
        font_builder.AddText(translationHistoryChat[i]);
        font_builder.AddText(translationHistoryInvite[i]);
        font_builder.AddText(translationHistoryAchievement[i]);
        font_builder.AddText(translationHistoryProgress[i]);
        font_builder.AddText(translationHistoryAutoInvite[i]);
        font_builder.AddText(translationHistoryScreenshot[i]);
        font_builder.AddText(translationAchievementWindow[i]);
        font_builder.AddText(translationListOfAchievements[i]);
        font_builder.AddText(translationAchievements[i]);
        font_builder.AddText(translationHiddenAchievement[i]);
        font_builder.AddText(translationShow[i]);
        font_builder.AddText(translationAchievedOn[i]);
        font_builder.AddText(translationUnlocked[i]);
        font_builder.AddText(translationNoUnlockedAchievements[i]);
        font_builder.AddText(translationLocked[i]);
        font_builder.AddText(translationAllAchievementsUnlocked[i]);
        font_builder.AddText(translationNotAchieved[i]);
        font_builder.AddText(translationGlobalAchievementPercent[i]);
        font_builder.AddText(translationGlobalSettingsWindow[i]);
        font_builder.AddText(translationGlobalSettingsWindowDescription[i]);
        font_builder.AddText(translationUsername[i]);
        font_builder.AddText(translationLanguage[i]);
        font_builder.AddText(translationSelectedLanguage[i]);
        font_builder.AddText(translationRestartTheGameToApply[i]);
        font_builder.AddText(translationSave[i]);
        font_builder.AddText(translationWarning[i]);
        font_builder.AddText(translationWarningDescription_badAppid[i]);
        font_builder.AddText(translationWarningDescription_localSave[i]);
        font_builder.AddText(translationSteamOverlayURL[i]);
        font_builder.AddText(translationClose[i]);
        font_builder.AddText(translationPlaying[i]);
		font_builder.AddText(translationScreenshotSaved[i]);
		font_builder.AddText(translationUnpinAll[i]);
		font_builder.AddText(translationDeleteSelected[i]);
		font_builder.AddText(translationOpenFolder[i]);
		font_builder.AddText(translationNoScreenshotsYet[i]);
		font_builder.AddText(translationDelete[i]);
		font_builder.AddText(translationScreenshotPreview[i]);
		font_builder.AddText(translationPrev[i]);
		font_builder.AddText(translationPin[i]);
		font_builder.AddText(translationCrop[i]);
		font_builder.AddText(translationNext[i]);
		font_builder.AddText(translationDeleteThisScreenshot[i]);
		font_builder.AddText(translationDeleteAllScelectedScreenshots[i]);
		font_builder.AddText(translationYes[i]);
		font_builder.AddText(translationNo[i]);
		font_builder.AddText(translationConfirmDelete[i]);
		font_builder.AddText(translationConfirm[i]);
		font_builder.AddText(translationCancel[i]);
		font_builder.AddText(translationPinnedScreenshots[i]);
		font_builder.AddText(translationOpacity[i]);
        font_builder.AddText(translationAutoAcceptFriendInvite[i]);
        font_builder.AddText(translationFpsCheckbox[i]);
        font_builder.AddText(translationFpsDisplay[i]);
        font_builder.AddText(translationFrametimeCheckbox[i]);
        font_builder.AddText(translationFrametimeDisplay[i]);
        font_builder.AddText(translationFrametimeUnitDisplay[i]);
        font_builder.AddText(translationPlaytimeCheckbox[i]);
        font_builder.AddText(translationPlaytimeDisplay[i]);
    }
    font_builder.AddRanges(fonts_atlas.GetGlyphRangesDefault());
    font_builder.AddChar((ImWchar)0x2713); // ✓ CHECK MARK
    font_builder.AddChar((ImWchar)0x2717); // ✗ BALLOT X
    font_builder.AddChar((ImWchar)0x25B6); // ▶ BLACK RIGHT-POINTING TRIANGLE (in-progress)

    font_builder.BuildRanges(&ranges);
    font_cfg.GlyphRanges = ranges.Data;

    auto add_overlay_font = [this](float size, const std::string &custom_font = "") {
        font_cfg.SizePixels = size;
        font_cfg.MergeMode = false;

        const std::string &font_path = custom_font.empty() ? settings->overlay_appearance.font_override : custom_font;
        ImFont *font = nullptr;
        if (font_path.size()) {
            font = fonts_atlas.AddFontFromFileTTF(font_path.c_str(), size, &font_cfg);
            if (font) {
                font_cfg.MergeMode = true; // merge next font into the custom font
            }
        }

        // note: base85 compressed arrays caused a compiler heap allocation error, regular compression is more guaranteed
        ImFont *fallback_font = fonts_atlas.AddFontFromMemoryCompressedTTF(unifont_compressed_data, unifont_compressed_size, size, &font_cfg);
        return font ? font : fallback_font;
    };

    font_notif = font_default = add_overlay_font(font_size);
    font_fps = add_overlay_font(font_size_fps);
    font_ach_title = add_overlay_font(font_size_ach_title, settings->overlay_appearance.font_override_ach_title);
    font_ach_desc = add_overlay_font(font_size_ach_desc, settings->overlay_appearance.font_override_ach_desc);
    stats.font = font_fps;

    // note: base85 compressed arrays caused a compiler heap allocation error, regular compression is more guaranteed
    ImFont *font = fonts_atlas.AddFontFromMemoryCompressedTTF(unifont_compressed_data, unifont_compressed_size, font_size, &font_cfg);
    font_notif = font_default = font;
    stats.font = font;
    
    // With ImGui 1.92+ and ImGuiBackendFlags_RendererHasTextures, the backend
    // builds the font atlas automatically — no need to call Build() manually.
    // bool res = fonts_atlas.IsBuilt();
    PRINT_DEBUG("fonts atlas successfully built...");

    reset_LastError();
}

void Steam_Overlay::load_audio()
{
    PRINT_DEBUG_ENTRY();

    for (auto &kv : wav_files) {
        std::string file_path{};
        unsigned int file_size{};

        // try local location first, then try global location
        for (const auto &settings_path : { Local_Storage::get_game_settings_path(), local_storage->get_global_settings_path() }) {
            file_path = settings_path + Steam_Overlay::ACH_SOUNDS_FOLDER + PATH_SEPARATOR + kv.first;
            file_size = file_size_(file_path);
            if (file_size) break;
        }

        kv.second.clear();
        if (file_size) {
            kv.second.assign(file_size + 1, 0); // +1 because this will be treated as a null-terminated string later
            int read = Local_Storage::get_file_data(file_path, (char *)&kv.second[0], file_size);
            if (read <= 0) kv.second.clear();
            PRINT_DEBUG("loaded '%s' (read %i/%u bytes)", file_path.c_str(), read, file_size);
        }
    }

    // Cascade inheritance: populate empty specific-type slots from their nearest generic fallback.
    // This means a user only needs overlay_friend_notification.wav and all friend/lobby types
    // will automatically use it, unless they provide a more specific file.
    auto inherit = [](std::vector<char> &dst, const std::vector<char> &src) {
        if (dst.empty() && !src.empty()) dst = src;
    };

    auto& friend_gen  = wav_files.at("overlay_friend_notification.wav");
    auto& ach_unlock  = wav_files.at("overlay_achievement_notification.wav");
    auto& join_resp   = wav_files.at("overlay_lobby_join_response.wav");
    auto& join_acc    = wav_files.at("overlay_lobby_join_accepted.wav");
    auto& join_deny   = wav_files.at("overlay_lobby_join_denied.wav");
    auto& ach_prog    = wav_files.at("overlay_achievement_progress.wav");

    // join_response is the intermediate for accepted/denied — fill it from friend first
    inherit(join_resp, friend_gen);
    // now accepted/denied inherit from join_response (which may itself be the friend sound)
    inherit(join_acc,  join_resp);
    inherit(join_deny, join_resp);
    // achievement progress inherits from achievement unlock
    inherit(ach_prog,  ach_unlock);
    // all remaining friend-derived types inherit directly from the friend generic
    for (const char* key : {
        "overlay_invite_notification.wav",
        "overlay_chat_notification.wav",
        "overlay_auto_accept_notification.wav",
        "overlay_lobby_join_request.wav",
        "overlay_lobby_kicked.wav",
        "overlay_friend_lobby.wav",
        "overlay_lobby_status.wav",
    }) {
        inherit(wav_files.at(key), friend_gen);
    }
}

void Steam_Overlay::load_achievements_data()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    Steam_User_Stats* steamUserStats = get_steam_client()->steam_user_stats;
    uint32 achievements_num = steamUserStats->GetNumAchievements();
    for (uint32 i = 0; i < achievements_num; ++i) {
        Overlay_Achievement ach{};
        ach.name = steamUserStats->GetAchievementName(i);
        ach.title = steamUserStats->GetAchievementDisplayAttribute(ach.name.c_str(), "name");
        ach.description = steamUserStats->GetAchievementDisplayAttribute(ach.name.c_str(), "desc");

        const char *hidden = steamUserStats->GetAchievementDisplayAttribute(ach.name.c_str(), "hidden");
        ach.hidden = hidden && hidden[0] == '1';

        ach.unlock_percentage = static_cast<float>(
            steamUserStats->GetAchievementUnlockPercentage(ach.name.c_str()));

        bool achieved = false;
        uint32 unlock_time = 0;
        if (steamUserStats->GetAchievementAndUnlockTime(ach.name.c_str(), &achieved, &unlock_time)) {
            ach.achieved = achieved;
            ach.unlock_time = unlock_time;
        } else {
            ach.achieved = false;
            ach.unlock_time = 0;
        }

        float pnMinProgress = 0, pnMaxProgress = 0;
        if (steamUserStats->GetAchievementProgressLimits(ach.name.c_str(), &pnMinProgress, &pnMaxProgress)) {
            ach.progress = (uint32)pnMinProgress;
            ach.max_progress = (uint32)pnMaxProgress;
        }

        if (_renderer) {
            if (ach.icon == nullptr) {
                ach.icon = _renderer->CreateResource();
            }
            if (ach.icon_gray == nullptr) {
                ach.icon_gray = _renderer->CreateResource();
            }
        }

        achievements.emplace_back(ach);

        if (!setup_overlay_called) return;
    }

    // save a snapshot for the Reset button
    achievements_snapshot.clear();
    for (const auto &a : achievements) {
        achievements_snapshot.push_back({ a.name, a.achieved, a.progress, a.unlock_time });
    }

    PRINT_DEBUG("count=%u, loaded=%zu", achievements_num, achievements.size());

    if (steamUserStats->global_achievement_percentages_populated) {
        // data already available (e.g. game called the API before overlay init)
        if (settings->overlay_achievement_sort_by_global_percent) {
            SortAchievementsByGlobalPercent(steamUserStats->global_achievement_percentages);
        } else {
            ach_global_percentages = steamUserStats->global_achievement_percentages;
        }
        ach_global_percentages_snapshot = ach_global_percentages;
    } else {
        // trigger fetch if the game hasn't done so yet; result arrives via steam_run_callback
        steamUserStats->RequestGlobalAchievementPercentages();
    }

    // Always trigger SteamHunters fetch once per launch (groups + supplemental % fallback)
    steamUserStats->RequestSteamHuntersData();
}

void Steam_Overlay::SortAchievementsByGlobalPercent(const std::map<std::string, float> &percentages)
{
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    ach_global_percentages = percentages;
    ach_global_percentages_snapshot = percentages;
    // Match Steam's display order:
    //   1. Unlocked achievements, sorted by unlock time descending (most recent first)
    //   2. Locked achievements, sorted by global percentage descending (easiest first)
    std::stable_sort(achievements.begin(), achievements.end(),
        [&percentages](const Overlay_Achievement &a, const Overlay_Achievement &b) {
            if (a.achieved != b.achieved) return a.achieved > b.achieved; // unlocked before locked
            if (a.achieved) return a.unlock_time > b.unlock_time;          // unlocked: most recent first
            // both locked: higher global % first
            auto ita = percentages.find(a.name);
            auto itb = percentages.find(b.name);
            float pa = (ita != percentages.end()) ? ita->second : -1.0f;
            float pb = (itb != percentages.end()) ? itb->second : -1.0f;
            return pa > pb;
        }
    );
    PRINT_DEBUG("achievements re-sorted (unlocked by recency, locked by global %)");
}

// Called from Steam_User_Stats background thread once SteamHunters data has arrived.
// Picks up any newly populated global percentages (in case Steam API hadn't returned yet).
void Steam_Overlay::UpdateSteamHuntersData()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    Steam_User_Stats *steamUserStats = get_steam_client()->steam_user_stats;
    if (!steamUserStats->global_achievement_percentages_populated) return;

    // If we don't have percentages yet in the overlay, grab them now
    if (ach_global_percentages.empty()) {
        if (settings->overlay_achievement_sort_by_global_percent) {
            SortAchievementsByGlobalPercent(steamUserStats->global_achievement_percentages);
        } else {
            ach_global_percentages = steamUserStats->global_achievement_percentages;
            ach_global_percentages_snapshot = ach_global_percentages;
        }
        PRINT_DEBUG("UpdateSteamHuntersData: overlay percentages updated from SteamHunters fallback");
    }
}

void Steam_Overlay::NotifySceAssetsReady(uint32_t downloaded, uint32_t skipped, uint32_t total)
{
    PRINT_DEBUG("SceAssets: downloaded=%u skipped=%u total=%u", downloaded, skipped, total);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    sce_asset_progress.downloaded           = downloaded;
    sce_asset_progress.skipped              = skipped;
    sce_asset_progress.total                = total;
    sce_asset_progress.pending_notification = true;
    // request a frame render so the notification is shown promptly
    allow_renderer_frame_processing(true);
}

// called initially and when window size is updated
void Steam_Overlay::overlay_state_hook(bool ready)
{
    PRINT_DEBUG("%i", (int)ready);

    // NOTE usage of local objects here cause an exception when this is called with false state
    // the reason is that by the time this hook is called, the object may have been already destructed
    // this is why we use global mutex
    // TODO this also doesn't seem right, no idea why it happens though
    // NOTE after initializing the renderer detector on another thread this was solved
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!setup_overlay_called) return;

    is_ready = ready;

    if (ready) {
        // Antichamber may crash here because ImGui Context is null!, no idea why
        bool not_yet = false;
        if (ImGui::GetCurrentContext() && late_init_imgui.compare_exchange_weak(not_yet, true)) {
            PRINT_DEBUG("late init ImGui");

            ImGuiIO &io = ImGui::GetIO();
            // disable loading the default ini file
            io.IniFilename = NULL;

            ImGuiStyle &style = ImGui::GetStyle();
            // Disable round window
            style.WindowRounding = 0.0;
        }
    }
}

// called when the user presses SHIFT + TAB
bool Steam_Overlay::open_overlay_hook(bool toggle)
{
    if (toggle) {
        ShowOverlay(!show_overlay);
    }

    return show_overlay;
}

void Steam_Overlay::allow_renderer_frame_processing(bool state, bool cleaning_up_overlay)
{
    // this is very important internally it calls the necessary fuctions
    // to properly update ImGui window size on the next overlay_render_proc() call

    // In bridge-only mode _renderer is null — nothing to do.
    if (!_renderer) return;

    if (state) {
        auto new_val = ++renderer_frame_processing_requests;
        if (new_val == 1) { // only take an action on first request
            // allow internal frmae processing
            _renderer->HideOverlayInputs(false);
            PRINT_DEBUG("enabled frame processing (count=%u)", new_val);
        }
    } else {
        if (renderer_frame_processing_requests > 0) {
            auto new_val = --renderer_frame_processing_requests;
            if (!new_val || cleaning_up_overlay) { // only take an action when the requests reach 0 or by force
                _renderer->HideOverlayInputs(true);
                PRINT_DEBUG("disabled frame processing (count=%u, force=%i)", new_val, (int)cleaning_up_overlay);
            }
        }
    }
}

void Steam_Overlay::obscure_game_input(bool state) {
    // In bridge-only mode _renderer is null and ImGui context is not ours.
    if (!_renderer) return;

    if (state) {
        auto new_val = ++obscure_cursor_requests;
        if (new_val == 1) { // only take an action on first request
            ImGuiIO &io = ImGui::GetIO();
            // force draw the cursor, otherwise games like Truberbrook will not have an overlay cursor
            io.MouseDrawCursor = state;
            // not necessary, just to be sure
            io.WantCaptureMouse = state;
            // not necessary, just to be sure
            io.WantCaptureKeyboard = state;

            // clip the cursor
            _renderer->HideAppInputs(true);
            PRINT_DEBUG("obscured app input (count=%u)", new_val);
        }
    } else {
        if (obscure_cursor_requests > 0) {
            auto new_val = --obscure_cursor_requests;
            if (!new_val) { // only take an action when the requests reach 0
                ImGuiIO &io = ImGui::GetIO();
                // force draw the cursor, otherwise games like Truberbrook will not have an overlay cursor
                io.MouseDrawCursor = state;
                // not necessary, just to be sure
                io.WantCaptureMouse = state;
                // not necessary, just to be sure
                io.WantCaptureKeyboard = state;

                // restore the old cursor
                _renderer->HideAppInputs(false);
                PRINT_DEBUG("restored app input (count=%u)", new_val);
            }
        }
    }
}

// Play a notification sound for the given key.
// load_audio() pre-populates each slot via cascade inheritance so the key is always ready
// when any WAV files are present. Drop WAV files into the game's or global "sounds/" folder.
void Steam_Overlay::play_overlay_sound(const char* sound_key)
{
#ifdef __WINDOWS__
    if (!sound_key) return;
    auto it = wav_files.find(sound_key);
    if (it != wav_files.end() && !it->second.empty()) {
        PlaySoundA((LPCSTR)it->second.data(), nullptr, SND_ASYNC | SND_MEMORY);
    }
#endif
}

void Steam_Overlay::notify_sound_user_invite(friend_window_state& friend_state)
{
    if (settings->disable_overlay_friend_notification) return;

    if (!(friend_state.window_state & window_state_show)) {
        friend_state.window_state |= window_state_need_attention;
        play_overlay_sound("overlay_invite_notification.wav");
    }
}

void Steam_Overlay::notify_sound_chat_message(friend_window_state& friend_state)
{
    if (settings->disable_overlay_friend_notification) return;

    if (!(friend_state.window_state & window_state_show)) {
        play_overlay_sound("overlay_chat_notification.wav");
    }
}

void Steam_Overlay::notify_sound_user_achievement()
{
    if (settings->disable_overlay_achievement_notification) return;

    play_overlay_sound("overlay_achievement_notification.wav");
}

void Steam_Overlay::notify_sound_auto_accept_friend_invite()
{
    play_overlay_sound("overlay_auto_accept_notification.wav");
}

void Steam_Overlay::notify_sound_lobby_join()
{
    if (settings->disable_overlay_friend_notification) return;

    play_overlay_sound("overlay_lobby_join_request.wav");
}

void Steam_Overlay::notify_sound_friend_lobby()
{
    if (settings->disable_overlay_friend_notification) return;

    play_overlay_sound("overlay_friend_lobby.wav");
}

void Steam_Overlay::notify_sound_lobby_kicked()
{
    if (settings->disable_overlay_friend_notification) return;

    play_overlay_sound("overlay_lobby_kicked.wav");
}

void Steam_Overlay::notify_sound_lobby_join_response(bool accepted)
{
    if (settings->disable_overlay_friend_notification) return;

    // accepted/denied each have their own file; both fall back to the generic response sound
    if (accepted) {
        play_overlay_sound("overlay_lobby_join_accepted.wav");
    } else {
        play_overlay_sound("overlay_lobby_join_denied.wav");
    }
}

void Steam_Overlay::notify_sound_achievement_progress()
{
    if (settings->disable_overlay_achievement_notification) return;

    play_overlay_sound("overlay_achievement_progress.wav");
}

void Steam_Overlay::notify_sound_lobby_status()
{
    play_overlay_sound("overlay_lobby_status.wav");
}

int find_free_id(std::vector<int> &ids, int base)
{
    std::sort(ids.begin(), ids.end());

    int id = base;
    for (auto i : ids)
    {
        if (id < i)
            break;
        id = i + 1;
    }

    return id > (base+max_window_id) ? 0 : id;
}

int find_free_friend_id(const std::map<Friend, friend_window_state, Friend_Less> &friend_windows)
{
    std::vector<int> ids{};
    ids.reserve(friend_windows.size());

    std::for_each(friend_windows.begin(), friend_windows.end(), [&ids](std::pair<Friend const, friend_window_state> const& i)
    {
        ids.emplace_back(i.second.id);
    });

    return find_free_id(ids, base_friend_window_id);
}

int find_free_notification_id(std::vector<Notification> const& notifications)
{
    std::vector<int> ids{};
    ids.reserve(notifications.size());

    std::for_each(notifications.begin(), notifications.end(), [&ids](Notification const& i)
    {
        ids.emplace_back(i.id);
    });


    return find_free_id(ids, base_friend_window_id);
}

bool Steam_Overlay::submit_notification(
    notification_type type,
    const std::string &msg,
    std::pair<const Friend, friend_window_state> *frd,
    Overlay_Achievement *ach)
{
    PRINT_DEBUG("%i", (int)type);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return false;

    int id = find_free_notification_id(notifications);
    if (id == 0) {
        PRINT_DEBUG("error no free id to create a notification window");
        return false;
    }

    Notification notif{};
    notif.start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    notif.steady_start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
    notif.id = id;
    notif.type = (uint8)type;
    notif.message = msg;
    notif.frd = frd;
    if (frd) notif.source_friend_id = frd->first.id();
    if (ach) notif.ach = *ach;

    notifications.emplace_back(notif);
    allow_renderer_frame_processing(true);
    // uncomment this block to obscure cursor input and steal focus for these specific notifications
    switch (type) {
        // we want to steal focus for these ones
        case notification_type::invite:
            obscure_game_input(true);
        break;

        // not effective
        case notification_type::achievement_progress:
        case notification_type::achievement:
        case notification_type::auto_accept_invite:
        case notification_type::message:
        case notification_type::screenshot:
            // nothing
        break;

        case notification_type::lobby_join_request:
            obscure_game_input(true);
        break;

        case notification_type::lobby_join_request_response:
            // non-interactive, no input stealing
        break;

        case notification_type::friend_lobby_available:
            obscure_game_input(true);
        break;

        default:
            PRINT_DEBUG("error unhandled type %i", (int)type);
        break;
    }

    return true;
}

void Steam_Overlay::add_chat_message_notification(std::string const &message, std::pair<const Friend, friend_window_state> *frd)
{
    if (settings->disable_overlay_friend_notification) return;

    PRINT_DEBUG("'%s'", message.c_str());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    submit_notification(notification_type::message, message, frd);
}

void Steam_Overlay::poll_lobby_join_requests()
{
    Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
    Steam_Friends *steamFriends = get_steam_client()->steam_friends;
    const auto &pending = mm->GetPendingLobbyJoinRequests();

    for (const auto &req : pending) {
        auto key = std::make_pair(req.lobby_id.ConvertToUint64(), req.requester_id.ConvertToUint64());
        if (notified_lobby_join_requests.count(key)) continue;

        notified_lobby_join_requests.insert(key);

        const char *name = steamFriends->GetFriendPersonaName(req.requester_id);
        std::string msg = std::string("Auto join request from ") + (name ? name : "Unknown") + "\nLobby: " + std::to_string(req.lobby_id.ConvertToUint64());

        int id = find_free_notification_id(notifications);
        if (id == 0) continue;

        Notification notif{};
        notif.start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
        notif.steady_start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
        notif.id = id;
        notif.type = (uint8)notification_type::lobby_join_request;
        notif.message = msg;
        notif.join_request_lobby_id = req.lobby_id.ConvertToUint64();
        notif.join_request_requester_id = req.requester_id.ConvertToUint64();
        notif.source_friend_id = req.requester_id.ConvertToUint64();

        notifications.emplace_back(notif);
        allow_renderer_frame_processing(true);
        obscure_game_input(true);
        notify_sound_lobby_join();
    }
}

void Steam_Overlay::add_lobby_join_request_response_notification(uint64 lobby_id, const std::string &owner_name, bool accepted, uint64 source_id)
{
    PRINT_DEBUG("lobby=%llu owner=%s accepted=%d", lobby_id, owner_name.c_str(), (int)accepted);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    std::string msg = std::string("Your join request was ") + (accepted ? "accepted" : "denied") + " by " + owner_name + "\nLobby: " + std::to_string(lobby_id);

    int id = find_free_notification_id(notifications);
    if (id == 0) return;

    Notification notif{};
    notif.start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    notif.steady_start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
    notif.id = id;
    notif.type = (uint8)notification_type::lobby_join_request_response;
    notif.message = msg;
    notif.source_friend_id = source_id;

    notifications.emplace_back(notif);
    allow_renderer_frame_processing(true);
    notify_sound_lobby_join_response(accepted);
}

void Steam_Overlay::add_lobby_kicked_notification(uint64 lobby_id, const std::string &kicker_name, uint64 source_id)
{
    PRINT_DEBUG("lobby=%llu kicker=%s", lobby_id, kicker_name.c_str());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    std::string msg = std::string("You were removed from lobby by ") + kicker_name + "\nLobby: " + std::to_string(lobby_id);

    int id = find_free_notification_id(notifications);
    if (id == 0) return;

    Notification notif{};
    notif.start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    notif.steady_start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
    notif.id = id;
    notif.type = (uint8)notification_type::lobby_kicked;
    notif.message = msg;
    notif.source_friend_id = source_id;

    notifications.emplace_back(notif);
    allow_renderer_frame_processing(true);
    notify_sound_lobby_kicked();
}

void Steam_Overlay::show_test_achievement()
{
    PRINT_DEBUG_ENTRY();
    Overlay_Achievement ach{};
    ach.title = translationTestAchievement[current_language];
    ach.description = "~~~ " + ach.title + " ~~~";
    ach.achieved = true;

    // random add icon
    if (achievements.size()) {
        size_t rand_idx = common_helpers::rand_number(achievements.size() - 1);
        auto &rand_ach = achievements[rand_idx];
        bool achieved = rand_idx < (achievements.size() / 2);
        // force upload to GPU if the pagination is request-based
        try_load_ach_icon(rand_ach, achieved, settings->paginated_achievements_icons == 0);
        ach.icon = rand_ach.icon;
        ach.icon_gray = rand_ach.icon_gray;
        // Copy decoded pixel data so the bridge addon can access the icon
        ach.icon_handle = rand_ach.icon_handle;
        ach.icon_gray_handle = rand_ach.icon_gray_handle;
        ach.icon_decoded_data = rand_ach.icon_decoded_data;
        ach.icon_gray_decoded_data = rand_ach.icon_gray_decoded_data;
        ach.name = rand_ach.name;
    }

    // randomly add progress
    bool for_progress = false;
    if (common_helpers::rand_number(1000) % 4 == 0) {
        for_progress = true;
        uint32 progress = (uint32)(common_helpers::rand_number(500) / 10 + 50); // [50, 100]
        ach.max_progress = 100;
        ach.progress = progress;
        ach.achieved = false;
    } else if (common_helpers::rand_number(1000) % 2) {
        ach.unlock_percentage = 1.0f;
    }

    post_achievement_notification(ach, for_progress);
    // sound is now played when notification is actually shown (delayed with queue)
}

void Steam_Overlay::build_friend_context_menu(Friend const& frd, friend_window_state& state)
{
    if (ImGui::BeginPopupContextItem("Friends_ContextMenu", 1)) {
        // this is set to true if any button was clicked
        // otherwise, after clicking any button, the menu will be persistent
        bool close_popup = false;

        // user clicked on "chat"
        if (ImGui::Button(translationChat[current_language])) {
            close_popup = true;
            state.window_state |= window_state_show;
        }
        // user clicked on "copy id" on a friend
        if (ImGui::Button(translationCopyId[current_language])) {
            close_popup = true;
            auto friend_id_str = std::to_string(frd.id());
            ImGui::SetClipboardText(friend_id_str.c_str());
        }
        // user clicked on "copy lobby id" on a friend (if they're in a lobby)
        if (frd.lobby_id() != 0 && ImGui::Button("Copy Lobby ID##PopupCopyLobbyId")) {
            close_popup = true;
            auto lobby_id_str = std::to_string(frd.lobby_id());
            ImGui::SetClipboardText(lobby_id_str.c_str());
        }
        // Check if friend is already in our lobby
        bool friend_in_my_lobby = false;
        {
            CSteamID my_lob = settings->get_lobby();
            if (my_lob.IsValid()) {
                Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
                if (mm) {
                    int mc = mm->GetNumLobbyMembers(my_lob);
                    for (int mi = 0; mi < mc; ++mi) {
                        if (mm->GetLobbyMemberByIndex(my_lob, mi).ConvertToUint64() == frd.id()) {
                            friend_in_my_lobby = true;
                            break;
                        }
                    }
                }
            }
        }
        // If we have the same appid, activate the invite button
        if (settings->get_local_game_id().AppID() == frd.appid()) {
            // user clicked on "invite to game" (hide if friend already in our lobby)
            std::string translationInvite_tmp(translationInvite[current_language]);
            translationInvite_tmp.append("##PopupInviteToGame");
            if (i_have_lobby && !friend_in_my_lobby && ImGui::Button(translationInvite_tmp.c_str())) {
                close_popup = true;
                state.window_state |= window_state_invite;
                has_friend_action.push(frd);
            }
        }
        // user clicked on "join lobby" (same app only, hide if already in same lobby)
        if (settings->get_local_game_id().AppID() == frd.appid() && state.joinable && !friend_in_my_lobby) {
            std::string translationJoin_tmp(translationJoin[current_language]);
            translationJoin_tmp.append("##PopupJoinLobby");
            if (ImGui::Button(translationJoin_tmp.c_str())) {
                close_popup = true;
                if (!invite_all_friends_clicked) {
                    state.window_state |= window_state_join;
                    has_friend_action.push(frd);
                }
            }
        }
        // Show Accept Invite if this friend sent us a pending invite we haven't accepted yet
        if (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
            if (ImGui::Button("Accept Invite##PopupAcceptInvite")) {
                close_popup = true;
                state.window_state |= window_state_join;
                has_friend_action.push(frd);
            }
        }

        if (close_popup || invite_all_friends_clicked) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void Steam_Overlay::build_chat_window()
{
    if (!show_chat) return;

    // Collect friends with open chat tabs
    struct ChatTab {
        const Friend *frd;
        friend_window_state *state;
    };
    std::vector<ChatTab> open_tabs;
    for (auto &[frd, state] : friends) {
        if (state.window_state & window_state_show)
            open_tabs.push_back({&frd, &state});
    }

    if (open_tabs.empty()) {
        show_chat = false;
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    float min_w = io.DisplaySize.x * 0.25f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_w, ImGui::GetFontSize() * 16), ImVec2(8192, 8192));
    ImGui::SetNextWindowSize(ImVec2(450, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(1.0f);

    if (ImGui::Begin("Chat##gse_chat_tabbed", &show_chat, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("##chat_tabs")) {
            for (auto &tab : open_tabs) {
                auto &frd = *tab.frd;
                auto &state = *tab.state;
                bool needs_attn = (state.window_state & window_state_need_attention) != 0;

                if (needs_attn)
                    ImGui::PushStyleColor(ImGuiCol_Tab, TC(ImVec4(0.6f, 0.4f, 0.1f, 1.0f)));

                bool tab_open = true;
                std::string tab_label = frd.name() + "###chat_tab_" + std::to_string(frd.id());

                if (ImGui::BeginTabItem(tab_label.c_str(), &tab_open)) {
                    // Clear attention when tab is active
                    if (needs_attn) state.window_state &= ~window_state_need_attention;

                    // ---- 64px friend avatar + 3 info lines ----
                    {
                        const float avatar_size = 64.0f;
                        bool has_avatar = try_load_avatar(state, frd.id());
                        if (has_avatar && state.avatar_resource && state.avatar_resource->GetResourceId() != 0) {
                            ImGui::Image(state.avatar_resource->GetResourceId(), ImVec2(avatar_size, avatar_size));
                        } else {
                            ImVec2 p = ImGui::GetCursorScreenPos();
                            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + avatar_size, p.y + avatar_size), TC32(IM_COL32(60, 60, 80, 255)));
                            ImGui::Dummy(ImVec2(avatar_size, avatar_size));
                        }
                        ImGui::SameLine();

                        ImVec2 text_start = ImGui::GetCursorPos();
                        uint32 local_appid = settings->get_local_game_id().AppID();

                        // Line 1: Friend name (ID: steamid)
                        ImGui::SetCursorPos(text_start);
                        ImGui::TextColored(TC(ImVec4(0.9f, 0.9f, 0.2f, 1.0f)), "%s", frd.name().c_str());
                        ImGui::SameLine();
                        ImGui::TextColored(TC(ImVec4(0.6f, 0.6f, 0.6f, 1.0f)), "(ID: %llu)", (unsigned long long)frd.id());

                        // Line 2: Playing AppID
                        ImGui::SetCursorPosX(text_start.x);
                        if (frd.appid() != 0) {
                            auto it = steam_preowned_app_ids.find(frd.appid());
                            std::string app_name = (it != steam_preowned_app_ids.end()) ? it->second : std::to_string(frd.appid());
                            ImGui::TextColored(TC(ImVec4(0.5f, 0.8f, 0.5f, 1.0f)), "Playing %s (AppID %u)", app_name.c_str(), frd.appid());
                        } else {
                            ImGui::TextColored(TC(ImVec4(0.5f, 0.7f, 0.5f, 1.0f)), "Online");
                        }

                        // Line 3: Status
                        ImGui::SetCursorPosX(text_start.x);
                        bool same_app = (local_appid == frd.appid());
                        if (frd.lobby_id() != 0) {
                            bool frd_is_owner = (frd.lobby_owner_name().size() > 0 && frd.lobby_owner_name() == frd.name());
                            ImGui::TextColored(TC(ImVec4(0.4f, 0.8f, 0.4f, 1.0f)), "%s - %llu",
                                frd_is_owner ? "Has Lobby" : "In Lobby", (unsigned long long)frd.lobby_id());
                        } else if (same_app) {
                            ImGui::TextColored(TC(ImVec4(0.5f, 0.7f, 0.5f, 1.0f)), "In Game");
                        } else if (frd.appid() != 0) {
                            ImGui::TextColored(TC(ImVec4(0.6f, 0.6f, 0.6f, 1.0f)), "In another game");
                        } else {
                            ImGui::TextColored(TC(ImVec4(0.5f, 0.7f, 0.5f, 1.0f)), "Online");
                        }
                    }
                    ImGui::Separator();

                    // Invite accept/refuse
                    if (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
                        bool same_app = (settings->get_local_game_id().AppID() == frd.appid());
                        ImGui::PushStyleColor(ImGuiCol_Text, TC(ImVec4(1.0f, 0.8f, 0.2f, 1.0f)));
                        ImGui::LabelText("##label", translationInvitedYouToJoinTheGame[current_language], frd.name().c_str(), frd.appid());
                        ImGui::PopStyleColor();
                        if (same_app) {
                            ImGui::SameLine();
                            if (ImGui::Button(translationAccept[current_language])) {
                                state.window_state |= window_state_join;
                                this->has_friend_action.push(frd);
                                state.chat_history.append("[INVITE ACCEPTED] You accepted the invite\n");
                            }
                            ImGui::SameLine();
                            if (ImGui::Button(translationRefuse[current_language])) {
                                state.window_state &= ~(window_state_lobby_invite | window_state_rich_invite);
                                state.chat_history.append("[INVITE REFUSED] You declined the invite\n");
                            }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextColored(TC(ImVec4(0.6f, 0.6f, 0.6f, 1.0f)), "(different game)");
                            ImGui::SameLine();
                            if (ImGui::Button(translationRefuse[current_language])) {
                                state.window_state &= ~(window_state_lobby_invite | window_state_rich_invite);
                                state.chat_history.append("[INVITE REFUSED] You declined the invite\n");
                            }
                        }
                    }

                    // Chat history
                    float footer_height = ImGui::GetFrameHeightWithSpacing() + 4;
                    ImGui::InputTextMultiline("##chat_history", &state.chat_history[0], state.chat_history.length(),
                        { -1.0f, -footer_height }, ImGuiInputTextFlags_ReadOnly);

                    // Input bar: [____chat line______] [send]
                    float wnd_width = ImGui::GetContentRegionAvail().x;
                    ImGuiStyle &style = ImGui::GetStyle();
                    wnd_width -= ImGui::CalcTextSize(translationSend[current_language]).x + style.FramePadding.x * 2 + style.ItemSpacing.x + 1;

                    uint64_t frd_id = frd.id();
                    ImGui::PushID((const char *)&frd_id, (const char *)&frd_id + sizeof(frd_id));
                    ImGui::PushItemWidth(wnd_width);

                    bool send_chat_msg = false;
                    if (ImGui::InputText("##chat_line", state.chat_input, max_chat_len, ImGuiInputTextFlags_EnterReturnsTrue)) {
                        send_chat_msg = true;
                        ImGui::SetKeyboardFocusHere(-1);
                    }

                    ImGui::PopItemWidth();
                    ImGui::PopID();

                    ImGui::SameLine();
                    if (ImGui::Button(translationSend[current_language])) {
                        send_chat_msg = true;
                    }

                    if (send_chat_msg) {
                        if (!(state.window_state & window_state_send_message)) {
                            has_friend_action.push(frd);
                            state.window_state |= window_state_send_message;
                        }
                    }

                    ImGui::EndTabItem();
                }

                if (needs_attn)
                    ImGui::PopStyleColor();

                // Close tab
                if (!tab_open) {
                    state.window_state &= ~window_state_show;
                }
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}


std::chrono::milliseconds Steam_Overlay::get_notification_duration(notification_type type)
{
    switch (type)
    {
    case notification_type::message:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_chat);

    case notification_type::invite:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_invitation);

    case notification_type::achievement:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_achievement);

    case notification_type::achievement_progress:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_progress);

    case notification_type::auto_accept_invite:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_invitation);
    
    case notification_type::lobby_join_request:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_invitation);
    
    case notification_type::lobby_join_request_response:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_invitation);
    
    case notification_type::lobby_kicked:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_invitation);

    case notification_type::lobby_status:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_invitation);

    case notification_type::screenshot:
        return std::chrono::milliseconds(settings->overlay_appearance.notification_duration_screenshot);
    }

    PRINT_DEBUG("ERROR unhandled type %i", (int)type);
    return Notification::default_show_time;
}

// set the position of the next notification
void Steam_Overlay::set_next_notification_pos(std::pair<float, float> scrn_size, std::chrono::milliseconds elapsed, std::chrono::milliseconds duration, const Notification &noti, struct NotificationsCoords &coords)
{
    const float scrn_width = scrn_size.first;
    const float scrn_height = scrn_size.second;

    auto &global_style = ImGui::GetStyle();
    const float padding_all_sides = 2 * (global_style.WindowPadding.y + global_style.WindowPadding.x);

    const float min_noti_width = scrn_width * Notification::width_percent;
    const bool is_achievement = ((notification_type)noti.type == notification_type::achievement ||
                                 (notification_type)noti.type == notification_type::achievement_progress);
    const float noti_width = is_achievement ? min_noti_width :
        (noti.last_width > min_noti_width ? noti.last_width : min_noti_width);
    const float msg_height = ImGui::CalcTextSize(
        noti.message.c_str(),
        noti.message.c_str() + noti.message.size(),
        false,
        noti_width - padding_all_sides - global_style.ItemSpacing.x
    ).y;
    float noti_height = msg_height;

    // Extra height for friend avatar + 3-line header (invite, message, join request, response, kicked)
    const float friend_header_height = (noti.source_friend_id != 0)
        ? std::max(48.0f, ImGui::GetTextLineHeight() * 3.0f) + global_style.ItemSpacing.y + 1.0f /* separator */
        : 0.0f;
    
    // get the required position
    Overlay_Appearance::NotificationPosition pos = Overlay_Appearance::default_pos;
    switch ((notification_type)noti.type) {
    case notification_type::achievement_progress:
    case notification_type::achievement: {
        pos = settings->overlay_appearance.ach_earned_pos;

        const auto &ach_h = noti.ach.value();
        const float title_height = ImGui::CalcTextSize(
            ach_h.title.c_str(), nullptr, false,
            noti_width - padding_all_sides
        ).y;
        const float desc_height = ImGui::CalcTextSize(
            ach_h.description.c_str(), nullptr, false,
            noti_width - padding_all_sides - global_style.ItemSpacing.x - settings->overlay_appearance.icon_size
        ).y;
        const float row_height = std::max(desc_height, (float)settings->overlay_appearance.icon_size);
        noti_height = title_height + global_style.ItemSpacing.y + row_height;

        if ((notification_type)noti.type == notification_type::achievement_progress) {
            if (!noti.ach.value().achieved && noti.ach.value().max_progress > 0) {
                noti_height += settings->overlay_appearance.font_size + global_style.WindowPadding.y;
            }
        }
    }
    break;

    // case notification_type::invite: pos = settings->overlay_appearance.invite_pos; break;
    case notification_type::invite: {
        pos = settings->overlay_appearance.invite_pos;
        const float msg_height = ImGui::CalcTextSize(
            noti.message.c_str(),
            noti.message.c_str() + noti.message.size(),
            false,
            noti_width - padding_all_sides - global_style.ItemSpacing.x
        ).y;
        noti_height = friend_header_height + msg_height + settings->overlay_appearance.font_size + global_style.WindowPadding.y;
    }
    break;
    case notification_type::message:
        pos = settings->overlay_appearance.chat_msg_pos;
        noti_height += friend_header_height;
    break;
    case notification_type::lobby_join_request: {
        pos = settings->overlay_appearance.invite_pos;
        const float ljr_msg_height = ImGui::CalcTextSize(
            noti.message.c_str(),
            noti.message.c_str() + noti.message.size(),
            false,
            noti_width - padding_all_sides - global_style.ItemSpacing.x
        ).y;
        noti_height = friend_header_height + ljr_msg_height + settings->overlay_appearance.font_size + global_style.WindowPadding.y;
    }
    break;
    case notification_type::lobby_join_request_response: {
        pos = settings->overlay_appearance.invite_pos;
        const float ljrr_msg_height = ImGui::CalcTextSize(
            noti.message.c_str(),
            noti.message.c_str() + noti.message.size(),
            false,
            noti_width - padding_all_sides - global_style.ItemSpacing.x
        ).y;
        noti_height = friend_header_height + ljrr_msg_height + settings->overlay_appearance.font_size + global_style.WindowPadding.y;
    }
    break;
    case notification_type::lobby_kicked: {
        pos = settings->overlay_appearance.invite_pos;
        const float lk_msg_height = ImGui::CalcTextSize(
            noti.message.c_str(),
            noti.message.c_str() + noti.message.size(),
            false,
            noti_width - padding_all_sides - global_style.ItemSpacing.x
        ).y;
        noti_height = friend_header_height + lk_msg_height + settings->overlay_appearance.font_size + global_style.WindowPadding.y;
    }
    break;
    default: PRINT_DEBUG("ERROR: unhandled notification type %i", (int)noti.type); break;
    }
    // add some y padding for niceness
    noti_height += 2 * global_style.WindowPadding.y;

    // For non-achievement types, prefer the cached rendered size from the previous frame
    // This ensures stacking is accurate even if the estimate was slightly off
    if (!is_achievement && noti.last_height > 0)
        noti_height = noti.last_height;

    // 0 on the y-axis is top, 0 on the x-axis is left
    float x = 0.0f;
    float y = 0.0f;
    float animate_size = 0.0f;
    const float margin_y = settings->overlay_appearance.notification_margin_y;
    const float margin_x = settings->overlay_appearance.notification_margin_x;

    switch (pos) {
    // top
    case Overlay_Appearance::NotificationPosition::top_left:
        animate_size = animate_factor(elapsed, duration) * noti_width;
        x = margin_x - animate_size;
        y = coords.top_left.second + margin_y;
        coords.top_left.second = y + noti_height;
    break;
    case Overlay_Appearance::NotificationPosition::top_center:
        animate_size = animate_factor(elapsed, duration) * noti_height;
        x = (scrn_width / 2) - (noti_width / 2);
        y = coords.top_center.second + margin_y - animate_size;
        coords.top_center.second = y + noti_height;
    break;
    case Overlay_Appearance::NotificationPosition::top_right:
        animate_size = animate_factor(elapsed, duration) * noti_width;
        x = (scrn_width - noti_width - margin_x) + animate_size;
        y = coords.top_right.second + margin_y;
        coords.top_right.second = y + noti_height;
    break;

    // bot
    case Overlay_Appearance::NotificationPosition::bot_left:
        animate_size = animate_factor(elapsed, duration) * noti_width;
        x = margin_x - animate_size;
        y = scrn_height - coords.bot_left.second - margin_y - noti_height;
        coords.bot_left.second = scrn_height - y;
    break;
    case Overlay_Appearance::NotificationPosition::bot_center:
        animate_size = animate_factor(elapsed, duration) * noti_height;
        x = (scrn_width / 2) - (noti_width / 2);
        y = scrn_height - coords.bot_center.second - margin_y - noti_height + animate_size;
        coords.bot_center.second = scrn_height - y;
    break;
    case Overlay_Appearance::NotificationPosition::bot_right:
        animate_size = animate_factor(elapsed, duration) * noti_width;
        x = (scrn_width - noti_width - margin_x) + animate_size;
        y = scrn_height - coords.bot_right.second - margin_y - noti_height;
        coords.bot_right.second = scrn_height - y;
    break;

    default: /* satisfy compiler warning */ break;
    }

    ImGui::SetNextWindowPos(ImVec2( x, y ));
    // Achievement notifications use fixed size (already sized correctly).
    // All other types auto-resize with 25% minimum width and 50% maximum width.
    if (is_achievement) {
        ImGui::SetNextWindowSize(ImVec2(min_noti_width, noti_height));
    } else {
        ImGui::SetNextWindowSizeConstraints(ImVec2(min_noti_width, 0), ImVec2(scrn_width * 0.5f, FLT_MAX));
    }
}

float Steam_Overlay::animate_factor(std::chrono::milliseconds elapsed, std::chrono::milliseconds duration)
{
    if (settings->overlay_appearance.notification_animation <= 0) return 0.0f; // no animation

    std::chrono::milliseconds animation_duration(settings->overlay_appearance.notification_animation);
    // PRINT_DEBUG("ELAPSED %u/%u/%u", (uint32)elapsed.count(), (uint32)duration.count(), (uint32)animation_duration.count());

    float factor = 0.0f;
    if (elapsed < animation_duration) { // sliding in
        factor = 1.0f - (static_cast<float>(elapsed.count()) / animation_duration.count());
        // PRINT_DEBUG("SHOW FACTOR %f", factor);
    } else {
        // time between sliding in/out animation
        // here we add the animation duration because we want to count after the animation
        // if we have 1 sec animation & 2 sec show time:
        //   the duration will start at < 1 sec during the initial animation
        //   after the animation (1 sec), the duration will be >= 1 sec
        //   but since we want 2 sec show time, the duration must last 3 sec
        auto steady_time = animation_duration + duration;
        if (elapsed > steady_time) {
            factor = static_cast<float>((elapsed - steady_time).count()) / animation_duration.count();
            // PRINT_DEBUG("HIDE FACTOR %f", factor);
        }
    }

    return factor;
}

void Steam_Overlay::add_ach_progressbar(const Overlay_Achievement &ach)
{
    if (!ach.achieved && ach.progress > 0 && ach.max_progress > 0) {
        char buf[32]{};
        sprintf(buf, "%u/%u", ach.progress, ach.max_progress);
        ImGui::ProgressBar((float)ach.progress / ach.max_progress, { -1 , settings->overlay_appearance.font_size }, buf);
    }
}

ImVec4 Steam_Overlay::get_notification_bg_rgba_safe()
{
    if (settings->overlay_appearance.notification_r >= 0 &&
        settings->overlay_appearance.notification_g >= 0 &&
        settings->overlay_appearance.notification_b >= 0 &&
        settings->overlay_appearance.notification_a >= 0)
    {
        return ImVec4(
            settings->overlay_appearance.notification_r,
            settings->overlay_appearance.notification_g,
            settings->overlay_appearance.notification_b,
            settings->overlay_appearance.notification_a
        );
    }

    // fallback to dark-gray background
    return ImVec4(
        0.12f,
        0.14f,
        0.21f,
        1.0f
    );
}

void Steam_Overlay::build_notifications(float width, float height)
{
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    std::queue<Friend> friend_actions_temp{};

    ImGui::PushFont(font_notif, 0.0f);
    // Add window rounding
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, settings->overlay_appearance.notification_rounding);

    // Helper: render 48px avatar + 3-line friend info for a notification
    auto render_notif_friend_header = [&](uint64 friend_id) {
        if (!friend_id) return;

        // Look up friend in the friends map
        const Friend *frd_ptr = nullptr;
        friend_window_state *frd_state = nullptr;
        for (auto &[f, s] : friends) {
            if (f.id() == friend_id) {
                frd_ptr = &f;
                frd_state = &s;
                break;
            }
        }
        if (!frd_ptr) return;

        const float avatar_size = 48.0f;

        // Avatar
        bool has_avatar = try_load_avatar(*frd_state, frd_ptr->id());
        if (has_avatar && frd_state->avatar_resource && frd_state->avatar_resource->GetResourceId() != 0) {
            ImGui::Image(frd_state->avatar_resource->GetResourceId(), ImVec2(avatar_size, avatar_size));
        } else {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + avatar_size, p.y + avatar_size), TC32(IM_COL32(60, 60, 80, 255)));
            ImGui::Dummy(ImVec2(avatar_size, avatar_size));
        }
        ImGui::SameLine();

        ImVec2 text_start = ImGui::GetCursorPos();
        uint32 local_appid = settings->get_local_game_id().AppID();

        // Line 1: Name (ID: steamid)
        ImGui::SetCursorPos(text_start);
        ImGui::TextColored(TC(ImVec4(0.9f, 0.9f, 0.2f, 1.0f)), "%s", frd_ptr->name().c_str());
        ImGui::SameLine();
        ImGui::TextColored(TC(ImVec4(0.6f, 0.6f, 0.6f, 1.0f)), "(ID: %llu)", (unsigned long long)frd_ptr->id());

        // Line 2: Playing AppName (AppID XXXX)
        ImGui::SetCursorPosX(text_start.x);
        if (frd_ptr->appid() != 0) {
            auto it2 = steam_preowned_app_ids.find(frd_ptr->appid());
            std::string game = (it2 != steam_preowned_app_ids.end()) ? it2->second : std::to_string(frd_ptr->appid());
            ImGui::TextColored(TC(ImVec4(0.5f, 0.8f, 0.5f, 1.0f)), "Playing %s (AppID %u)", game.c_str(), frd_ptr->appid());
        } else {
            ImGui::TextColored(TC(ImVec4(0.5f, 0.5f, 0.5f, 1.0f)), "Online");
        }

        // Line 3: Lobby info (only if same app)
        ImGui::SetCursorPosX(text_start.x);
        if ((local_appid == frd_ptr->appid()) && frd_ptr->lobby_id() != 0) {
            Steam_Matchmaking *mm_n = get_steam_client()->steam_matchmaking;
            if (mm_n) {
                CSteamID frd_lobby((uint64)frd_ptr->lobby_id());
                int mc = mm_n->GetNumLobbyMembers(frd_lobby);
                int ml = mm_n->GetLobbyMemberLimit(frd_lobby);
                CSteamID owner = mm_n->GetLobbyOwner(frd_lobby);
                std::string owner_name;
                for (auto &[f2, s2] : friends) {
                    if (f2.id() == owner.ConvertToUint64()) { owner_name = f2.name(); break; }
                }
                if (owner_name.empty()) {
                    if (owner == settings->get_local_steam_id())
                        owner_name = settings->get_local_name();
                    else if (owner.ConvertToUint64() == frd_ptr->id())
                        owner_name = frd_ptr->name();
                    else
                        owner_name = std::to_string(owner.ConvertToUint64());
                }
                ImGui::TextColored(TC(ImVec4(0.4f, 0.8f, 0.4f, 1.0f)), "%s - %llu (%d/%d - %s)",
                    (owner_name == frd_ptr->name()) ? "Has Lobby" : "In Lobby",
                    (unsigned long long)frd_ptr->lobby_id(), mc, ml, owner_name.c_str());
            }
        }

        ImGui::Separator();
    };
   
    NotificationsCoords coords{};
    for (auto it = notifications.begin(); it != notifications.end(); ++it) {
        auto noti_duration = get_notification_duration((notification_type)it->type);
        if (noti_duration.count() <= 0) {
            it->expired = true;
            continue;
        }

        // *2 for sliding in & out animation
        auto total_allowed_duration = noti_duration + std::chrono::milliseconds(settings->overlay_appearance.notification_animation * 2);
        auto elapsed_notif = now - it->start_time;
        if (elapsed_notif > total_allowed_duration) {
            it->expired = true;
            continue;
        }

        float settings_noti_alpha = settings->overlay_appearance.notification_a >= 0.0f && settings->overlay_appearance.notification_a <= 1.0f
            ? settings->overlay_appearance.notification_a
            : 1.0f;

        const bool is_rare_achievement =
            (notification_type)it->type == notification_type::achievement &&
            it->ach.has_value() &&
            it->ach->unlock_percentage >= 0.0f &&
            it->ach->unlock_percentage <= 10.0f;
        const bool is_achievement =
            (notification_type)it->type == notification_type::achievement;

        ImGui::PushStyleColor(ImGuiCol_Border, is_rare_achievement
            ? ImVec4(32.0f / 255.0f, 24.0f / 255.0f, 8.0f / 255.0f, settings_noti_alpha)
            : ImVec4(0, 0, 0, settings_noti_alpha));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, get_notification_bg_rgba_safe());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, settings_noti_alpha));
       
        // some extra window flags for each notification type
        ImGuiWindowFlags extra_flags = ImGuiWindowFlags_NoFocusOnAppearing;
        switch ((notification_type)it->type) {
            // games like "Mafia Definitive Edition" will pause the entire game/scene if focus was stolen
            // be less intrusive for notifications that do not require interaction
            case notification_type::achievement_progress:
            case notification_type::achievement:
            case notification_type::auto_accept_invite:
                extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs;
            break;

            case notification_type::message:
                // If we have a friend pointer, make the notification interactive for "Open Chat" button
                if (it->frd)
                    extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
                else
                    extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs;
            break;

            case notification_type::invite:
                // interactive: buttons remain clickable
                if (show_overlay) extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
            break;

            case notification_type::lobby_join_request:
                // interactive: Accept/Decline buttons
                if (show_overlay) extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
            break;

            case notification_type::lobby_join_request_response:
                extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
            break;

            case notification_type::lobby_kicked:
                extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
            break;

            case notification_type::friend_lobby_available:
                // interactive: Request to Join button
                if (show_overlay) extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
            break;

            case notification_type::lobby_status:
                // non-interactive but always visible on top when overlay is open
                extra_flags |= ImGuiWindowFlags_NoInputs;
            case notification_type::screenshot:
                // nothing (needs input for buttons)
            break;

            default:
                PRINT_DEBUG("error unhandled flags for type %i", (int)it->type);
            break;
        }

        std::string wnd_name = "NotiPopupShow" + std::to_string(it->id);

        set_next_notification_pos({width, height}, elapsed_notif, noti_duration, *it, coords);
        ImGuiWindowFlags noti_flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | extra_flags;
        {
            bool is_ach = ((notification_type)it->type == notification_type::achievement ||
                           (notification_type)it->type == notification_type::achievement_progress);
            if (!is_ach) noti_flags |= ImGuiWindowFlags_AlwaysAutoResize;
        }
        if (ImGui::Begin(wnd_name.c_str(), nullptr, noti_flags)) {
            // Cache actual rendered size for accurate stacking next frame
            ImVec2 win_sz = ImGui::GetWindowSize();
            it->last_width = win_sz.x;
            it->last_height = win_sz.y;
            switch ((notification_type)it->type) {
                case notification_type::achievement_progress:
                case notification_type::achievement: {
                    const auto &ach = it->ach.value();
                    auto &icon_rsrc = (notification_type)it->type == notification_type::achievement
                        ? ach.icon
                        : ach.icon_gray;
                    ImGui::Text("%s", ach.title.c_str());
                    if (icon_rsrc->GetResourceId() != 0 && ImGui::BeginTable("imgui_table", 2)) {
                        ImGui::TableSetupColumn("imgui_table_image", ImGuiTableColumnFlags_WidthFixed, settings->overlay_appearance.icon_size);
                        ImGui::TableSetupColumn("imgui_table_text");
                        ImGui::TableNextRow(ImGuiTableRowFlags_None, settings->overlay_appearance.icon_size);

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Image(icon_rsrc->GetResourceId(), ImVec2(settings->overlay_appearance.icon_size, settings->overlay_appearance.icon_size));
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextWrapped("%s", ach.description.c_str());

                        ImGui::EndTable();
                    } else {
                        ImGui::TextWrapped("%s", ach.description.c_str());
                    }

                    if ((notification_type)it->type == notification_type::achievement_progress) {
                        add_ach_progressbar(ach);
                    }
                }
                break;

                case notification_type::invite: {
                    render_notif_friend_header(it->source_friend_id);
                    ImGui::TextWrapped("%s", it->message.c_str());
                    if (it->frd) {
                        if (ImGui::Button(translationJoin[current_language])) {
                            it->frd->second.window_state |= window_state_join;
                            friend_actions_temp.push(it->frd->first);
                            // when we click "accept game invite" from someone else, we want to remove this notification immediately since it's no longer relevant
                            // this assignment will make the notification elapsed time insanely large
                            it->start_time = {};
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(translationRefuse[current_language])) {
                            it->frd->second.window_state &= ~(window_state_lobby_invite | window_state_rich_invite);
                            it->frd->second.chat_history.append("[INVITE REFUSED] You declined the invite\n");
                            it->start_time = {};
                        }
                    }
                }
                break;

                case notification_type::message:
                    render_notif_friend_header(it->source_friend_id);
                    ImGui::TextWrapped("%s", it->message.c_str());
                    if (it->frd) {
                        if (ImGui::Button("Open Chat")) {
                            it->frd->second.window_state |= window_state_show;
                            show_chat = true;
                            it->start_time = {}; // dismiss notification
                        }
                    }
                break;

                case notification_type::auto_accept_invite:
                case notification_type::screenshot:
                    ImGui::TextWrapped("%s", it->message.c_str());
                break;

                case notification_type::lobby_join_request: {
                    render_notif_friend_header(it->source_friend_id);
                    ImGui::TextWrapped("%s", it->message.c_str());
                    if (ImGui::Button(translationJoin[current_language])) {
                        Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
                        mm->AcceptLobbyJoinRequest(it->join_request_lobby_id, it->join_request_requester_id);
                        notified_lobby_join_requests.erase({it->join_request_lobby_id, it->join_request_requester_id});
                        it->start_time = {};
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(translationRefuse[current_language])) {
                        Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
                        mm->DeclineLobbyJoinRequest(it->join_request_lobby_id, it->join_request_requester_id);
                        notified_lobby_join_requests.erase({it->join_request_lobby_id, it->join_request_requester_id});
                        it->start_time = {};
                    }
                }
                break;

                case notification_type::lobby_join_request_response:
                    render_notif_friend_header(it->source_friend_id);
                    ImGui::TextWrapped("%s", it->message.c_str());
                    if (ImGui::Button(translationClose[current_language])) {
                        it->start_time = {};
                    }
                break;

                case notification_type::lobby_kicked:
                    render_notif_friend_header(it->source_friend_id);
                    ImGui::TextWrapped("%s", it->message.c_str());
                    if (ImGui::Button(translationClose[current_language])) {
                        it->start_time = {};
                    }
                break;

                case notification_type::friend_lobby_available: {
                    render_notif_friend_header(it->source_friend_id);
                    ImGui::TextWrapped("%s", it->message.c_str());
                    if (ImGui::Button("Request to Join")) {
                        Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
                        PRINT_DEBUG("user requesting join to lobby %" PRIu64 " (friend %" PRIu64 ")", it->join_request_lobby_id, it->source_friend_id);
                        mm->JoinLobby(CSteamID((uint64)it->join_request_lobby_id));
                        it->start_time = {};
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(translationClose[current_language])) {
                        it->start_time = {};
                    }
                }
                break;
                
                default:
                    PRINT_DEBUG("error unhandled notification for type %i", (int)it->type);
                break;
            }

            if (is_achievement) {
                const ImVec2 wnd_pos = ImGui::GetWindowPos();
                const ImVec2 wnd_size = ImGui::GetWindowSize();
                if (is_rare_achievement) {
                    // Glowing gold border for the notification window (same effect as the
                    // achievement list icon): 3 semi-transparent outer rings + a solid
                    // bright inner line.  Outer rings go in the background draw list so
                    // they can extend past the window bounds.
                    const float inset = 2.0f;
                    const ImVec2 border_min(wnd_pos.x + inset, wnd_pos.y + inset);
                    const ImVec2 border_max(wnd_pos.x + wnd_size.x - inset, wnd_pos.y + wnd_size.y - inset);
                    const float rounding = settings->overlay_appearance.notification_rounding;
                    const int   steps = 4;
                    const float step_px = 1.8f;
                    const float max_expand = step_px * (float)steps;

                    // Use the foreground draw list with a clip rect that extends past
                    // the window bounds, otherwise the window background clips the rings.
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->PushClipRect(
                        ImVec2(wnd_pos.x - max_expand, wnd_pos.y - max_expand),
                        ImVec2(wnd_pos.x + wnd_size.x + max_expand,
                               wnd_pos.y + wnd_size.y + max_expand),
                        false);
                    for (int i = steps; i >= 1; --i) {
                        const float expand = step_px * (float)i;
                        const ImVec2 lo(border_min.x - expand, border_min.y - expand);
                        const ImVec2 hi(border_max.x + expand, border_max.y + expand);
                        const int a = (int)(180.0f * (1.0f - (float)(i - 1) / (float)steps) * settings_noti_alpha);
                        dl->AddRect(lo, hi, IM_COL32(218, 165, 32, a), rounding + expand, 2.0f, 0);
                    }
                    dl->PopClipRect();

                    ImGui::GetWindowDrawList()->AddRect(
                        border_min,
                        border_max,
                        IM_COL32(255, 215, 90, (int)(220.0f * settings_noti_alpha)),
                        rounding,
                        2.0f,
                        0);
                } else {
                    const float inset = 1.0f;
                    const ImVec2 inner_min(wnd_pos.x + inset, wnd_pos.y + inset);
                    const ImVec2 inner_max(wnd_pos.x + wnd_size.x - inset, wnd_pos.y + wnd_size.y - inset);
                    const ImU32 accent_color = ImGui::ColorConvertFloat4ToU32(
                        ImVec4(1.0f, 1.0f, 1.0f, 0.82f * settings_noti_alpha));

                    ImGui::GetWindowDrawList()->AddRect(
                        inner_min,
                        inner_max,
                        accent_color,
                        settings->overlay_appearance.notification_rounding,
                        2.0f,
                        0);
                }
            }

            if (is_rare_achievement) {
                const float shimmer_duration_ms = 700.0f;
                const float shimmer_delay_ms = 450.0f;
                const float shimmer_elapsed_ms = static_cast<float>(
                    elapsed_notif.count() - settings->overlay_appearance.notification_animation) - shimmer_delay_ms;
                if (shimmer_elapsed_ms >= 0.0f && shimmer_elapsed_ms <= shimmer_duration_ms) {
                    float shimmer_t = shimmer_elapsed_ms / shimmer_duration_ms;
                    if (shimmer_t < 0.0f) shimmer_t = 0.0f;
                    if (shimmer_t > 1.0f) shimmer_t = 1.0f;

                    const ImVec2 wnd_pos = ImGui::GetWindowPos();
                    const ImVec2 wnd_size = ImGui::GetWindowSize();
                    const ImVec2 wnd_max(wnd_pos.x + wnd_size.x, wnd_pos.y + wnd_size.y);
                    const float sweep_width = wnd_size.x * 0.18f;
                    const float sweep_x = wnd_pos.x - sweep_width + (wnd_size.x + sweep_width * 2.0f) * shimmer_t;
                    const float alpha = (1.0f - shimmer_t) * 0.22f * settings_noti_alpha;

                    ImDrawList* draw_list = ImGui::GetWindowDrawList();
                    draw_list->PushClipRect(wnd_pos, wnd_max, true);
                    draw_list->AddQuadFilled(
                        ImVec2(sweep_x - sweep_width, wnd_pos.y),
                        ImVec2(sweep_x, wnd_pos.y),
                        ImVec2(sweep_x + sweep_width, wnd_max.y),
                        ImVec2(sweep_x, wnd_max.y),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.88f, 0.45f, alpha)));
                    draw_list->PopClipRect();
                }
            }

        }

        ImGui::End();

        if (is_rare_achievement) {
            ImGui::PopStyleVar();
        }
        ImGui::PopStyleColor(3);
    }

    ImGui::PopStyleVar();
    ImGui::PopFont();

    // erase all notifications whose visible time exceeded the max
    notifications.erase(std::remove_if(notifications.begin(), notifications.end(), [this](const Notification &item) {
        if (item.expired) {
            PRINT_DEBUG("removing a notification");
            allow_renderer_frame_processing(false);
            // uncomment this block to restore app input focus
            switch ((notification_type)item.type) {
                // we want to restore focus for these ones
                case notification_type::invite:
                    obscure_game_input(false);
                break;

                case notification_type::lobby_join_request:
                    obscure_game_input(false);
                    // clean up tracking if expired without action
                    notified_lobby_join_requests.erase({item.join_request_lobby_id, item.join_request_requester_id});
                break;

                case notification_type::friend_lobby_available:
                    obscure_game_input(false);
                break;

                // not effective
                case notification_type::achievement_progress:
                case notification_type::achievement:
                case notification_type::auto_accept_invite:
                case notification_type::message:
                case notification_type::lobby_join_request_response:
                case notification_type::lobby_kicked:
                case notification_type::screenshot:
                    // nothing
                break;

                default:
                    PRINT_DEBUG("error unhandled remove for type %i", (int)item.type);
                break;
            }

            // Archive to notification history (lightweight copy, no pointers/GPU resources)
            {
                NotificationHistoryEntry entry{};
                // Use actual achievement unlock time when available,
                // otherwise fall back to the notification display time.
                if (item.ach.has_value() && item.ach->unlock_time > 0) {
                    entry.timestamp = std::chrono::milliseconds(
                        static_cast<long long>(item.ach->unlock_time) * 1000);
                } else {
                    entry.timestamp = item.start_time;
                }
                entry.type = item.type;
                entry.message = item.message;
                if (notification_history.size() >= MAX_NOTIFICATION_HISTORY) {
                    notification_history.pop_front();
                }
                notification_history.push_back(std::move(entry));
                notification_history_cache_dirty = true;
            }

            return true;
        }

        return false;
    }), notifications.end());

    if (!friend_actions_temp.empty()) {
        while (!friend_actions_temp.empty()) {
            has_friend_action.push(friend_actions_temp.front());
            friend_actions_temp.pop();
        }
    }
}

void Steam_Overlay::add_auto_accept_invite_notification()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    char tmp[TRANSLATION_BUFFER_SIZE]{};
    snprintf(tmp, sizeof(tmp), "%s", translationAutoAcceptFriendInvite[current_language]);

    submit_notification(notification_type::auto_accept_invite, tmp);
    notify_sound_auto_accept_friend_invite();
}

void Steam_Overlay::add_invite_notification(std::pair<const Friend, friend_window_state>& wnd_state)
{
    if (settings->disable_overlay_friend_notification) return;

    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    char tmp[TRANSLATION_BUFFER_SIZE]{};
    auto &first_friend = wnd_state.first;
    auto &name = first_friend.name();
    snprintf(tmp, sizeof(tmp), translationInvitedYouToJoinTheGame[current_language], name.c_str(), (uint64)first_friend.appid());

    submit_notification(notification_type::invite, tmp, &wnd_state);
}

void Steam_Overlay::post_achievement_notification(Overlay_Achievement &ach, bool for_progress)
{
    if (settings->disable_overlay_achievement_notification) return;

    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
// Get current time
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());

    // Calculate scheduled show time based on rate limiting
    std::chrono::milliseconds scheduled_show_time;
    int delay_ms = settings->achievement_notification_delay_ms;

    PRINT_DEBUG("Achievement delay: %d ms", delay_ms);

    if (delay_ms <= 0) {
        // No delay - show immediately
        scheduled_show_time = now;
    } else {
        // Apply rate limiting: earliest show time is last_scheduled_show_time + delay
        scheduled_show_time = std::max(now, last_scheduled_show_time + std::chrono::milliseconds(delay_ms));
    }

    // Create scheduled achievement entry
    ScheduledAchievement scheduled_ach;
    scheduled_ach.ach = ach;
    scheduled_ach.for_progress = for_progress;
    scheduled_ach.trigger_time = now;
    scheduled_ach.scheduled_show_time = scheduled_show_time;

    // Add to queue
    achievement_queue.push_back(scheduled_ach);

    // Update last scheduled show time for next item
    last_scheduled_show_time = scheduled_show_time;

    PRINT_DEBUG("Achievement queued: '%s', scheduled for %lld ms, delay=%d ms", 
                ach.name.c_str(), (long long)scheduled_show_time.count(), delay_ms);
}

void Steam_Overlay::process_achievement_queue()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    if (achievement_queue.empty()) return;

    // Get current time
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());

    // Process all ready achievements
    while (!achievement_queue.empty()) {
        auto& scheduled_ach = achievement_queue.front();

        // Check if it's time to show this notification
        PRINT_DEBUG("Queue check: scheduled=%lld, now=%lld, diff=%lld",
                    (long long)scheduled_ach.scheduled_show_time.count(),
                    (long long)now.count(),
                    (long long)(now - scheduled_ach.scheduled_show_time).count());
        if (scheduled_ach.scheduled_show_time <= now) {
            // Show the notification
            bool achieved = !scheduled_ach.for_progress;
            // force upload to GPU if the pagination is request-based
            try_load_ach_icon(scheduled_ach.ach, achieved, settings->paginated_achievements_icons == 0);

            submit_notification(
                scheduled_ach.for_progress ? notification_type::achievement_progress : notification_type::achievement,
                scheduled_ach.ach.title + "\n" + scheduled_ach.ach.description,
                {},
                &scheduled_ach.ach
            );

            // Play sound when notification is actually shown (delayed with queue)
            notify_sound_user_achievement();

            PRINT_DEBUG("Achievement shown: '%s' at %lld ms", 
                        scheduled_ach.ach.name.c_str(), (long long)now.count());

            // Remove from queue
            achievement_queue.pop_front();
        } else {
            // This achievement is not ready yet, and queue is ordered by scheduled time,
            // so no more achievements will be ready either
            break;
        }
    }

}

// Per-display HDR + colour-space info queried from the OS for the overlay info panel.
struct DisplayHdrDetail_t {
    bool hdr_supported   = false;
    bool hdr_enabled     = false;
    bool wide_color      = false;   // WCG enforced (HDR10 WCG without full tone-mapping)
    bool force_disabled  = false;   // HDR suppressed by Windows for compatibility
    int  bpc             = 0;       // bits per colour channel; 0 = unknown
    int  sdr_white_nits  = -1;      // SDR white point in nits; -1 = unavailable
    char encoding[16]    = {};      // "RGB", "YCbCr444", "YCbCr422", "YCbCr420"
    char gamut[24]       = {};      // inferred: "BT.2020", "P3/BT.2020 WCG", "sRGB/BT.709"
    char transfer[16]    = {};      // inferred: "PQ/ST.2084", "sRGB-ext", "gamma 2.2"
    char range[8]        = {};      // inferred: "Full", "Limited", "?"
    char name[128]       = {};      // friendly monitor name (UTF-8)
};

static std::vector<DisplayHdrDetail_t> query_display_hdr_details()
{
    std::vector<DisplayHdrDetail_t> result;
#ifdef __WINDOWS__
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return result;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return result;
    for (UINT32 i = 0; i < pathCount; ++i) {
        DisplayHdrDetail_t d{};

        // Advanced colour / HDR state
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO aci{};
        aci.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        aci.header.size      = sizeof(aci);
        aci.header.adapterId = paths[i].targetInfo.adapterId;
        aci.header.id        = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&aci.header) == ERROR_SUCCESS) {
            d.hdr_supported  = aci.advancedColorSupported     != 0;
            d.hdr_enabled    = aci.advancedColorEnabled       != 0;
            d.wide_color     = aci.wideColorEnforced          != 0;
            d.force_disabled = aci.advancedColorForceDisabled != 0;
            d.bpc            = static_cast<int>(aci.bitsPerColorChannel);
            switch (aci.colorEncoding) {
                case DISPLAYCONFIG_COLOR_ENCODING_RGB:      strncpy(d.encoding, "RGB",      sizeof(d.encoding)-1); break;
                case DISPLAYCONFIG_COLOR_ENCODING_YCBCR444: strncpy(d.encoding, "YCbCr444", sizeof(d.encoding)-1); break;
                case DISPLAYCONFIG_COLOR_ENCODING_YCBCR422: strncpy(d.encoding, "YCbCr422", sizeof(d.encoding)-1); break;
                case DISPLAYCONFIG_COLOR_ENCODING_YCBCR420: strncpy(d.encoding, "YCbCr420", sizeof(d.encoding)-1); break;
                default:                                     strncpy(d.encoding, "?",        sizeof(d.encoding)-1); break;
            }
        }

        // SDR white level (nits) — Windows 10 1809+
        // DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL = 11
        // Value: nits = SDRWhiteLevel * 80 / 1000  (1000 -> 80 nits standard, 2500 -> 200 nits)
        struct SdrWlReq { DISPLAYCONFIG_DEVICE_INFO_HEADER header; UINT32 SDRWhiteLevel; };
        SdrWlReq wl{};
        wl.header.type      = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(11);
        wl.header.size      = sizeof(wl);
        wl.header.adapterId = paths[i].targetInfo.adapterId;
        wl.header.id        = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS)
            d.sdr_white_nits = static_cast<int>(wl.SDRWhiteLevel * 80 / 1000);

        // Friendly monitor name
        DISPLAYCONFIG_TARGET_DEVICE_NAME tdn{};
        tdn.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tdn.header.size      = sizeof(tdn);
        tdn.header.adapterId = paths[i].targetInfo.adapterId;
        tdn.header.id        = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tdn.header) == ERROR_SUCCESS && tdn.monitorFriendlyDeviceName[0])
            WideCharToMultiByte(CP_UTF8, 0, tdn.monitorFriendlyDeviceName, -1, d.name, sizeof(d.name)-1, nullptr, nullptr);
        if (!d.name[0])
            strncpy(d.name, "Unknown Display", sizeof(d.name)-1);

        // Inferred transfer function / gamma
        if (d.hdr_enabled)
            strncpy(d.transfer, "PQ/ST.2084", sizeof(d.transfer)-1);
        else if (d.wide_color)
            strncpy(d.transfer, "sRGB-ext", sizeof(d.transfer)-1);
        else
            strncpy(d.transfer, "gamma 2.2", sizeof(d.transfer)-1);

        // Inferred color gamut
        if (d.hdr_enabled)
            strncpy(d.gamut, "BT.2020", sizeof(d.gamut)-1);
        else if (d.wide_color)
            strncpy(d.gamut, "P3/BT.2020 WCG", sizeof(d.gamut)-1);
        else
            strncpy(d.gamut, "sRGB/BT.709", sizeof(d.gamut)-1);

        // Inferred color range (RGB on PC = full; YCbCr = limited)
        if (d.encoding[0] == 'R')
            strncpy(d.range, "Full", sizeof(d.range)-1);
        else if (d.encoding[0] == 'Y')
            strncpy(d.range, "Limited", sizeof(d.range)-1);
        else
            strncpy(d.range, "?", sizeof(d.range)-1);

        result.push_back(d);
    }
#endif
    return result;
}

// Query display HDR info, update s_sdr_white_scale, and mark the scale as queried.
// Returns the display list so the settings panel can display it without double-querying.
// Scale selection: prefers the first HDR-enabled display; falls back to the first display
// with any valid sdr_white_nits value; otherwise keeps the current scale.
static std::vector<DisplayHdrDetail_t> refresh_sdr_white_scale()
{
    auto displays = query_display_hdr_details();
    s_sdr_scale_queried = true;
    int best_nits = -1;
    // First pass: HDR-active display
    for (const auto& d : displays)
        if (d.hdr_enabled && d.sdr_white_nits > 0) { best_nits = d.sdr_white_nits; break; }
    // Second pass: any display with a valid reading
    if (best_nits < 0)
        for (const auto& d : displays)
            if (d.sdr_white_nits > 0) { best_nits = d.sdr_white_nits; break; }
    if (best_nits > 0)
        s_sdr_white_scale = best_nits / 80.0f;
    else
        s_sdr_white_scale = 1.0f; // fallback: standard 80 nits
    return displays;
}

// sRGB->linear decode before GPU upload.
// ingame_overlay forces DXGI_FORMAT_R8G8B8A8_UNORM on its own RTV so there is NO
// hardware sRGB encoding at the overlay render pass.  sRGB-encoded PNG bytes write
// as-is into the back buffer regardless of the swap-chain format.
//
// When to decode / adjust:
//   FP16/scRGB swap chain (R16G16B16A16_FLOAT)  — HDR path:
//     sRGB bytes are stored as linear-space floats and appear over-dark/over-saturated
//     without pre-decoding.  We decode sRGB->linear and then apply a pow(0.75) brightness
//     lift so shadow/midtone values are raised and the harsh contrast of the raw gamma-2.4
//     curve is reduced.
//   8-bit SDR (UNORM / UNORM_SRGB)  — SDR path:
//     UNORM bytes pass through to the display correctly, but a gentle 1.15× contrast boost
//     (applied in the sRGB domain around mid-grey) is added when running on a modern API
//     with a confirmed SDR swap chain.
//   HDR10 (R10G10B10A2, PQ gamma):
//     sRGB decode would produce wrong PQ colours.  NO decode, NO contrast.
//   DX9 / OpenGL: linear framebuffer, no sRGB path.  NO adjustment.
//
// Controlled by Overlay_Appearance::image_gamma (auto/on/off).
// "auto": HDR decode + lift ONLY when confirmed FP16/scRGB; SDR contrast on confirmed SDR.
// "on":   always apply HDR decode + lift.
// "off":  never adjust.

static void srgb_decode_pixels_if_needed(InGameOverlay::RendererHook_t *renderer,
                                         const Overlay_Appearance::SrgbDecode mode,
                                         uint8_t *rgba, size_t npixels)
{
    using RHT = InGameOverlay::RendererHookType_t;
    using SD  = Overlay_Appearance::SrgbDecode;

    // Determine which transform path to take based on the swap chain classification.
    // Possible paths:
    //   decode_linear_hdr  : sRGB→linear + SDR-white scale  (scRGB / FP16 / FP32 / 16-bit linear)
    //   decode_pq          : sRGB→linear→PQ encode + nit scale  (HDR10 PQ swap chains)
    //   decode_srgb_rtv    : sRGB→linear only (no scale)  (_SRGB back-buffer with hw encoding)
    //   sdr_contrast       : mild 1.15× contrast boost  (SDR UNORM, modern APIs)
    //   (none)             : raw pass-through
    bool decode_linear_hdr = false;
    bool decode_pq         = false;
    bool decode_srgb_rtv   = false;
    bool sdr_contrast      = false;

    bool modern_api = false;
    if (renderer) {
        switch (renderer->GetRendererHookType()) {
            case RHT::DirectX10:
            case RHT::DirectX11:
            case RHT::DirectX12:
            case RHT::Vulkan:
            case RHT::Metal:
                modern_api = true;
                break;
            default:
                break;
        }
    }

    const SwapchainColorSpace ecs = effective_swapchain_cs();

    if (mode == SD::On) {
        // Forced on: pick path based on effective colour space, or fall back to linear HDR decode.
        switch (ecs) {
            case SCS_HDR10_PQ:     decode_pq = true;         break;
            case SCS_SDR_SRGB_RTV: decode_srgb_rtv = true;   break;
            default:               decode_linear_hdr = true;  break;
        }
    } else if (mode == SD::Auto && modern_api) {
        switch (ecs) {
            case SCS_LINEAR_HDR:   decode_linear_hdr = true;  break;
            case SCS_HDR10_PQ:     decode_pq = true;          break;
            case SCS_SDR_SRGB_RTV: decode_srgb_rtv = true;    break;
            case SCS_SDR_UNORM:    sdr_contrast = true;       break;
            default: break; // SCS_UNKNOWN: no transform
        }
    }

    if (!decode_linear_hdr && !decode_pq && !decode_srgb_rtv && !sdr_contrast) return;

    // Apply the user's brightness / contrast / gamma adjustments in linear space.
    // This runs as a pre-pass in the sRGB domain, so every transfer path below
    // (linear HDR / PQ / sRGB RTV / SDR) works on already-adjusted pixels:
    //   adjusted = srgb_encode(adjust(srgb_decode(byte)))
    // which is exactly what the FP16 path does via transform_pixels_to_fp16(),
    // keeping the 8-bit fallback consistent with it (just with less precision).
    const bool has_adj = (s_img_brightness != 1.0f || s_img_contrast != 1.0f || s_img_gamma_adj != 1.0f);
    if (has_adj) {
        static uint8_t adj_lut[256];
        static float   adj_lut_for_b = -1.0f, adj_lut_for_c = -1.0f, adj_lut_for_g = -1.0f;
        if (adj_lut_for_b != s_img_brightness || adj_lut_for_c != s_img_contrast || adj_lut_for_g != s_img_gamma_adj) {
            adj_lut_for_b = s_img_brightness;
            adj_lut_for_c = s_img_contrast;
            adj_lut_for_g = s_img_gamma_adj;
            for (int i = 0; i < 256; ++i) {
                float s = i / 255.0f;
                float l = (s <= 0.04045f) ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
                if (s_img_gamma_adj != 1.0f && l > 0.0f)
                    l = powf(l, s_img_gamma_adj);
                if (s_img_contrast != 1.0f)
                    l = (l - 0.5f) * s_img_contrast + 0.5f;
                if (s_img_brightness != 1.0f)
                    l *= s_img_brightness;
                if (l < 0.0f) l = 0.0f; else if (l > 1.0f) l = 1.0f;
                const float e = (l <= 0.0031308f) ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
                adj_lut[i] = (uint8_t)(fminf(e * 255.0f + 0.5f, 255.0f));
            }
        }
        for (size_t i = 0; i < npixels; ++i, rgba += 4) {
            rgba[0] = adj_lut[rgba[0]];
            rgba[1] = adj_lut[rgba[1]];
            rgba[2] = adj_lut[rgba[2]];
        }
    }

    if (decode_linear_hdr) {
        // HDR / FP16-scRGB path: sRGB→linear decode scaled by s_sdr_white_scale.
        // s_sdr_white_scale = sdr_white_nits / 80.0f (queried from the OS display API).
        // scRGB convention: 1.0 = 80 nits.  If Windows SDR white is 200 nits, the OS
        // scales SDR content by 2.5× when compositing.  We apply the same factor so our
        // overlay sits at the same perceived brightness as the game's own SDR UI.
        // To mitigate the uint8 saturation problem for high SDR-white scales, we use a
        // soft shoulder (Reinhard-like) that compresses highlights instead of hard-clipping.
        static uint8_t hdr_lut[256];
        static float   hdr_lut_built_for = -1.0f;
        if (hdr_lut_built_for != s_sdr_white_scale) {
            hdr_lut_built_for = s_sdr_white_scale;
            for (int i = 0; i < 256; ++i) {
                float s = i / 255.0f;
                float l = (s <= 0.04045f) ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
                l *= s_sdr_white_scale; // lift to match display SDR white brightness
                // Soft shoulder: Reinhard tone-map when scale > 1 to avoid hard clipping.
                // Maps [0, ∞) → [0, 1).  At scale=1 it's close to identity below 1.0.
                if (s_sdr_white_scale > 1.0f)
                    l = l / (1.0f + l);
                hdr_lut[i] = (uint8_t)(fminf(l * 255.0f + 0.5f, 255.0f));
            }
        }
        for (size_t i = 0; i < npixels; ++i, rgba += 4) {
            rgba[0] = hdr_lut[rgba[0]];
            rgba[1] = hdr_lut[rgba[1]];
            rgba[2] = hdr_lut[rgba[2]];
        }
    } else if (decode_pq) {
        // HDR10 PQ path: sRGB → linear → PQ (ST.2084) encode.
        // PQ reference: ITU-R BT.2100, SMPTE ST 2084.
        //   m1 = 2610 / 16384 = 0.1593017578125
        //   m2 = 2523 / 32    = 78.84375
        //   c1 = 3424 / 4096  = 0.8359375    (= c3 − c2 + 1)
        //   c2 = 2413 / 128   = 18.8515625
        //   c3 = 2392 / 128   = 18.6875
        // Input:  linear light normalised to 10000 nits  →  L = linear_nits / 10000.
        // Output: PQ value [0, 1] → scale to uint8 [0, 255].
        // SDR UI at s_sdr_white_scale*80 nits (e.g. 200 nits) → PQ ≈ 0.509 → uint8 ≈ 130.
        static uint8_t pq_lut[256];
        static float   pq_lut_built_for = -1.0f;
        if (pq_lut_built_for != s_sdr_white_scale) {
            pq_lut_built_for = s_sdr_white_scale;
            constexpr float m1 = 0.1593017578125f;
            constexpr float m2 = 78.84375f;
            constexpr float c1 = 0.8359375f;
            constexpr float c2 = 18.8515625f;
            constexpr float c3 = 18.6875f;
            for (int i = 0; i < 256; ++i) {
                float s = i / 255.0f;
                // sRGB EOTF → linear [0,1] where 1 = diffuse white
                float lin = (s <= 0.04045f) ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
                // Scale to absolute nits, then normalise to 10000.
                float nits = lin * s_sdr_white_scale * 80.0f; // e.g. 1.0 * 2.5 * 80 = 200 nits
                float Y = fminf(nits / 10000.0f, 1.0f);
                // PQ OETF
                float Ym1 = powf(Y, m1);
                float pq  = powf((c1 + c2 * Ym1) / (1.0f + c3 * Ym1), m2);
                pq_lut[i] = (uint8_t)(fminf(pq * 255.0f + 0.5f, 255.0f));
            }
        }
        for (size_t i = 0; i < npixels; ++i, rgba += 4) {
            rgba[0] = pq_lut[rgba[0]];
            rgba[1] = pq_lut[rgba[1]];
            rgba[2] = pq_lut[rgba[2]];
        }
    } else if (decode_srgb_rtv) {
        // _SRGB back-buffer path: the RTV has hardware sRGB encoding, so the GPU will
        // apply sRGB OETF on write.  If we feed sRGB bytes, they get double-encoded
        // (too dark).  Fix: decode to linear first; the hardware re-encodes to sRGB.
        // No SDR-white scale — this is an SDR display path.
        static uint8_t srgb_rtv_lut[256];
        static bool srgb_rtv_lut_ready = false;
        if (!srgb_rtv_lut_ready) {
            for (int i = 0; i < 256; ++i) {
                float s = i / 255.0f;
                float l = (s <= 0.04045f) ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
                srgb_rtv_lut[i] = (uint8_t)(fminf(l * 255.0f + 0.5f, 255.0f));
            }
            srgb_rtv_lut_ready = true;
        }
        for (size_t i = 0; i < npixels; ++i, rgba += 4) {
            rgba[0] = srgb_rtv_lut[rgba[0]];
            rgba[1] = srgb_rtv_lut[rgba[1]];
            rgba[2] = srgb_rtv_lut[rgba[2]];
        }
    } else {
        // SDR path: mild contrast boost (~1.15×) in the sRGB domain around mid-grey.
        static uint8_t sdr_lut[256];
        static bool sdr_lut_ready = false;
        if (!sdr_lut_ready) {
            constexpr float kContrast = 1.15f;
            for (int i = 0; i < 256; ++i) {
                float s = i / 255.0f;
                float l = (s - 0.5f) * kContrast + 0.5f;
                l = (l < 0.f ? 0.f : (l > 1.f ? 1.f : l));
                sdr_lut[i] = (uint8_t)(l * 255.0f + 0.5f);
            }
            sdr_lut_ready = true;
        }
        for (size_t i = 0; i < npixels; ++i, rgba += 4) {
            rgba[0] = sdr_lut[rgba[0]];
            rgba[1] = sdr_lut[rgba[1]];
            rgba[2] = sdr_lut[rgba[2]];
        }
    }
}

// Decode an sRGB float channel to linear and scale by s_sdr_white_scale.
// Used to adjust ImGui style colors for linear-HDR and PQ swap chains.
// For _SRGB back-buffers, decode to linear only (no scale).
static float srgb_ch_decode_scale(float c)
{
    float l = (c <= 0.04045f) ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    return l * s_sdr_white_scale;
}
// PQ OETF for a single float channel (used for ImGui colours on HDR10 PQ swap chains).
static float srgb_ch_to_pq(float c)
{
    constexpr float m1 = 0.1593017578125f;
    constexpr float m2 = 78.84375f;
    constexpr float c1 = 0.8359375f;
    constexpr float c2 = 18.8515625f;
    constexpr float c3 = 18.6875f;
    float lin = (c <= 0.04045f) ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    float nits = lin * s_sdr_white_scale * 80.0f;
    float Y = fminf(nits / 10000.0f, 1.0f);
    float Ym1 = powf(Y, m1);
    return powf((c1 + c2 * Ym1) / (1.0f + c3 * Ym1), m2);
}
// sRGB→linear only (no scale) — for _SRGB back-buffer.
static float srgb_ch_decode_only(float c)
{
    return (c <= 0.04045f) ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static ImVec4 adjust_imgui_color_for_swapchain(ImVec4 c)
{
    // Only transform when a non-SDR-UNORM colour space is confirmed.  Alpha preserved as-is.
    switch (effective_swapchain_cs()) {
        case SCS_LINEAR_HDR:
            return ImVec4(srgb_ch_decode_scale(c.x),
                          srgb_ch_decode_scale(c.y),
                          srgb_ch_decode_scale(c.z), c.w);
        case SCS_HDR10_PQ:
            return ImVec4(srgb_ch_to_pq(c.x),
                          srgb_ch_to_pq(c.y),
                          srgb_ch_to_pq(c.z), c.w);
        case SCS_SDR_SRGB_RTV:
            return ImVec4(srgb_ch_decode_only(c.x),
                          srgb_ch_decode_only(c.y),
                          srgb_ch_decode_only(c.z), c.w);
        default:
            return c;
    }
}

static ImU32 adjust_imgui_color_u32_for_swapchain(ImU32 c)
{
    if (effective_swapchain_cs() == SCS_SDR_UNORM || effective_swapchain_cs() == SCS_UNKNOWN) return c;
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v = adjust_imgui_color_for_swapchain(v);
    return ImGui::ColorConvertFloat4ToU32(v);
}

// ── FP16 texture upload helpers ──────────────────────────────────────────────
// IEEE 754 binary16 (half-float) conversion.
static inline uint16_t float_to_half(float value)
{
    uint32_t f32;
    memcpy(&f32, &value, 4);
    uint32_t sign = (f32 >> 16) & 0x8000;
    int32_t  exp  = ((f32 >> 23) & 0xFF) - 127;
    uint32_t mant = f32 & 0x7FFFFF;
    if (exp > 15) return (uint16_t)(sign | 0x7C00);             // overflow → ±inf
    if (exp < -14) {                                             // denorm / underflow
        if (exp < -24) return (uint16_t)sign;                    // too small → ±0
        mant |= 0x800000;
        uint32_t shift = (uint32_t)(-exp - 1 + 13);             // 14..24 → 13..23
        return (uint16_t)(sign | (mant >> shift));
    }
    return (uint16_t)(sign | ((exp + 15) << 10) | (mant >> 13));
}

// sRGB EOTF for pixel transforms (same as srgb_ch_decode_only but for uint8→float).
static inline float srgb_byte_to_linear(uint8_t b)
{
    float s = b / 255.0f;
    return (s <= 0.04045f) ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
}

// Apply per-image brightness / contrast / gamma adjustments in linear space.
static inline float apply_image_adjustments(float lin, float brightness, float contrast, float gamma_adj)
{
    if (gamma_adj != 1.0f && lin > 0.0f)
        lin = powf(lin, gamma_adj);
    if (contrast != 1.0f)
        lin = (lin - 0.5f) * contrast + 0.5f;
    if (brightness != 1.0f)
        lin *= brightness;
    return lin < 0.0f ? 0.0f : lin;
}

// Convert RGBA8 sRGB pixels → R16G16B16A16_FLOAT with colour-space transform baked in.
static std::vector<uint16_t> transform_pixels_to_fp16(const uint8_t *pixels, int w, int h,
                                                      SwapchainColorSpace cs, float sdr_scale,
                                                      float brightness = 1.0f, float contrast = 1.0f,
                                                      float gamma_adj = 1.0f)
{
    const int count = w * h;
    std::vector<uint16_t> out(count * 4);
    const bool has_adj = (brightness != 1.0f || contrast != 1.0f || gamma_adj != 1.0f);
    if (cs == SCS_LINEAR_HDR) {
        for (int i = 0; i < count; ++i) {
            const uint8_t *s = pixels + i * 4;
            uint16_t      *d = out.data() + i * 4;
            for (int ch = 0; ch < 3; ++ch) {
                float lin = srgb_byte_to_linear(s[ch]);
                if (has_adj) lin = apply_image_adjustments(lin, brightness, contrast, gamma_adj);
                d[ch] = float_to_half(lin * sdr_scale);
            }
            d[3] = float_to_half(s[3] / 255.0f);
        }
    } else if (cs == SCS_HDR10_PQ) {
        constexpr float m1 = 0.1593017578125f, m2 = 78.84375f;
        constexpr float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
        const float nits = sdr_scale * 80.0f;
        for (int i = 0; i < count; ++i) {
            const uint8_t *s = pixels + i * 4;
            uint16_t      *d = out.data() + i * 4;
            for (int ch = 0; ch < 3; ++ch) {
                float lin = srgb_byte_to_linear(s[ch]);
                if (has_adj) lin = apply_image_adjustments(lin, brightness, contrast, gamma_adj);
                float L = fminf((lin * nits) / 10000.0f, 1.0f);
                float Lm1 = powf(L, m1);
                d[ch] = float_to_half(powf((c1 + c2 * Lm1) / (1.0f + c3 * Lm1), m2));
            }
            d[3] = float_to_half(s[3] / 255.0f);
        }
    } else if (cs == SCS_SDR_SRGB_RTV) {
        for (int i = 0; i < count; ++i) {
            const uint8_t *s = pixels + i * 4;
            uint16_t      *d = out.data() + i * 4;
            for (int ch = 0; ch < 3; ++ch) {
                float lin = srgb_byte_to_linear(s[ch]);
                if (has_adj) lin = apply_image_adjustments(lin, brightness, contrast, gamma_adj);
                d[ch] = float_to_half(lin);
            }
            d[3] = float_to_half(s[3] / 255.0f);
        }
    } else {
        // SDR / Unknown: 8-bit → FP16 with optional adjustments
        for (int i = 0; i < count; ++i) {
            const uint8_t *s = pixels + i * 4;
            uint16_t      *d = out.data() + i * 4;
            for (int ch = 0; ch < 3; ++ch) {
                float v = s[ch] / 255.0f;
                if (has_adj) {
                    float lin = srgb_byte_to_linear(s[ch]);
                    lin = apply_image_adjustments(lin, brightness, contrast, gamma_adj);
                    v = lin <= 0.0031308f ? lin * 12.92f : 1.055f * powf(lin, 1.0f / 2.4f) - 0.055f;
                    if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
                }
                d[ch] = float_to_half(v);
            }
            d[3] = float_to_half(s[3] / 255.0f);
        }
    }
    return out;
}

// Capability of the active renderer backend, refreshed once per frame in
// overlay_render_proc(). Uploading RGBA16F requires the backend to actually
// understand that format: a backend that does not would reinterpret the buffer as
// RGBA8 and corrupt the image instead of reporting an error, so this must be
// checked before every upload (the 8-bit path is still colour-correct, just lossy).
static bool s_renderer_supports_fp16 = false;

// Decide whether FP16 upload is available and appropriate.
// Also used when image adjustments are active (even on SDR) for better precision.
static bool should_use_fp16()
{
    if (!s_renderer_supports_fp16)
        return false;

    SwapchainColorSpace cs = effective_swapchain_cs();
    if (cs == SCS_LINEAR_HDR || cs == SCS_HDR10_PQ || cs == SCS_SDR_SRGB_RTV)
        return true;
    return (s_img_brightness != 1.0f || s_img_contrast != 1.0f || s_img_gamma_adj != 1.0f);
}
// ─────────────────────────────────────────────────────────────────────────────

bool Steam_Overlay::try_load_ach_icon(Overlay_Achievement &ach, bool achieved, bool upload_new_icon_to_gpu)
{
    if (!_renderer) return false;
    if (settings->paginated_achievements_icons < 0) return false; // no icons are loaded anyway
    if (!settings->overlay_upload_achs_icons_to_gpu) return false; // don't upload anything to the GPU

    auto &icon_rsrc = achieved ? ach.icon : ach.icon_gray;
    if (icon_rsrc->GetResourceId() != 0) return true;

    // icons needs to be loaded, but we're not allowed
    if (!upload_new_icon_to_gpu) return false;

    int &icon_handle = achieved ? ach.icon_handle : ach.icon_gray_handle;
    if (Settings::UNLOADED_IMAGE_HANDLE == icon_handle) { // not loaded yet
        icon_handle = get_steam_client()->steam_user_stats->get_achievement_icon_handle(ach.name, achieved);
    }
    auto image_info = settings->get_image(icon_handle);
    if (image_info) {
        const int iw = static_cast<int>(image_info->width);
        const int ih = static_cast<int>(image_info->height);
        if (iw <= 0 || ih <= 0 || image_info->data.size() < (size_t)iw * ih * 4) return false;
        // Store in the struct — AttachResource holds a raw pointer, so the buffer must outlive the resource
        auto& icon_decoded_data = achieved ? ach.icon_decoded_data : ach.icon_gray_decoded_data;
        if (should_use_fp16()) {
            auto fp16 = transform_pixels_to_fp16((const uint8_t*)image_info->data.data(), iw, ih,
                                                  effective_swapchain_cs(), s_sdr_white_scale,
                                                  s_img_brightness, s_img_contrast, s_img_gamma_adj);
            icon_decoded_data.assign(reinterpret_cast<const char*>(fp16.data()), fp16.size() * sizeof(uint16_t));
            icon_rsrc->AttachResource((void*)icon_decoded_data.data(), (uint32_t)iw, (uint32_t)ih,
                                      InGameOverlay::RendererPixelFormat::RGBA16F);
        } else {
            icon_decoded_data = image_info->data;
            srgb_decode_pixels_if_needed(_renderer, settings->overlay_appearance.image_gamma,
                (uint8_t*)icon_decoded_data.data(), (size_t)iw * ih);
            //int icon_size = static_cast<int>(settings->overlay_appearance.icon_size);
            //icon_rsrc->SetAutoLoad(InGameOverlay::ResourceAutoLoad_t::OnUse);
            //icon_rsrc->AttachResource((void*)image_info->data.c_str(), icon_size, icon_size);
            icon_rsrc->AttachResource((void*)icon_decoded_data.data(), (uint32_t)iw, (uint32_t)ih);
        }
        
        PRINT_DEBUG("'%s' (%dx%d, result=%i)", ach.name.c_str(), iw, ih, (int)icon_rsrc->GetResourceId() != 0);
    }

    return icon_rsrc->GetResourceId() != 0;
}

bool Steam_Overlay::try_load_avatar(friend_window_state &state, uint64 steam_id)
{
    if (!_renderer) return false;
    
    // Already loaded?
    if (state.avatar_resource && state.avatar_resource->GetResourceId() != 0) return true;
    
    // Create resource if not exist
    if (!state.avatar_resource) {
        state.avatar_resource = _renderer->CreateResource();
        if (!state.avatar_resource) return false;
    }
    
    // Try to get avatar handle
    if (state.avatar_handle == -1) {
        Steam_Friends *steamFriends = get_steam_client()->steam_friends;
        if (!steamFriends) return false;
        state.avatar_handle = steamFriends->GetMediumFriendAvatar(CSteamID(steam_id));
    }
    if (state.avatar_handle <= 0) return false;
    
    // Get image data
    Image_Data *img = settings->get_image(state.avatar_handle);
    if (!img || img->data.empty()) return false;
    
    int iw = static_cast<int>(img->width);
    int ih = static_cast<int>(img->height);
    if (iw <= 0 || ih <= 0 || img->data.size() < (size_t)iw * ih * 4) return false;
    
    // Store pixel data - AttachResource holds a raw pointer so buffer must outlive resource
    if (should_use_fp16()) {
        auto fp16 = transform_pixels_to_fp16((const uint8_t*)img->data.data(), iw, ih,
                                              effective_swapchain_cs(), s_sdr_white_scale,
                                              s_img_brightness, s_img_contrast, s_img_gamma_adj);
        state.avatar_pixels.assign(reinterpret_cast<const char*>(fp16.data()), fp16.size() * sizeof(uint16_t));
        state.avatar_resource->AttachResource((void*)state.avatar_pixels.data(), (uint32_t)iw, (uint32_t)ih,
                                              InGameOverlay::RendererPixelFormat::RGBA16F);
    } else {
        state.avatar_pixels = img->data;
        srgb_decode_pixels_if_needed(_renderer, settings->overlay_appearance.image_gamma,
            (uint8_t*)state.avatar_pixels.data(), (size_t)iw * ih);
        state.avatar_resource->AttachResource((void*)state.avatar_pixels.data(), (uint32_t)iw, (uint32_t)ih);
    }
    
    return state.avatar_resource->GetResourceId() != 0;
}

bool Steam_Overlay::try_load_local_avatar()
{
    if (!_renderer) return false;
    
    // Already loaded?
    if (local_avatar_resource && local_avatar_resource->GetResourceId() != 0) return true;
    
    // Create resource if not exist
    if (!local_avatar_resource) {
        local_avatar_resource = _renderer->CreateResource();
        if (!local_avatar_resource) return false;
    }
    
    // Try to get avatar handle
    if (local_avatar_handle == -1) {
        Steam_Friends *steamFriends = get_steam_client()->steam_friends;
        if (!steamFriends) return false;
        CSteamID local_id = settings->get_local_steam_id();
        local_avatar_handle = steamFriends->GetMediumFriendAvatar(local_id);
    }
    if (local_avatar_handle <= 0) return false;
    
    // Get image data
    Image_Data *img = settings->get_image(local_avatar_handle);
    if (!img || img->data.empty()) return false;
    
    int iw = static_cast<int>(img->width);
    int ih = static_cast<int>(img->height);
    if (iw <= 0 || ih <= 0 || img->data.size() < (size_t)iw * ih * 4) return false;
    
    // Store pixel data - AttachResource holds a raw pointer so buffer must outlive resource
    if (should_use_fp16()) {
        auto fp16 = transform_pixels_to_fp16((const uint8_t*)img->data.data(), iw, ih,
                                              effective_swapchain_cs(), s_sdr_white_scale,
                                              s_img_brightness, s_img_contrast, s_img_gamma_adj);
        local_avatar_pixels.assign(reinterpret_cast<const char*>(fp16.data()), fp16.size() * sizeof(uint16_t));
        local_avatar_resource->AttachResource((void*)local_avatar_pixels.data(), (uint32_t)iw, (uint32_t)ih,
                                              InGameOverlay::RendererPixelFormat::RGBA16F);
    } else {
        local_avatar_pixels = img->data;
        srgb_decode_pixels_if_needed(_renderer, settings->overlay_appearance.image_gamma,
            (uint8_t*)local_avatar_pixels.data(), (size_t)iw * ih);
        local_avatar_resource->AttachResource((void*)local_avatar_pixels.data(), (uint32_t)iw, (uint32_t)ih);
    }
    
    return local_avatar_resource->GetResourceId() != 0;
}

// Try to make this function as short as possible or it might affect game's fps.
void Steam_Overlay::overlay_render_proc()
{
    std::lock_guard lock(overlay_mutex);

    if (!Ready()) return;

    // Give the file-scope helper access to the current appearance settings
    // so effective_swapchain_cs() can resolve the user's Swapchain_Override.
    s_ov_app = &settings->overlay_appearance;

    // Refresh per-image colour adjustments from settings
    s_img_brightness = settings->overlay_appearance.image_brightness;
    s_img_contrast   = settings->overlay_appearance.image_contrast;
    s_img_gamma_adj  = settings->overlay_appearance.image_gamma_adjust;

    // Query the renderer's supported upload formats. Backends that cannot upload
    // RGBA16F (e.g. the Linux/macOS hooks) must never receive one: they would
    // reinterpret the 16-bit buffer as RGBA8. Callers fall back to RGBA8 instead,
    // which still gets the correct brightness / contrast / gamma via the
    // adjustment pre-pass in srgb_decode_pixels_if_needed().
    s_renderer_supports_fp16 = (_renderer != nullptr) &&
        _renderer->SupportsPixelFormat(InGameOverlay::RendererPixelFormat::RGBA16F);

    // Deferred SCE texture cleanup - do this BEFORE any ImGui rendering
    // to avoid deleting resources mid-frame (which crashes DX9)
    if (sce_textures_pending_free) {
        sce_textures_pending_free = false;
        sce_textures_free_all();
    }

    // Drain the deferred display-info refresh (set on Reset/Removing).
    // Done here — on the render thread but outside the hooked resize path — so
    // QueryDisplayConfig does not block ResizeBuffers / device-release calls.
    if (s_pending_sdr_refresh) {
        s_pending_sdr_refresh = false;
        refresh_sdr_white_scale();
    }

    // Lazy-init SDR white scale from display info on the first frame.
    // Placed here because refresh_sdr_white_scale() is defined after the hook-ready
    // callback where arm_swapchain_format_detect is called.
    if (!s_sdr_scale_queried)
        refresh_sdr_white_scale();

    // ── Texture cache invalidation ──────────────────────────────────────
    // If the swapchain colour space, SDR scale, or image adjustments changed
    // since we last baked the textures, invalidate everything so they are
    // re-decoded + re-uploaded with the new parameters on next access.
    {
        const SwapchainColorSpace cur_cs = effective_swapchain_cs();
        const bool cs_changed    = (cur_cs != s_cached_tex_cs);
        const bool scale_changed = (cur_cs != SCS_SDR_UNORM && cur_cs != SCS_UNKNOWN &&
                                    fabsf(s_sdr_white_scale - s_cached_tex_scale) > 0.01f);
        const bool adj_changed   = (fabsf(s_img_brightness - s_cached_tex_brightness) > 0.001f ||
                                    fabsf(s_img_contrast   - s_cached_tex_contrast)   > 0.001f ||
                                    fabsf(s_img_gamma_adj  - s_cached_tex_gamma_adj)  > 0.001f);
        if (cs_changed || scale_changed || adj_changed) {
            PRINT_DEBUG("Texture cache invalidation: cs %d→%d  scale %.2f→%.2f  adj %.2f/%.2f/%.2f→%.2f/%.2f/%.2f",
                (int)s_cached_tex_cs, (int)cur_cs, s_cached_tex_scale, s_sdr_white_scale,
                s_cached_tex_brightness, s_cached_tex_contrast, s_cached_tex_gamma_adj,
                s_img_brightness, s_img_contrast, s_img_gamma_adj);

            // Achievement icons: clear decoded pixel caches and detach GPU resources.
            // The RendererResource_t objects stay alive; try_load_ach_icon() will
            // re-decode and re-attach on next access (GetResourceId() == 0).
            for (auto &ach : achievements) {
                ach.icon_decoded_data.clear();
                ach.icon_gray_decoded_data.clear();
                if (ach.icon)      ach.icon->ClearAttachedResource();
                if (ach.icon_gray) ach.icon_gray->ClearAttachedResource();
            }
            last_loaded_ach_icon = 0;

            // Avatars: clear decoded pixels and detach GPU resources.
            for (auto &[frd, state] : friends) {
                state.avatar_pixels.clear();
                if (state.avatar_resource) state.avatar_resource->ClearAttachedResource();
            }
            local_avatar_pixels.clear();
            if (local_avatar_resource) local_avatar_resource->ClearAttachedResource();

            // SCE textures: free the whole cache so they are re-loaded lazily.
            sce_textures_free_all();

            // Snapshot the current parameters
            s_cached_tex_cs         = cur_cs;
            s_cached_tex_scale      = s_sdr_white_scale;
            s_cached_tex_brightness = s_img_brightness;
            s_cached_tex_contrast   = s_img_contrast;
            s_cached_tex_gamma_adj  = s_img_gamma_adj;
        }
    }

    // ── Periodic swapchain re-detection ─────────────────────────────────
    // While the overlay is visible, periodically re-arm the format detector
    // to catch mid-session HDR/resolution/format changes the game may make
    // without triggering a device Reset (e.g. borderless ↔ fullscreen, HDR toggle).
    if (show_overlay && _renderer &&
        settings->overlay_appearance.image_gamma == Overlay_Appearance::SrgbDecode::Auto)
    {
        s_swapchain_redetect_timer += ImGui::GetIO().DeltaTime;
        if (s_swapchain_redetect_timer >= SWAPCHAIN_REDETECT_INTERVAL_SEC) {
            s_swapchain_redetect_timer = 0.0f;
            arm_swapchain_format_detect(_renderer);
        }
    } else {
        s_swapchain_redetect_timer = 0.0f;
    }

    // NOTE: ImGui style colours are NOT patched for HDR — InGameOverlay handles
    // sRGB→swapchain conversion for ImGui rendering internally.  Only textures
    // need our explicit FP16 HDR transform (via transform_pixels_to_fp16).

    // Process achievement queue to show scheduled notifications
    process_achievement_queue();

    // -- Screenshot hotkey detection --
    if (_renderer && settings->enable_screenshot && !screenshot_keys.empty()) {
#ifdef __WINDOWS__
        // Only respond when the game window is focused
        bool screenshot_focused = true;
        {
            HWND fg = GetForegroundWindow();
            if (fg) {
                DWORD fg_pid = 0;
                GetWindowThreadProcessId(fg, &fg_pid);
                if (fg_pid != GetCurrentProcessId())
                    screenshot_focused = false;
            }
        }

        if (screenshot_focused) {
            bool all_pressed = true;
            for (auto k : screenshot_keys) {
                int vk = toggle_key_to_vk(k);
                if (!vk || !(GetAsyncKeyState(vk) & 0x8000)) {
                    all_pressed = false;
                    break;
                }
            }

            // Rising edge detection + cooldown (1 second).
            // `prev_initialized` ensures we don't fire a false trigger on the first call when the
            // user happens to be holding the hotkey when the overlay is first loaded.
            static bool prev_screenshot_keys = false;
            static bool prev_initialized = false;
            static std::chrono::steady_clock::time_point last_screenshot_time_local{};
            auto now_local = std::chrono::steady_clock::now();
            bool rising_edge = false;
            if (prev_initialized) {
                rising_edge = all_pressed && !prev_screenshot_keys;
            }
            prev_screenshot_keys = all_pressed;
            prev_initialized = true;

            if (rising_edge && (now_local - last_screenshot_time_local) > std::chrono::seconds(1)) {
                last_screenshot_time_local = now_local;
                PRINT_DEBUG("Screenshot hotkey triggered");
                _renderer->TakeScreenshot(InGameOverlay::ScreenshotType_t::BeforeOverlay);
            }
        }
#else
        // Non-Windows: use ToggleKey to ImGui mapping
        auto toggleKeyToImGui = [](InGameOverlay::ToggleKey key) -> ImGuiKey {
            switch (key) {
                case InGameOverlay::ToggleKey::SHIFT: return ImGuiKey_LeftShift;
                case InGameOverlay::ToggleKey::CTRL:  return ImGuiKey_LeftCtrl;
                case InGameOverlay::ToggleKey::ALT:   return ImGuiKey_LeftAlt;
                case InGameOverlay::ToggleKey::TAB:   return ImGuiKey_Tab;
                case InGameOverlay::ToggleKey::F1:    return ImGuiKey_F1;
                case InGameOverlay::ToggleKey::F2:    return ImGuiKey_F2;
                case InGameOverlay::ToggleKey::F3:    return ImGuiKey_F3;
                case InGameOverlay::ToggleKey::F4:    return ImGuiKey_F4;
                case InGameOverlay::ToggleKey::F5:    return ImGuiKey_F5;
                case InGameOverlay::ToggleKey::F6:    return ImGuiKey_F6;
                case InGameOverlay::ToggleKey::F7:    return ImGuiKey_F7;
                case InGameOverlay::ToggleKey::F8:    return ImGuiKey_F8;
                case InGameOverlay::ToggleKey::F9:    return ImGuiKey_F9;
                case InGameOverlay::ToggleKey::F10:   return ImGuiKey_F10;
                case InGameOverlay::ToggleKey::F11:   return ImGuiKey_F11;
                case InGameOverlay::ToggleKey::F12:   return ImGuiKey_F12;
                default: return ImGuiKey_None;
            }
        };

        bool all_pressed = true;
        for (auto k : screenshot_keys) {
            ImGuiKey ik = toggleKeyToImGui(k);
            if (ik == ImGuiKey_None || !ImGui::IsKeyDown(ik)) {
                all_pressed = false;
                break;
            }
        }

        // Rising edge detection + cooldown (1 second). Skip the first frame so we don't
        // trigger a false-positive if the user is already holding the hotkey.
        static bool prev_screenshot_keys = false;
        static bool prev_initialized = false;
        static std::chrono::steady_clock::time_point last_screenshot_time_local{};
        auto now_local = std::chrono::steady_clock::now();
        bool rising_edge = false;
        if (prev_initialized) {
            rising_edge = all_pressed && !prev_screenshot_keys;
        }
        prev_screenshot_keys = all_pressed;
        prev_initialized = true;

        if (rising_edge && (now_local - last_screenshot_time_local) > std::chrono::seconds(1)) {
            last_screenshot_time_local = now_local;
            PRINT_DEBUG("Screenshot hotkey triggered");
            _renderer->TakeScreenshot(InGameOverlay::ScreenshotType_t::BeforeOverlay);
        }
#endif
    }

    // Process any captured screenshots and save them to disk
    process_captured_screenshots();

    // ── ReShade addon takeover point ────────────────────────────────────
    // Everything above this line runs in BOTH modes and must not be skipped:
    //   * the screenshot hotkey / process_captured_screenshots() — the emu still
    //     owns the renderer hook while the addon is attached, and the addon
    //     cannot capture screenshots itself;
    //   * process_achievement_queue() — the only drain of the achievement
    //     notification queue, which the addon reads through the bridge.
    //
    // When the addon is actively connected it handles all rendering below.
    // We still keep the data structures alive so the bridge can read them.
    // If the addon stops calling (unloaded/disabled) the heartbeat goes stale
    // and the native overlay automatically resumes after the timeout.
    if (Bridge_IsConnected()) {
        // Keep the tabbed chat window available alongside the addon's UI
        if (Ready()) {
            build_chat_window();
        }
        return;
    }

    if (show_overlay) {
        render_main_window();
        render_gallery_window();
    }

    if (stats.show_any_stats()) {
        // Give the stats HUD the same swapchain colour transform as the main overlay
        stats.color_transform = nullptr;
        stats.render_stats(current_language);
    }

    // Stats settings window (rendered when overlay is open)
    if (show_overlay) {
        stats.render_stats_settings(current_language);
    }

    // Notifications rendered LAST so they always draw on top of everything
    // Pinned screenshot (always rendered when active, click-through when overlay closed)
    render_pinned_screenshot();

    if (notifications.size()) {
        ImGuiIO &io = ImGui::GetIO();
        build_notifications(io.DisplaySize.x, io.DisplaySize.y);
    }



    load_next_ach_icon();
}

uint32 Steam_Overlay::apply_global_style_color()
{
    uint32 style_color_stack = 0;
    if ((settings->overlay_appearance.background_r >= 0) &&
        (settings->overlay_appearance.background_g >= 0) &&
        (settings->overlay_appearance.background_b >= 0) &&
        (settings->overlay_appearance.background_a >= 0)) {
        ImVec4 colorSet = ImVec4(
            settings->overlay_appearance.background_r,
            settings->overlay_appearance.background_g,
            settings->overlay_appearance.background_b,
            settings->overlay_appearance.background_a
        );
        ImGui::PushStyleColor(ImGuiCol_WindowBg, colorSet);
        style_color_stack += 1;
    }

    if ((settings->overlay_appearance.element_r >= 0) &&
        (settings->overlay_appearance.element_g >= 0) &&
        (settings->overlay_appearance.element_b >= 0) &&
        (settings->overlay_appearance.element_a >= 0)) {
        ImVec4 colorSet = ImVec4(
            settings->overlay_appearance.element_r,
            settings->overlay_appearance.element_g,
            settings->overlay_appearance.element_b,
            settings->overlay_appearance.element_a
        );
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, colorSet);
        ImGui::PushStyleColor(ImGuiCol_Button, colorSet);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, colorSet);
        ImGui::PushStyleColor(ImGuiCol_ResizeGrip, colorSet);
        style_color_stack += 4;
    }

    if ((settings->overlay_appearance.element_hovered_r >= 0) &&
        (settings->overlay_appearance.element_hovered_g >= 0) &&
        (settings->overlay_appearance.element_hovered_b >= 0) &&
        (settings->overlay_appearance.element_hovered_a >= 0)) {
        ImVec4 colorSet = ImVec4(
            settings->overlay_appearance.element_hovered_r,
            settings->overlay_appearance.element_hovered_g,
            settings->overlay_appearance.element_hovered_b,
            settings->overlay_appearance.element_hovered_a
        );
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colorSet);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colorSet);
        ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, colorSet);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, colorSet);
        style_color_stack += 4;
    }

    if ((settings->overlay_appearance.element_active_r >= 0) &&
        (settings->overlay_appearance.element_active_g >= 0) &&
        (settings->overlay_appearance.element_active_b >= 0) &&
        (settings->overlay_appearance.element_active_a >= 0)) {
        ImVec4 colorSet = ImVec4(
            settings->overlay_appearance.element_active_r,
            settings->overlay_appearance.element_active_g,
            settings->overlay_appearance.element_active_b,
            settings->overlay_appearance.element_active_a
        );
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colorSet);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, colorSet);
        ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, colorSet);
        ImGui::PushStyleColor(ImGuiCol_Header, colorSet);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, colorSet);
        style_color_stack += 5;
    }

    return style_color_stack;
}

// Try to make this function as short as possible or it might affect game's fps.
void Steam_Overlay::render_main_window()
{
    char tmp[TRANSLATION_BUFFER_SIZE]{};
    snprintf(tmp, sizeof(tmp), translationRenderer[current_language], (_renderer == nullptr ? "Unknown" : _renderer->GetLibraryName()));
    std::string windowTitle{};
    // Note: don't translate this, project and author names are nouns, they must be kept intact for proper referral
    // think of it as translating "Protobuf - Google"
    windowTitle.append("Ingame Overlay project - Nemirtingas (").append(tmp).append(")");

    bool show = true;

    ImGuiIO &io = ImGui::GetIO();

    ImGui::PushFont(font_default, 0.0f);
    uint32 style_color_stack = apply_global_style_color();

    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize({ io.DisplaySize.x, io.DisplaySize.y });
    if (ImGui::Begin(windowTitle.c_str(), &show,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        if (show_user_info) {
            // Show local user avatar next to info
            const float avatar_size = 48.0f;
            bool has_local_avatar = try_load_local_avatar();
            if (has_local_avatar && local_avatar_resource && local_avatar_resource->GetResourceId() != 0) {
                ImGui::Image(local_avatar_resource->GetResourceId(), ImVec2(avatar_size, avatar_size));
                ImGui::SameLine();
            }
            ImGui::LabelText("##playinglabel", translationUserPlaying[current_language],
                settings->get_local_name(),
                settings->get_local_steam_id().ConvertToUint64(),
                settings->get_local_game_id().AppID());
            
            // Show local IP addresses (one per adapter)
            if (network) {
                Networking::AdapterInfo local_adapters[16];
                int local_adapter_count = network->getAdapters(local_adapters, 16);
                for (int ai = 0; ai < local_adapter_count; ++ai) {
                    if (local_adapters[ai].ip == 0) continue;
                    char ip_buf[24];
                    format_ip_address(local_adapters[ai].ip, ip_buf, sizeof(ip_buf));
                    ImGui::TextColored(TC(ImVec4(0.5f, 0.7f, 0.9f, 1.0f)), "%s: %s", local_adapters[ai].name, ip_buf);
                    ImGui::SameLine();
                    char btn_id[64];
                    snprintf(btn_id, sizeof(btn_id), "Copy##lip%d", ai);
                    if (ImGui::SmallButton(btn_id)) {
                        ImGui::SetClipboardText(ip_buf);
                    }
                }
            }

            // Show lobby/game status
            if (i_have_lobby) {
                Steam_Friends *steamFriends = get_steam_client()->steam_friends;
                std::string connect_str;
                if (steamFriends) {
                    connect_str = steamFriends->get_friend_rich_presence_silent(settings->get_local_steam_id(), "connect");
                }

                CSteamID lobby = settings->get_lobby();
                if (lobby.IsValid()) {
                    Steam_Matchmaking *matchmaking = get_steam_client()->steam_matchmaking;
                    if (matchmaking) {
                        int member_count = matchmaking->GetNumLobbyMembers(lobby);
                        int member_limit = matchmaking->GetLobbyMemberLimit(lobby);
                        bool is_owner = (matchmaking->GetLobbyOwner(lobby) == settings->get_local_steam_id());
                        ImGui::TextColored(TC(ImVec4(0.4f, 0.8f, 0.4f, 1.0f)), "In Lobby (%d/%d) %s", 
                            member_count, member_limit,
                            is_owner ? "[Owner]" : "");
                        ImGui::SameLine();
                        char lobby_id_str[32];
                        snprintf(lobby_id_str, sizeof(lobby_id_str), "%llu", lobby.ConvertToUint64());
                        if (ImGui::SmallButton("Copy Lobby ID")) {
                            ImGui::SetClipboardText(lobby_id_str);
                        }
                    }
                } else if (!connect_str.empty()) {
                    // Connect string only (no formal lobby)
                    ImGui::TextColored(TC(ImVec4(0.4f, 0.8f, 0.4f, 1.0f)), "Hosting Game");
                }

                if (!connect_str.empty()) {
                    // Get exe filename for the full launch command
                    std::string full_path = get_full_exe_path();
                    std::string::size_type pos = full_path.find_last_of("/\\");
                    std::string exe_name = (pos != std::string::npos) ? full_path.substr(pos + 1) : full_path;
                    
                    std::string launch_cmd = exe_name + " " + connect_str;
                    ImGui::TextColored(TC(ImVec4(0.7f, 0.7f, 0.7f, 1.0f)), "Launch: %s", launch_cmd.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Copy##launch")) {
                        ImGui::SetClipboardText(launch_cmd.c_str());
                    }
                }
            }

            // Show game server info (listen server in same process)
            {
                Steam_GameServer *gs = get_steam_client()->steam_gameserver;
                if (gs && gs->BLoggedOn()) {
                    const auto &sd = gs->get_server_data();
                    std::string sname = sd.server_name();
                    std::string mname = sd.map_name();
                    uint32 num = sd.num_players();
                    uint32 maxp = sd.max_player_count();

                    if (!sname.empty() || !mname.empty() || maxp > 0) {
                        // Build: "ServerName - MapName (players/max)"
                        std::string server_line = "Server: ";
                        if (!sname.empty()) {
                            server_line += sname;
                            if (!mname.empty()) server_line += " - " + mname;
                        } else if (!mname.empty()) {
                            server_line += mname;
                        }
                        if (maxp > 0) {
                            char counts[32];
                            snprintf(counts, sizeof(counts), " (%u/%u)", num, maxp);
                            server_line += counts;
                        }
                        ImGui::TextColored(TC(ImVec4(0.6f, 0.8f, 1.0f, 1.0f)), "%s", server_line.c_str());
                    }
                }

            if (settings->record_playtime && playtime_counter && settings->overlay_appearance.show_playtime_in_user_info) {
                uint64_t session_sec = playtime_counter->session_seconds();
                unsigned ss = static_cast<unsigned>(session_sec % 60);
                unsigned mm = static_cast<unsigned>((session_sec / 60) % 60);
                unsigned hh = static_cast<unsigned>(session_sec / 3600);

                uint64_t total_sec = playtime_counter->seconds();
                unsigned total_h = static_cast<unsigned>(total_sec / 3600);
                unsigned total_m = static_cast<unsigned>((total_sec % 3600) / 60);

                char total_buf[32]{};
                char session_buf[32]{};
                snprintf(total_buf, sizeof(total_buf), translationTotalTime[current_language], total_h, total_m);
                snprintf(session_buf, sizeof(session_buf), "%02u:%02u:%02u", hh, mm, ss);

                ImGui::LabelText("##playtime", translationTotalTimeText[current_language], total_buf, session_buf);
            }
        }

        ImGui::Spacing();

        if (settings->overlay_show_button_user_info) {
            ImGui::SameLine();
            // user clicked on "toggle user info"
            if (ImGui::Button(translationToggleUserInfo[current_language])) {
                show_user_info = !show_user_info;
            }
        }

        ImGui::SameLine();
        // user clicked on "friends" - opens friends window
        {
            bool has_unread = false;
            for (auto &[frd, state] : friends) {
                if (state.window_state & window_state_need_attention) {
                    has_unread = true;
                    break;
                }
            }
            if (has_unread)
                ImGui::PushStyleColor(ImGuiCol_Text, TC(ImVec4(1.0f, 0.8f, 0.2f, 1.0f)));
            if (ImGui::Button(translationFriends[current_language])) {
                show_friends = !show_friends;
            }
            if (has_unread)
                ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        // user clicked on "show achievements"
        if (ImGui::Button(translationShowAchievements[current_language])) {
            show_achievements = !show_achievements;
        }

        if (settings->overlay_show_button_copy_id) {
            ImGui::SameLine();
            // user clicked on "copy id" on themselves
            if (ImGui::Button(translationCopyId[current_language])) {
                auto friend_id_str = std::to_string(settings->get_local_steam_id().ConvertToUint64());
                ImGui::SetClipboardText(friend_id_str.c_str());
            }
        }

        if (settings->overlay_show_button_screenshots) {
            ImGui::SameLine();
            // user clicked on "Screenshots"
            if (ImGui::Button(translationScreenshots[current_language])) {
                show_screenshots_window = !show_screenshots_window;
            }
		}

        if (settings->overlay_show_button_history) {
            ImGui::SameLine();
            // user clicked on "notification history"
            if (ImGui::Button(translationHistory[current_language])) {
                show_notification_history = !show_notification_history;
            }
        }

        if (settings->overlay_show_button_settings) {
            ImGui::SameLine();
            // user clicked on "settings"
            if (ImGui::Button(translationSettings[current_language])) {
                show_settings = !show_settings;
            }
        }
        
        ImGui::Spacing();
        ImGui::Spacing();
        // user clicked on "FPS"
        ImGui::SameLine();
        // user clicked on "networks"
        if (ImGui::Button("Networks")) {
            show_networks = !show_networks;
        }

        // Lobby Chat button — only shown when in a lobby
        if (i_have_lobby) {
            ImGui::SameLine();
            if (ImGui::Button("Lobby Chat")) {
                show_lobby_chat = !show_lobby_chat;
            }
        }
        ImGui::Spacing();
        ImGui::Spacing();

        // --- Notification history panel ---
        if (show_notification_history) {
            if (ImGui::Button(translationClearAll[current_language])) {
                notification_history.clear();
                notification_history_cache.clear();
                notification_history_cache_dirty = false;
            }
            ImGui::Separator();
            if (notification_history.empty()) {
                // Explicit "%s": these strings are runtime data, and ImGui::* treats the
                // first argument as a printf format. None of the translations contain a
                // specifier today, but a translator adding one would turn this into a
                // read of a nonexistent vararg.
                ImGui::TextDisabled("%s", translationNoNotification[current_language]);
            } else {
                ImGui::BeginChild("##history_scroll", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 10), true);

                // Rebuild cache only when history actually changes
                if (notification_history_cache_dirty) {
                    notification_history_cache.clear();
                    notification_history_cache.reserve(notification_history.size());

                    for (auto it = notification_history.rbegin(); it != notification_history.rend(); ++it) {
                        // Format timestamp HH:MM:SS in local timezone
                        const time_t total_sec = std::chrono::duration_cast<std::chrono::seconds>(it->timestamp).count();
                        struct tm local_tm_buf{};
#ifdef _MSC_VER
                        localtime_s(&local_tm_buf, &total_sec);
#else
                        localtime_r(&total_sec, &local_tm_buf);
#endif
                        const auto hr = local_tm_buf.tm_hour;
                        const auto min = local_tm_buf.tm_min;
                        const auto sec = local_tm_buf.tm_sec;

                        // Type label
                        const char *type_label = "?";
                        switch ((notification_type)it->type) {
                            case notification_type::message: type_label = translationHistoryChat[current_language]; break;
                            case notification_type::invite: type_label = translationHistoryInvite[current_language]; break;
                            case notification_type::achievement: type_label = translationHistoryAchievement[current_language]; break;
                            case notification_type::achievement_progress: type_label = translationHistoryProgress[current_language]; break;
                            case notification_type::auto_accept_invite: type_label = translationHistoryAutoInvite[current_language]; break;
                            case notification_type::screenshot: type_label = translationHistoryScreenshot[current_language]; break;
                        }

                        // For achievements the message contains "title\ndescription"
                        // Replace newline with inline separator for compact display
                        std::string display_msg = it->message;
                        if (it->type == static_cast<uint8>(notification_type::achievement) ||
                            it->type == static_cast<uint8>(notification_type::achievement_progress)) {
                            size_t pos = display_msg.find('\n');
                            if (pos != std::string::npos) {
                                display_msg.replace(pos, 1, " — ");
                            }
                        }

                        std::string line = (std::ostringstream{}
                            << "[" << std::setw(2) << std::setfill('0') << hr << ":"
                            << std::setw(2) << std::setfill('0') << min << ":"
                            << std::setw(2) << std::setfill('0') << sec << "] "
                            << type_label << "  "
                            << display_msg).str();

                        notification_history_cache.push_back(std::move(line));
                    }
                    notification_history_cache_dirty = false;
                }

                // Render from cache
                for (const auto &line : notification_history_cache) {
                    ImGui::TextWrapped("%s", line.c_str());
                    ImGui::Separator();
                }
                ImGui::EndChild();
            }
        }

        ImGui::LabelText("##label", "%s", translationFriends[current_language]);

        // SCE buttons — only shown when SCE catalog data is present
        {
            Steam_User_Stats *user_stats = get_steam_client()->steam_user_stats;
            if (user_stats->sce_data_populated && !user_stats->sce_game_data.series.empty()) {
                ImGui::SameLine();
                bool downloading = user_stats->sce_assets_downloading.load();
                if (!downloading) {
                    if (ImGui::Button("Download SCE Assets")) {
                        user_stats->RequestSceAssetDownload();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(show_sce_browser ? "[SCE Assets]" : "Browse SCE Assets"))
                        show_sce_browser = !show_sce_browser;
                } else {
                    ImGui::BeginDisabled();
                    ImGui::Button("Downloading SCE Assets...");
                    ImGui::EndDisabled();
                }

                // ---- Download progress: fixed 25%-wide floating window ----
                if (downloading) {
                    uint32_t grand_dl    = user_stats->sce_assets_downloaded.load();
                    uint32_t grand_skip  = user_stats->sce_assets_skipped.load();
                    uint32_t grand_total = user_stats->sce_assets_total.load();
                    uint32_t grand_done  = grand_dl + grand_skip;

                    float win_w = io.DisplaySize.x * 0.25f;
                    ImGui::SetNextWindowSize(ImVec2(win_w, 0.0f), ImGuiCond_Always);
                    ImGui::SetNextWindowPos(
                        ImVec2(io.DisplaySize.x * 0.5f - win_w * 0.5f, io.DisplaySize.y * 0.08f),
                        ImGuiCond_Always);
                    ImGui::SetNextWindowBgAlpha(0.92f);
                    ImGui::Begin("SCE Download Progress##sce_dl", nullptr,
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_AlwaysAutoResize);

                    if (grand_total > 0) {
                        char total_lbl[128]{};
                        snprintf(total_lbl, sizeof(total_lbl),
                            "Total: %u downloaded, %u cached of %u",
                            grand_dl, grand_skip, grand_total);
                        float frac = (float)grand_done / (float)grand_total;
                        ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), total_lbl);
                    } else {
                        ImGui::TextUnformatted("Collecting asset URLs...");
                    }

                    ImGui::Spacing();
                    for (int i = 0; i < Steam_User_Stats::SCE_NUM_TYPES; ++i) {
                        uint32_t tot = user_stats->sce_type_progress[i].total.load();
                        if (tot == 0) continue;
                        uint32_t cur = user_stats->sce_type_progress[i].current.load();
                        uint32_t dl  = user_stats->sce_type_progress[i].downloaded.load();
                        char lbl[128]{};
                        snprintf(lbl, sizeof(lbl), "%s: %u of %u (%u new)",
                            Steam_User_Stats::SCE_TYPE_LABELS[i], cur, tot, dl);
                        ImGui::ProgressBar((float)cur / (float)tot, ImVec2(-1.0f, 0.0f), lbl);
                    }

                    ImGui::End();
                }
            }

            // flush pending completion notification
            if (sce_asset_progress.pending_notification) {
                sce_asset_progress.pending_notification = false;
                char msg[256]{};
                snprintf(msg, sizeof(msg),
                    "SCE assets ready: %u downloaded, %u already cached (%u total)",
                    sce_asset_progress.downloaded,
                    sce_asset_progress.skipped,
                    sce_asset_progress.total);
                submit_notification(notification_type::message, msg);
                allow_renderer_frame_processing(false);
            }
        }
        
        ImGui::Spacing();
        ImGui::Spacing();
        // Stats settings button (replaces individual FPS/Frametime/Playtime checkboxes)
        ImGui::SameLine();
        if (ImGui::Button("Stats Settings")) {
            stats.show_stats_settings = !stats.show_stats_settings;
        }
        ImGui::SameLine();
        // Quick status indicators
        {
            char indicator[64] = {};
            bool any = false;
            if (stats.show_fps) { strcat(indicator, "FPS"); any = true; }
            if (stats.show_frametime) { if (any) strcat(indicator, ", "); strcat(indicator, "FT"); any = true; }
            if (stats.show_playtime) { if (any) strcat(indicator, ", "); strcat(indicator, "PT"); any = true; }
            if (any) {
                char wrapped[72];
                snprintf(wrapped, sizeof(wrapped), "(%s)", indicator);
                ImGui::TextDisabled("%s", wrapped);
            }
        }

        // --- Rendering info panel -------------------------------------------
        ImGui::Spacing();
        ImGui::Separator();
        {
            using RHT = InGameOverlay::RendererHookType_t;

            // -- API / renderer lib --
            const char* api_name = "Unknown";
            if (_renderer) {
                switch (_renderer->GetRendererHookType()) {
                    case RHT::DirectX9:  api_name = "DirectX 9";  break;
                    case RHT::DirectX10: api_name = "DirectX 10"; break;
                    case RHT::DirectX11: api_name = "DirectX 11"; break;
                    case RHT::DirectX12: api_name = "DirectX 12"; break;
                    case RHT::OpenGL:    api_name = "OpenGL";     break;
                    case RHT::Vulkan:    api_name = "Vulkan";     break;
                    case RHT::Metal:     api_name = "Metal";      break;
                    default: break;
                }
            }
            ImGui::TextDisabled("API        : %s  (%s)",
                api_name, _renderer ? _renderer->GetLibraryName() : "None");

            ImGui::TextDisabled("Emu build  : " EMU_BUILD_STRING);
            ImGui::TextDisabled("Build date : " EMU_BUILD_DATE_STRING);

            // -- Game swapchain --
            bool hdr_api = false;
            if (_renderer) {
                auto rt = _renderer->GetRendererHookType();
                hdr_api = (rt == RHT::DirectX10 || rt == RHT::DirectX11 ||
                           rt == RHT::DirectX12 || rt == RHT::Vulkan || rt == RHT::Metal);
            }
            if (hdr_api) {
                ImGui::TextDisabled("Swapchain  : %s", s_swapchain_fmt_str);
                ImGui::TextDisabled("           : %s", s_swapchain_type_str);
            } else {
                ImGui::TextDisabled("Swapchain  : N/A  (DX9/OpenGL — linear framebuffer, no HDR path)");
            }

            // -- sRGB correction --
            using SD = Overlay_Appearance::SrgbDecode;
            const char* corr_str;
            const char* corr_reason;
            if (settings->overlay_appearance.image_gamma == SD::On) {
                corr_str    = "ON  (forced)";
                corr_reason = "Image_Gamma=on in config";
            } else if (settings->overlay_appearance.image_gamma == SD::Off) {
                corr_str    = "OFF (forced)";
                corr_reason = "Image_Gamma=off in config";
            } else if (!hdr_api) {
                corr_str    = "OFF";
                corr_reason = "DX9/OpenGL — legacy, no colour-space transform";
            } else {
                const SwapchainColorSpace ecs = effective_swapchain_cs();
                switch (ecs) {
                    case SCS_LINEAR_HDR:
                        corr_str    = "ON ";
                        corr_reason = "linear HDR (FP16/FP32/UNORM16) — sRGB->linear + SDR-white scale";
                        break;
                    case SCS_HDR10_PQ:
                        corr_str    = "ON ";
                        corr_reason = "HDR10 PQ — sRGB->linear->PQ (ST.2084) encode";
                        break;
                    case SCS_SDR_SRGB_RTV:
                        corr_str    = "ON ";
                        corr_reason = "_SRGB back-buffer — sRGB->linear (hw re-encodes)";
                        break;
                    case SCS_SDR_UNORM:
                        corr_str    = "OFF";
                        corr_reason = "SDR UNORM — bytes pass through unchanged";
                        break;
                    default:
                        corr_str    = "OFF";
                        corr_reason = "awaiting swap chain format detection";
                        break;
                }
            }
            ImGui::TextDisabled("sRGB corr. : %s  — %s", corr_str, corr_reason);
            {
                int so = static_cast<int>(settings->overlay_appearance.swapchain_override);
                if (so > 0) {
                    static const char* ov_names[] = { "auto", "linear_hdr", "hdr10_pq", "srgb_rtv", "sdr" };
                    const char* ov_name = (so >= 1 && so <= 4) ? ov_names[so] : "?";
                    ImGui::TextDisabled("           : Swapchain_Override=%s (user config)", ov_name);
                }
            }

            // -- Per-display info (queried once per launch; also updates s_sdr_white_scale) --
            static bool displays_queried = false;
            static std::vector<DisplayHdrDetail_t> displays;
            if (!displays_queried) {
                displays         = refresh_sdr_white_scale();
                displays_queried = true;
            }
            if (displays.empty()) {
                ImGui::TextDisabled("Display    : N/A");
            } else {
                for (int di = 0; di < (int)displays.size() && di < 4; ++di) {
                    const auto& d = displays[di];
                    const char* hdr_st;
                    if      (!d.hdr_supported)  hdr_st = "SDR only";
                    else if (d.force_disabled)  hdr_st = "HDR suppressed";
                    else if (d.wide_color && !d.hdr_enabled) hdr_st = "WCG (no HDR)";
                    else if (d.hdr_enabled)     hdr_st = "HDR ON";
                    else                        hdr_st = "HDR OFF (supported)";
                    char bpc_buf[16] = "?";
                    if (d.bpc > 0) snprintf(bpc_buf, sizeof(bpc_buf), "%d", d.bpc);
                    char white_buf[24] = "?";
                    if (d.sdr_white_nits >= 0)
                        snprintf(white_buf, sizeof(white_buf), "%d nits", d.sdr_white_nits);
                    ImGui::TextDisabled("Display %d  : %s  |  %s  |  %sbpc  |  SDR white: %s",
                        di+1, d.name, hdr_st, bpc_buf, white_buf);
                    ImGui::TextDisabled("           : gamut: %s  |  TF: %s  |  range: %s  |  enc: %s",
                        d.gamut, d.transfer, d.range, d.encoding);
                }
            }

            if (ImGui::SmallButton("Refresh##hdr_info")) {
                displays = refresh_sdr_white_scale();
                displays_queried = true;
                if (_renderer)
                    arm_swapchain_format_detect(_renderer);
            }
            { const auto ecs = effective_swapchain_cs();
            if (ecs == SCS_LINEAR_HDR || ecs == SCS_HDR10_PQ)
                ImGui::TextDisabled("HDR scale  : %.2fx  (SDR white = %d nits)",
                    s_sdr_white_scale, (int)(s_sdr_white_scale * 80.f + 0.5f));
            }

            // -- Image adjustments (live sliders) --
            ImGui::Spacing();
            ImGui::TextDisabled("Image Adjustments:");
            {
                static bool s_adj_slider_active = false;
                bool any_active = false;
                bool released = false;

                float &br = settings->overlay_appearance.image_brightness;
                float &ct = settings->overlay_appearance.image_contrast;
                float &ga = settings->overlay_appearance.image_gamma_adjust;

                ImGui::SliderFloat("Brightness##img", &br, 0.5f, 2.0f, "%.2f");
                if (ImGui::IsItemActive()) any_active = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) released = true;

                ImGui::SliderFloat("Contrast##img",   &ct, 0.5f, 2.0f, "%.2f");
                if (ImGui::IsItemActive()) any_active = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) released = true;

                ImGui::SliderFloat("Gamma##img",      &ga, 0.5f, 2.0f, "%.2f");
                if (ImGui::IsItemActive()) any_active = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) released = true;

                if (any_active) s_adj_slider_active = true;
                if (released)   s_adj_slider_active = false;

                if (ImGui::SmallButton("Reset##img_adj")) {
                    br = 1.0f; ct = 1.0f; ga = 1.0f;
                    released = true;
                }

                // While a slider is being dragged, suppress the per-frame cache
                // invalidation by keeping the cached values in sync.  On release
                // (or Reset), force a mismatch so the check triggers exactly once.
                if (s_adj_slider_active) {
                    s_cached_tex_brightness = s_img_brightness;
                    s_cached_tex_contrast   = s_img_contrast;
                    s_cached_tex_gamma_adj  = s_img_gamma_adj;
                }
                if (released) {
                    s_cached_tex_brightness = -999.0f; // sentinel → forces adj_changed
                }
            }
        }
        ImGui::Separator();
        // -------------------------------------------------------------------

        // Friends window (separate from main overlay)
        if (show_friends) {
            const float min_w = io.DisplaySize.x * 0.25f;
            ImGui::SetNextWindowSizeConstraints(ImVec2(min_w, ImGui::GetFontSize() * 16), ImVec2(8192, 8192));
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin(translationFriends[current_language], &show_friends)) {

                // ---- Local user header: 64px avatar + 3 info lines ----
                {
                    const float avatar_size = 64.0f;
                    bool has_local_avatar = try_load_local_avatar();
                    if (has_local_avatar && local_avatar_resource && local_avatar_resource->GetResourceId() != 0) {
                        ImGui::Image(local_avatar_resource->GetResourceId(), ImVec2(avatar_size, avatar_size));
                    } else {
                        ImVec2 p = ImGui::GetCursorScreenPos();
                        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + avatar_size, p.y + avatar_size), TC32(IM_COL32(60, 60, 80, 255)));
                        ImGui::Dummy(ImVec2(avatar_size, avatar_size));
                    }
                    ImGui::SameLine();

                    // 3 lines to the right of avatar
                    ImVec2 text_start = ImGui::GetCursorPos();
                    uint32 local_appid = settings->get_local_game_id().AppID();

                    auto resolve_app_name = [](uint32 appid) -> std::string {
                        auto it = steam_preowned_app_ids.find(appid);
                        if (it != steam_preowned_app_ids.end()) return it->second;
                        return std::to_string(appid);
                    };

                    // Line 1: Username (ID: steamid)
                    ImGui::SetCursorPos(text_start);
                    ImGui::TextColored(TC(ImVec4(0.9f, 0.9f, 0.2f, 1.0f)), "%s", settings->get_local_name());
                    ImGui::SameLine();
                    ImGui::TextColored(TC(ImVec4(0.6f, 0.6f, 0.6f, 1.0f)), "(ID: %llu)",
                        settings->get_local_steam_id().ConvertToUint64());

                    // Line 2: Playing AppName (AppID XXXX)
                    ImGui::SetCursorPosX(text_start.x);
                    std::string app_name = resolve_app_name(local_appid);
                    ImGui::TextColored(TC(ImVec4(0.5f, 0.8f, 0.5f, 1.0f)), "Playing %s (AppID %u)", app_name.c_str(), local_appid);

                    // Line 3: In Lobby / In Server / In Game
                    ImGui::SetCursorPosX(text_start.x);
                    CSteamID lobby = settings->get_lobby();
                    bool in_lobby = lobby.IsValid();
                    Steam_GameServer *gs = get_steam_client()->steam_gameserver;
                    bool in_server = gs && gs->BLoggedOn();

                    if (in_lobby) {
                        Steam_Matchmaking *matchmaking = get_steam_client()->steam_matchmaking;
                        if (matchmaking) {
                            int member_count = matchmaking->GetNumLobbyMembers(lobby);
                            int member_limit = matchmaking->GetLobbyMemberLimit(lobby);
                            CSteamID owner = matchmaking->GetLobbyOwner(lobby);
                            std::string owner_name;
                            for (auto &[frd, st] : friends) {
                                if (frd.id() == owner.ConvertToUint64()) {
                                    owner_name = frd.name();
                                    break;
                                }
                            }
                            if (owner_name.empty()) {
                                if (owner == settings->get_local_steam_id())
                                    owner_name = settings->get_local_name();
                                else
                                    owner_name = std::to_string(owner.ConvertToUint64());
                            }
                            bool local_is_owner = (owner == settings->get_local_steam_id());
                            ImGui::TextColored(TC(ImVec4(0.4f, 0.8f, 0.4f, 1.0f)), "%s - %llu (%d/%d - %s)",
                                local_is_owner ? "Has Lobby" : "In Lobby",
                                lobby.ConvertToUint64(), member_count, member_limit, owner_name.c_str());
                        }
                    } else if (in_server) {
                        const auto &sd = gs->get_server_data();
                        std::string sname = sd.server_name();
                        if (!sname.empty())
                            ImGui::TextColored(TC(ImVec4(0.6f, 0.8f, 1.0f, 1.0f)), "In Server - %s", sname.c_str());
                        else
                            ImGui::TextColored(TC(ImVec4(0.6f, 0.8f, 1.0f, 1.0f)), "In Server");
                    } else {
                        ImGui::TextColored(TC(ImVec4(0.5f, 0.7f, 0.5f, 1.0f)), "In Game");
                    }
                }

                // ---- Dynamic action buttons ----
                {
                    CSteamID lobby = settings->get_lobby();
                    bool is_lobby_owner = false;
                    if (lobby.IsValid()) {
                        Steam_Matchmaking *mm_chk = get_steam_client()->steam_matchmaking;
                        if (mm_chk) is_lobby_owner = (mm_chk->GetLobbyOwner(lobby) == settings->get_local_steam_id());
                    }
                    if (i_have_lobby && is_lobby_owner && !friends.empty()) {
                        std::string inviteAll(translationInviteAll[current_language]);
                        inviteAll.append("##PopupInviteAllFriends");
                        if (ImGui::Button(inviteAll.c_str())) {
                            invite_all_friends_clicked = true;
                        }
                        ImGui::SameLine();
                    }
                    if (lobby.IsValid()) {
                        if (ImGui::Button("Copy Lobby ID##fl")) {
                            ImGui::SetClipboardText(std::to_string(lobby.ConvertToUint64()).c_str());
                        }
                        ImGui::SameLine();
                        // "Remove All from Lobby" — owner only
                        Steam_Matchmaking *mm_btns = get_steam_client()->steam_matchmaking;
                        if (mm_btns && mm_btns->GetLobbyOwner(lobby) == settings->get_local_steam_id()) {
                            if (mm_btns->GetNumLobbyMembers(lobby) > 1) {
                                if (ImGui::Button("Remove All from Lobby##fl")) {
                                    mm_btns->KickAllLobbyMembers(lobby.ConvertToUint64());
                                }
                                ImGui::SameLine();
                            }
                        } else {
                            if (ImGui::Button("Leave Lobby##fl")) {
                                mm_btns->LeaveLobby(lobby);
                            }
                            ImGui::SameLine();
                        }
                    }
                    if (ImGui::Button(translationCopyId[current_language])) {
                        ImGui::SetClipboardText(std::to_string(settings->get_local_steam_id().ConvertToUint64()).c_str());
                    }
                }
                ImGui::Separator();

                if (!friends.empty()) {

                    // ---- Partition friends into In Game (same app) / Online (different app) ----
                    struct FriendEntry {
                        const Friend *frd;
                        friend_window_state *state;
                    };
                    std::vector<FriendEntry> in_game_friends, online_friends;
                    uint32 local_appid = settings->get_local_game_id().AppID();
                    for (auto &[frd, state] : friends) {
                        if (frd.appid() == local_appid)
                            in_game_friends.push_back({&frd, &state});
                        else
                            online_friends.push_back({&frd, &state});
                    }

                    // ---- App name resolver ----
                    auto resolve_app_name = [](uint32 appid) -> std::string {
                        auto it = steam_preowned_app_ids.find(appid);
                        if (it != steam_preowned_app_ids.end()) return it->second;
                        return std::to_string(appid);
                    };

                    // ---- Per-friend renderer (3-line: avatar + name/id + game + lobby) ----
                    auto render_friend_row = [&](const Friend &frd, friend_window_state &state) {
                        ImGui::PushID(state.id - base_friend_window_id + base_friend_item_id);

                        // 48px avatar to fit 3 text lines
                        const float avatar_size = 48.0f;
                        float line_h = ImGui::GetTextLineHeight();
                        float row_height = (std::max)(avatar_size, line_h * 3.0f);
                        ImVec2 cursor_before = ImGui::GetCursorPos();
                        ImGui::Selectable("##friend_row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, row_height));
                        // Capture right-click on the full selectable row for context menu
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                            ImGui::OpenPopup("##ctx_friend");
                        ImGui::SetCursorPos(cursor_before);

                        // Avatar
                        bool has_avatar = try_load_avatar(state, frd.id());
                        if (has_avatar && state.avatar_resource && state.avatar_resource->GetResourceId() != 0) {
                            ImGui::Image(state.avatar_resource->GetResourceId(), ImVec2(avatar_size, avatar_size));
                        } else {
                            ImVec2 p = ImGui::GetCursorScreenPos();
                            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + avatar_size, p.y + avatar_size), TC32(IM_COL32(60, 60, 80, 255)));
                            ImGui::Dummy(ImVec2(avatar_size, avatar_size));
                        }
                        ImGui::SameLine();

                        ImVec2 text_start = ImGui::GetCursorPos();

                        // Line 1: FriendName (ID: steamid)
                        ImGui::SetCursorPos(text_start);
                        bool needs_attn = (state.window_state & window_state_need_attention);
                        if (needs_attn)
                            ImGui::TextColored(TC(ImVec4(1.0f, 0.8f, 0.2f, 1.0f)), "%s", frd.name().c_str());
                        else
                            ImGui::TextColored(TC(ImVec4(0.9f, 0.9f, 0.2f, 1.0f)), "%s", frd.name().c_str());
                        ImGui::SameLine();
                        ImGui::TextColored(TC(ImVec4(0.6f, 0.6f, 0.6f, 1.0f)), "(ID: %llu)", (unsigned long long)frd.id());
                        // Show detected IPs if available
                        if (network) {
                            uint32 frd_ips[16];
                            int frd_ip_count = network->getIPs(CSteamID((uint64)frd.id()), frd_ips, 16);
                            for (int ipi = 0; ipi < frd_ip_count; ++ipi) {
                                if (frd_ips[ipi] == 0) continue;
                                char ip_buf[24];
                                format_ip_address(frd_ips[ipi], ip_buf, sizeof(ip_buf));
                                ImGui::SameLine();
                                ImGui::TextColored(TC(ImVec4(0.5f, 0.7f, 0.9f, 1.0f)), "[%s]", ip_buf);
                            }
                        }

                        // Line 2: Playing AppName (AppID XXXX)
                        ImGui::SetCursorPosX(text_start.x);
                        if (frd.appid() != 0) {
                            std::string game = resolve_app_name(frd.appid());
                            ImGui::TextColored(TC(ImVec4(0.5f, 0.8f, 0.5f, 1.0f)), "Playing %s (AppID %u)", game.c_str(), frd.appid());
                        } else {
                            ImGui::TextColored(TC(ImVec4(0.5f, 0.5f, 0.5f, 1.0f)), "Online");
                        }

                        // Line 3: Lobby info (if friend has a lobby)
                        if (frd.lobby_id() != 0) {
                            ImGui::SetCursorPosX(text_start.x);
                            int frd_mc = 0, frd_ml = 0;
                            std::string frd_owner_name;

                            // Try local matchmaking data first (available for same-app lobbies)
                            Steam_Matchmaking *mm_frd = get_steam_client()->steam_matchmaking;
                            if (mm_frd) {
                                CSteamID frd_lobby((uint64)frd.lobby_id());
                                frd_mc = mm_frd->GetNumLobbyMembers(frd_lobby);
                                frd_ml = mm_frd->GetLobbyMemberLimit(frd_lobby);
                                CSteamID frd_owner = mm_frd->GetLobbyOwner(frd_lobby);
                                if (frd_owner.IsValid()) {
                                    for (auto &[f2, s2] : friends) {
                                        if (f2.id() == frd_owner.ConvertToUint64()) { frd_owner_name = f2.name(); break; }
                                    }
                                    if (frd_owner_name.empty()) {
                                        if (frd_owner == settings->get_local_steam_id())
                                            frd_owner_name = settings->get_local_name();
                                        else if (frd_owner.ConvertToUint64() == frd.id())
                                            frd_owner_name = frd.name();
                                    }
                                }
                            }

                            // Fall back to proto fields for cross-app lobbies
                            if (frd_mc == 0 && frd.lobby_member_count() > 0)
                                frd_mc = frd.lobby_member_count();
                            if (frd_ml == 0 && frd.lobby_member_limit() > 0)
                                frd_ml = frd.lobby_member_limit();
                            if (frd_owner_name.empty() && frd.lobby_owner_name().size() > 0)
                                frd_owner_name = frd.lobby_owner_name();

                            // Check if this friend is the lobby owner
                            bool friend_is_owner = (!frd_owner_name.empty() && frd_owner_name == frd.name());

                            if (!frd_owner_name.empty())
                                ImGui::TextColored(TC(ImVec4(0.4f, 0.8f, 0.4f, 1.0f)), "%s - %llu (%d/%d - %s)",
                                    friend_is_owner ? "Has Lobby" : "In Lobby",
                                    (unsigned long long)frd.lobby_id(), frd_mc, frd_ml, frd_owner_name.c_str());
                            else
                                ImGui::TextColored(TC(ImVec4(0.4f, 0.8f, 0.4f, 1.0f)), "In Lobby - %llu (%d/%d)",
                                    (unsigned long long)frd.lobby_id(), frd_mc, frd_ml);
                        }

                        // Right-click context menu (opened from selectable above)
                        if (ImGui::BeginPopup("##ctx_friend")) {
                            if (ImGui::MenuItem(translationChat[current_language])) {
                                state.window_state |= window_state_show;
                                show_chat = true;
                            }
                            bool same_app = (settings->get_local_game_id().AppID() == frd.appid());
                            bool is_my_lobby_owner = false;
                            bool friend_in_my_lobby = false;
                            {
                                CSteamID my_lob = settings->get_lobby();
                                if (my_lob.IsValid()) {
                                    Steam_Matchmaking *mm_ctx = get_steam_client()->steam_matchmaking;
                                    if (mm_ctx) {
                                        is_my_lobby_owner = (mm_ctx->GetLobbyOwner(my_lob) == settings->get_local_steam_id());
                                        int mc = mm_ctx->GetNumLobbyMembers(my_lob);
                                        for (int mi = 0; mi < mc; ++mi) {
                                            if (mm_ctx->GetLobbyMemberByIndex(my_lob, mi).ConvertToUint64() == frd.id()) {
                                                friend_in_my_lobby = true;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                            if (same_app && i_have_lobby && is_my_lobby_owner && !friend_in_my_lobby) {
                                if (ImGui::MenuItem(translationInvite[current_language])) {
                                    state.window_state |= window_state_invite;
                                    has_friend_action.push(frd);
                                }
                            }
                            if (same_app && state.joinable && frd.lobby_id() != 0 && !friend_in_my_lobby) {
                                if (ImGui::MenuItem(translationJoin[current_language])) {
                                    state.window_state |= window_state_join;
                                    has_friend_action.push(frd);
                                }
                            }
                            // Show Accept Invite if this friend sent us a pending invite we haven't accepted yet
                            if (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
                                if (ImGui::MenuItem("Accept Invite")) {
                                    state.window_state |= window_state_join;
                                    has_friend_action.push(frd);
                                }
                            }
                            if (ImGui::MenuItem(translationCopyId[current_language])) {
                                ImGui::SetClipboardText(std::to_string(frd.id()).c_str());
                            }
                            if (network) {
                                uint32 frd_ips[16];
                                int frd_ip_count = network->getIPs(CSteamID((uint64)frd.id()), frd_ips, 16);
                                if (frd_ip_count == 1 && frd_ips[0] != 0) {
                                    char ip_buf[24];
                                    format_ip_address(frd_ips[0], ip_buf, sizeof(ip_buf));
                                    if (ImGui::MenuItem("Copy IP")) {
                                        ImGui::SetClipboardText(ip_buf);
                                    }
                                } else if (frd_ip_count > 1) {
                                    if (ImGui::BeginMenu("Copy IP")) {
                                        for (int ipi = 0; ipi < frd_ip_count; ++ipi) {
                                            if (frd_ips[ipi] == 0) continue;
                                            char ip_buf[24];
                                            format_ip_address(frd_ips[ipi], ip_buf, sizeof(ip_buf));
                                            if (ImGui::MenuItem(ip_buf)) {
                                                ImGui::SetClipboardText(ip_buf);
                                            }
                                        }
                                        ImGui::EndMenu();
                                    }
                                }
                            }
                            if (frd.lobby_id() != 0) {
                                if (ImGui::MenuItem("Copy Lobby ID")) {
                                    ImGui::SetClipboardText(std::to_string(frd.lobby_id()).c_str());
                                }
                            }
                            // Kick from lobby (only if friend is actually in our lobby member list and we're the owner)
                            {
                                CSteamID my_lobby = settings->get_lobby();
                                if (my_lobby.IsValid()) {
                                    Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
                                    CSteamID owner = mm->GetLobbyOwner(my_lobby);
                                    if (owner == settings->get_local_steam_id()) {
                                        // Check actual lobby member list
                                        bool friend_in_my_lobby = false;
                                        int member_count = mm->GetNumLobbyMembers(my_lobby);
                                        for (int mi = 0; mi < member_count; ++mi) {
                                            if (mm->GetLobbyMemberByIndex(my_lobby, mi).ConvertToUint64() == frd.id()) {
                                                friend_in_my_lobby = true;
                                                break;
                                            }
                                        }
                                        if (friend_in_my_lobby) {
                                            ImGui::Separator();
                                            if (ImGui::MenuItem("Kick from Lobby")) {
                                                mm->KickLobbyMember(my_lobby.ConvertToUint64(), frd.id());
                                            }
                                        }
                                    }
                                }
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::PopID();
                    };

                    // ---- Render grouped friend list ----
                    ImGui::BeginChild("##friends_child", ImVec2(0, 0), true);

                    // In Game section
                    if (!in_game_friends.empty()) {
                        char hdr[64];
                        snprintf(hdr, sizeof(hdr), "In Game (%d)##frd_ingame", (int)in_game_friends.size());
                        ImGui::PushStyleColor(ImGuiCol_Header, TC(ImVec4(0.15f, 0.35f, 0.15f, 0.80f)));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TC(ImVec4(0.20f, 0.45f, 0.20f, 0.90f)));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive, TC(ImVec4(0.25f, 0.55f, 0.25f, 1.00f)));
                        bool open = ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen);
                        ImGui::PopStyleColor(3);
                        if (open) {
                            for (auto &e : in_game_friends) render_friend_row(*e.frd, *e.state);
                        }
                    }

                    // Online section
                    if (!online_friends.empty()) {
                        char hdr[64];
                        snprintf(hdr, sizeof(hdr), "Online (%d)##frd_online", (int)online_friends.size());
                        ImGui::PushStyleColor(ImGuiCol_Header, TC(ImVec4(0.20f, 0.30f, 0.45f, 0.80f)));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TC(ImVec4(0.25f, 0.38f, 0.55f, 0.90f)));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive, TC(ImVec4(0.30f, 0.45f, 0.65f, 1.00f)));
                        bool open = ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen);
                        ImGui::PopStyleColor(3);
                        if (open) {
                            for (auto &e : online_friends) render_friend_row(*e.frd, *e.state);
                        }
                    }

                    ImGui::EndChild();
                }
            }
            ImGui::End();
        }

        // Chat window (tabbed, separate from friends list)
        build_chat_window();

        // user clicked on "show achievements" button
        if (show_achievements && achievements.size()) {
            const float noti_w = io.DisplaySize.x * Notification::width_percent;
            const float min_w = std::max(ImGui::GetFontSize() * 32, noti_w);
            ImGui::SetNextWindowSizeConstraints(ImVec2(min_w, ImGui::GetFontSize() * 32), ImVec2(8192, 8192));
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin(translationAchievementWindow[current_language], &show_achievements)) {
                // --- total completion progress bar ---
                {
                    int total = (int)achievements.size();
                    int done = 0;
                    for (const auto &a : achievements) if (a.achieved) ++done;
                    float fill = total > 0 ? (float)done / total : 0.0f;
                    float pct  = fill * 100.0f;

                    char left_buf[32]{};
                    snprintf(left_buf, sizeof(left_buf), "%d/%d", done, total);
                    char right_buf[32]{};
                    snprintf(right_buf, sizeof(right_buf), "%.1f%%", pct);

                    const float bar_h = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
                    ImVec2 bar_pos = ImGui::GetCursorScreenPos();
                    float bar_width = ImGui::GetContentRegionAvail().x;
                    ImGui::ProgressBar(fill, ImVec2(-1.0f, bar_h), "");

                    auto *dl = ImGui::GetWindowDrawList();
                    const ImU32 shadow_col = TC32(IM_COL32(0, 0, 0, 200));
                    const ImU32 text_col   = TC32(IM_COL32(255, 255, 255, 255));
                    auto draw_sh = [&](ImVec2 pos, const char *text) {
                        dl->AddText(ImVec2(pos.x + 1, pos.y + 1), shadow_col, text);
                        dl->AddText(pos, text_col, text);
                    };

                    // left: x/y
                    ImVec2 left_sz = ImGui::CalcTextSize(left_buf);
                    ImVec2 left_pos = { bar_pos.x + 4.0f, bar_pos.y + (bar_h - left_sz.y) * 0.5f };
                    draw_sh(left_pos, left_buf);

                    // right: percentage
                    ImVec2 right_sz = ImGui::CalcTextSize(right_buf);
                    ImVec2 right_pos = { bar_pos.x + bar_width - right_sz.x - 4.0f, bar_pos.y + (bar_h - right_sz.y) * 0.5f };
                    draw_sh(right_pos, right_buf);
                }

                // --- Reset / Simulate buttons ---
                if (ImGui::Button("Reset##ach_reset") && !achievements_snapshot.empty()) {
                    for (auto &ax : achievements) {
                        for (const auto &snap : achievements_snapshot) {
                            if (snap.name == ax.name) {
                                ax.achieved    = snap.achieved;
                                ax.progress    = snap.progress;
                                ax.unlock_time = snap.unlock_time;
                                break;
                            }
                        }
                    }
                    ach_global_percentages = ach_global_percentages_snapshot;
                }
                ImGui::SameLine();
                if (ImGui::Button(translationTestAchievement[current_language])) {
                    show_test_achievement();
                }
                ImGui::SameLine();
                if (ImGui::Button("Simulate##ach_simulate")) {
                    // random seed based on current time
                    static std::mt19937 rng(std::random_device{}());
                    std::uniform_int_distribution<uint32_t> percent_dist(0, 100);
                    std::uniform_real_distribution<float> global_dist(0.5f, 99.9f);
                    std::uniform_int_distribution<uint32_t> time_dist(0, 5u * 24u * 3600u); // up to 5 days ago
                    const uint32_t now_ts = (uint32_t)std::time(nullptr);

                    for (auto &ax : achievements) {
                        if (ax.hidden) continue; // skip hidden achievements

                        uint32_t roll = percent_dist(rng);
                        if (ax.max_progress > 0) {
                            // progress achievement: ~40% chance fully achieved, ~40% partial, ~20% zero
                            if (roll < 40) {
                                ax.achieved = true;
                                ax.progress = ax.max_progress;
                                ax.unlock_time = now_ts - time_dist(rng);
                            } else if (roll < 80) {
                                ax.achieved = false;
                                std::uniform_int_distribution<uint32_t> prog_dist(1, ax.max_progress - 1);
                                ax.progress = (ax.max_progress > 1) ? prog_dist(rng) : 0;
                                ax.unlock_time = 0;
                            } else {
                                ax.achieved = false;
                                ax.progress = 0;
                                ax.unlock_time = 0;
                            }
                        } else {
                            // regular achievement: ~40% achieved, ~30% in-progress (fake), ~30% locked
                            if (roll < 40) {
                                ax.achieved = true;
                                ax.unlock_time = now_ts - time_dist(rng);
                                ax.max_progress = 0;
                                ax.progress = 0;
                            } else if (roll < 70) {
                                ax.achieved = false;
                                ax.unlock_time = 0;
                                // assign a fake progress bar so it shows up in "In Progress" tab
                                std::uniform_int_distribution<uint32_t> fake_max(5, 50);
                                ax.max_progress = fake_max(rng);
                                std::uniform_int_distribution<uint32_t> fake_prog(1, ax.max_progress - 1);
                                ax.progress = fake_prog(rng);
                            } else {
                                ax.achieved = false;
                                ax.unlock_time = 0;
                                ax.max_progress = 0;
                                ax.progress = 0;
                            }
                        }

                        // simulate a global percentage only if we don't have a real one
                        if (ach_global_percentages.find(ax.name) == ach_global_percentages.end())
                            ach_global_percentages[ax.name] = global_dist(rng);
                    }
                }

                // ---- Tab bar: In Progress | My Achievements | [Groups] | Global Stats ----
                Steam_User_Stats *steamUserStats_sh = get_steam_client()->steam_user_stats;
                bool has_sh_groups = steamUserStats_sh->steamhunters_data_populated
                                     && !steamUserStats_sh->steamhunters_achievement_groups.empty();
                if (ImGui::BeginTabBar("##ach_tabs")) {
                    if (ImGui::BeginTabItem("In Progress##ach_tab0")) {
                        ach_current_tab = 0;
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("My Achievements##ach_tab1")) {
                        ach_current_tab = 1;
                        ImGui::EndTabItem();
                    }
                    if (has_sh_groups) {
                        if (ImGui::BeginTabItem("Groups##ach_tab2")) {
                            ach_current_tab = 2;
                            ImGui::EndTabItem();
                        }
                    }
                    if (ImGui::BeginTabItem("Global Stats##ach_tab3")) {
                        ach_current_tab = 3;
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                // safety: if groups disappeared while on Groups tab, fall back
                if (ach_current_tab == 2 && !has_sh_groups) ach_current_tab = 1;

                // ---- Search + sort controls ----
                // measure button width to size the search box dynamically
                const auto &style = ImGui::GetStyle();
                float btn_w = 0.0f;
                float spacing = style.ItemSpacing.x;
                const char *sort_labels[] = { "Sort: Global %%##ach_srt", "Sort: Schema Order##ach_srt", "Sort: A-Z##ach_srt" };
                const char *srt_lbl = sort_labels[ach_sort_mode % 3];
                btn_w += ImGui::CalcTextSize(srt_lbl).x + style.FramePadding.x * 2.0f + spacing;
                float avail = ImGui::GetContentRegionAvail().x;
                float search_w = avail - btn_w;
                if (search_w < ImGui::GetFontSize() * 6.0f) search_w = ImGui::GetFontSize() * 6.0f;

                ImGui::SetNextItemWidth(search_w);
                ImGui::InputTextWithHint("##ach_search", "Search achievements...", ach_search_buf, sizeof(ach_search_buf));

                ImGui::SameLine();
                if (ImGui::Button(sort_labels[ach_sort_mode % 3]))
                    ach_sort_mode = (ach_sort_mode + 1) % 3;

                ImGui::Separator();

                ImGui::BeginChild(translationAchievements[current_language]);

                // ---- search filter (case-insensitive substring) ----
                std::string search_lower;
                bool has_search = ach_search_buf[0] != '\0';
                if (has_search) {
                    search_lower = ach_search_buf;
                    std::transform(search_lower.begin(), search_lower.end(), search_lower.begin(),
                        [](unsigned char c){ return std::tolower(c); });
                }
                auto ach_matches_search = [&](const Overlay_Achievement &a) -> bool {
                    if (!has_search) return true;
                    std::string t = a.title;
                    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c){ return std::tolower(c); });
                    if (t.find(search_lower) != std::string::npos) return true;
                    std::string n = a.name;
                    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c){ return std::tolower(c); });
                    if (n.find(search_lower) != std::string::npos) return true;
                    std::string d = a.description;
                    std::transform(d.begin(), d.end(), d.begin(), [](unsigned char c){ return std::tolower(c); });
                    return d.find(search_lower) != std::string::npos;
                };

                // ---- tab filter ----
                auto ach_matches_tab = [&](const Overlay_Achievement &a) -> bool {
                    switch (ach_current_tab) {
                        case 0: // In Progress: has progress tracking and partial progress, not achieved
                            return !a.achieved && a.max_progress > 0 && a.progress > 0;
                        case 1: // My Achievements: all
                            return true;
                        case 2: // Global Stats: all
                            return true;
                        default: return true;
                    }
                };

                // ---- obtainability info from SteamHunters ----
                bool has_sh_data = steamUserStats_sh->steamhunters_data_populated
                                   && !steamUserStats_sh->steamhunters_achievement_data.empty();

                // ---- helper: get best global % (steam → SH local → -1) ----
                auto get_ach_pct = [&](const Overlay_Achievement &a) -> float {
                    auto it = ach_global_percentages.find(a.name);
                    if (it != ach_global_percentages.end() && it->second >= 0.0f) return it->second;
                    if (has_sh_data) {
                        auto sit = steamUserStats_sh->steamhunters_achievement_data.find(a.name);
                        if (sit != steamUserStats_sh->steamhunters_achievement_data.end() && sit->second.localPercentage >= 0.0f)
                            return sit->second.localPercentage;
                    }
                    return -1.0f;
                };

                // ---- comparator (unified across all tabs) ----
                auto ach_compare = [&](size_t ai, size_t bi) -> bool {
                    const auto &a = achievements[ai];
                    const auto &b = achievements[bi];
                    // hidden (locked) achievements last, except Global Stats tab
                    if (ach_current_tab != 3) {
                        bool a_hidden = a.hidden && !a.achieved;
                        bool b_hidden = b.hidden && !b.achieved;
                        if (a_hidden != b_hidden) return !a_hidden;
                        if (a_hidden) return false;
                    }
                    // My Achievements / Groups: achieved first by unlock time
                    if (ach_current_tab == 1 || ach_current_tab == 2) {
                        if (a.achieved != b.achieved) return a.achieved > b.achieved;
                        if (a.achieved) return a.unlock_time > b.unlock_time;
                    }
                    // sort mode
                    if (ach_sort_mode == 0) {
                        float pa = get_ach_pct(a), pb = get_ach_pct(b);
                        if (pa != pb) return pa > pb;
                    } else if (ach_sort_mode == 2) {
                        int cmp = a.title.compare(b.title);
                        if (cmp != 0) return cmp < 0;
                    }
                    return ai < bi; // tie-break: schema order
                };

                // ---- per-achievement render lambda ----
                auto render_ach_item = [&](Overlay_Achievement &x) {
                    bool achieved = x.achieved;
                    bool hidden = x.hidden && !achieved;

                    try_load_ach_icon(x, true,  settings->paginated_achievements_icons == 0);
                    try_load_ach_icon(x, false, settings->paginated_achievements_icons == 0);

                    ImGui::Separator();

                    const float icon_col_w = settings->overlay_appearance.icon_size;
                    const float bar_h      = settings->overlay_appearance.font_size;
                    bool has_icon = x.icon->GetResourceId() != 0 || x.icon_gray->GetResourceId() != 0;
                    bool rendered = false;

                    // Title line with obtainability badges
                    if (has_icon) {
                        const char *sym_for_measure = achieved ? u8"\u2713" : u8"\u2717";
                        float sym_w = ImGui::CalcTextSize(sym_for_measure).x;
                        float title_x_offset = (icon_col_w - sym_w) * 0.5f;
                        if (title_x_offset > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + title_x_offset);
                    }
                    if (hidden) ImGui::Text("%s", translationHiddenAchievement[current_language]);
                    else        ImGui::Text("%s", x.title.c_str());

                    // Obtainability badges (SteamHunters)
                    if (has_sh_data) {
                        auto sit = steamUserStats_sh->steamhunters_achievement_data.find(x.name);
                        if (sit != steamUserStats_sh->steamhunters_achievement_data.end() && sit->second.obtainability > 0) {
                            ImGui::SameLine();
                            switch (sit->second.obtainability) {
                                case 1: ImGui::TextColored(TC(ImVec4(1.0f, 0.85f, 0.0f, 1.0f)), "[Missable]"); break;
                                case 2: ImGui::TextColored(TC(ImVec4(0.9f, 0.2f, 0.2f, 1.0f)), "[Bugged]"); break;
                                case 3: ImGui::TextColored(TC(ImVec4(1.0f, 0.6f, 0.0f, 1.0f)), "[Online Only]"); break;
                                default: ImGui::TextColored(TC(ImVec4(0.7f, 0.7f, 0.7f, 1.0f)), "[Special]"); break;
                            }
                        }
                    }

                    if (has_icon) {
                        std::string tbl_id = std::string("##ach_") + x.name;
                        if (ImGui::BeginTable(tbl_id.c_str(), 2)) {
                            rendered = true;
                            ImGui::TableSetupColumn("img", ImGuiTableColumnFlags_WidthFixed, icon_col_w);
                            ImGui::TableSetupColumn("txt");
                            ImGui::TableNextRow(ImGuiTableRowFlags_None, icon_col_w);
                            ImGui::TableSetColumnIndex(0);
                            auto &icon_rsrc = (achieved && !hidden) ? x.icon : x.icon_gray;
                            if (icon_rsrc->GetResourceId() != 0)
                                ImGui::Image(icon_rsrc->GetResourceId(), ImVec2(icon_col_w, icon_col_w));
                            ImGui::TableSetColumnIndex(1);
                            if (!hidden) ImGui::TextWrapped("%s", x.description.c_str());
                            else         ImGui::TextDisabled("%s", x.description.c_str());
                            ImGui::EndTable();
                        }
                    }

                    if (!rendered) {
                        if (hidden) {
                            ImGui::TextDisabled("%s", x.description.c_str());
                        } else {
                            ImGui::TextWrapped("%s", x.description.c_str());
                        }
                    }

                    // --- status bar ---
                    {
                        const char *sym;
                        ImU32 sym_col;
                        bool has_progress = !achieved && x.max_progress > 0;
                        if (achieved) {
                            sym = u8"\u2713"; sym_col = TC32(IM_COL32(0, 220, 0, 255));
                        } else if (has_progress && x.progress > 0) {
                            sym = u8"\u25B6"; sym_col = TC32(IM_COL32(255, 180, 0, 255));
                        } else {
                            sym = u8"\u2717"; sym_col = TC32(IM_COL32(220, 0, 0, 255));
                        }

                        char date_buf[128]{};
                        if (achieved) {
                            char tmp[80]{};
                            time_t unlock_time = (time_t)x.unlock_time;
                            size_t written = std::strftime(tmp, sizeof(tmp), settings->overlay_appearance.ach_unlock_datetime_format.c_str(), std::localtime(&unlock_time));
                            if (!written) std::strftime(tmp, sizeof(tmp), "%Y/%m/%d - %H:%M:%S", std::localtime(&unlock_time));
                            snprintf(date_buf, sizeof(date_buf), "%s", tmp);
                        }

                        char pbuf[32]{};
                        bool show_progress = x.max_progress > 1 || (x.max_progress > 0 && !achieved);
                        if (show_progress) snprintf(pbuf, sizeof(pbuf), "%u/%u", achieved ? x.max_progress : x.progress, x.max_progress);

                        float fill = achieved ? 1.0f : (has_progress ? (float)x.progress / (float)x.max_progress : 0.0f);
                        ImVec2 bar_pos   = ImGui::GetCursorScreenPos();
                        float  bar_width = ImGui::GetContentRegionAvail().x;
                        ImGui::ProgressBar(fill, ImVec2(-1.0f, bar_h), "");
                        auto  *dl  = ImGui::GetWindowDrawList();
                        ImFont *fnt = ImGui::GetFont();
                        const float sym_font_sz = bar_h * 0.8f;
                        const ImU32 shadow_col = TC32(IM_COL32(0, 0, 0, 200));

                        auto draw_shadowed = [&](ImVec2 pos, ImU32 col, const char *text) {
                            dl->AddText(ImVec2(pos.x + 1, pos.y + 1), shadow_col, text);
                            dl->AddText(pos, col, text);
                        };
                        auto draw_shadowed_ex = [&](ImFont *f, float sz, ImVec2 pos, ImU32 col, const char *text) {
                            dl->AddText(f, sz, ImVec2(pos.x + 1, pos.y + 1), shadow_col, text);
                            dl->AddText(f, sz, pos, col, text);
                        };

                        ImVec2 sym_sz  = fnt->CalcTextSizeA(sym_font_sz, FLT_MAX, 0.0f, sym);
                        ImVec2 sym_pos = { bar_pos.x + 4.0f, bar_pos.y + (bar_h - sym_sz.y) * 0.5f };
                        draw_shadowed_ex(fnt, sym_font_sz, sym_pos, sym_col, sym);

                        if (achieved && date_buf[0]) {
                            float  date_x   = sym_pos.x + sym_sz.x + 4.0f;
                            ImVec2 date_sz  = ImGui::CalcTextSize(date_buf);
                            ImVec2 date_pos = { date_x, bar_pos.y + (bar_h - date_sz.y) * 0.5f };
                            draw_shadowed(date_pos, TC32(IM_COL32(255, 255, 255, 255)), date_buf);
                        }

                        if (show_progress && pbuf[0]) {
                            ImVec2 pbar_sz  = ImGui::CalcTextSize(pbuf);
                            ImVec2 pbar_pos = { bar_pos.x + (bar_width - pbar_sz.x) * 0.5f, bar_pos.y + (bar_h - pbar_sz.y) * 0.5f };
                            draw_shadowed(pbar_pos, TC32(IM_COL32(255, 255, 255, 255)), pbuf);
                        }
                    }

                    // --- global % + SteamHunters community % ---
                    {
                        auto it = ach_global_percentages.find(x.name);
                        if (it != ach_global_percentages.end()) {
                            ImGui::TextDisabled(translationGlobalAchievementPercent[current_language], it->second);
                            // Show SteamHunters community % alongside if available
                            if (has_sh_data) {
                                auto sit = steamUserStats_sh->steamhunters_achievement_data.find(x.name);
                                if (sit != steamUserStats_sh->steamhunters_achievement_data.end() && sit->second.localPercentage >= 0.0f) {
                                    ImGui::SameLine();
                                    ImGui::TextDisabled("| %.1f%% of hunters", sit->second.localPercentage);
                                }
                            }
                        }
                    }

                    ImGui::Separator();
                }; // end render_ach_item

                // ---- build name → index map (needed for grouping) ----
                std::unordered_map<std::string, size_t> ach_name_idx;
                ach_name_idx.reserve(achievements.size());
                for (size_t i = 0; i < achievements.size(); ++i)
                    ach_name_idx[achievements[i].name] = i;

                // ---- build filtered index list (tab + search) ----
                std::vector<size_t> filtered_idx;
                filtered_idx.reserve(achievements.size());
                for (size_t i = 0; i < achievements.size(); ++i) {
                    if (!ach_matches_tab(achievements[i])) continue;
                    if (!ach_matches_search(achievements[i])) continue;
                    filtered_idx.push_back(i);
                }

                // ---- "My Achievements" tab: split into Unlocked/Locked sections ----
                if (ach_current_tab == 1) {
                    // Unlocked section
                    std::vector<size_t> unlocked, locked;
                    for (size_t fi : filtered_idx) {
                        if (achievements[fi].achieved) unlocked.push_back(fi);
                        else locked.push_back(fi);
                    }
                    std::stable_sort(unlocked.begin(), unlocked.end(), ach_compare);
                    std::stable_sort(locked.begin(), locked.end(), ach_compare);

                    char hdr_u[64]; snprintf(hdr_u, sizeof(hdr_u), "Unlocked (%d)##ach_unlocked", (int)unlocked.size());
                    char hdr_l[64]; snprintf(hdr_l, sizeof(hdr_l), "Locked (%d)##ach_locked", (int)locked.size());

                    ImGui::PushStyleColor(ImGuiCol_Header, TC(ImVec4(0.15f, 0.35f, 0.15f, 0.80f)));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TC(ImVec4(0.20f, 0.45f, 0.20f, 0.90f)));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive, TC(ImVec4(0.25f, 0.55f, 0.25f, 1.00f)));
                    bool open_u = ImGui::CollapsingHeader(hdr_u, ImGuiTreeNodeFlags_DefaultOpen);
                    ImGui::PopStyleColor(3);
                    if (open_u) {
                        for (size_t si : unlocked) render_ach_item(achievements[si]);
                    }

                    ImGui::PushStyleColor(ImGuiCol_Header, TC(ImVec4(0.35f, 0.20f, 0.20f, 0.80f)));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TC(ImVec4(0.45f, 0.25f, 0.25f, 0.90f)));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive, TC(ImVec4(0.55f, 0.30f, 0.30f, 1.00f)));
                    bool open_l = ImGui::CollapsingHeader(hdr_l, ImGuiTreeNodeFlags_DefaultOpen);
                    ImGui::PopStyleColor(3);
                    if (open_l) {
                        for (size_t si : locked) render_ach_item(achievements[si]);
                    }
                } else if (ach_current_tab == 2 && has_sh_groups) {
                    // === GROUPED RENDER (Groups tab) ===
                    std::unordered_set<size_t> rendered_set;
                    std::unordered_set<size_t> filtered_set(filtered_idx.begin(), filtered_idx.end());

                    for (const auto &grp : steamUserStats_sh->steamhunters_achievement_groups) {
                        std::vector<size_t> grp_idx;
                        for (const auto &api_name : grp.achievementApiNames) {
                            auto it = ach_name_idx.find(api_name);
                            if (it != ach_name_idx.end() && filtered_set.count(it->second)) {
                                grp_idx.push_back(it->second);
                                rendered_set.insert(it->second);
                            }
                        }
                        if (grp_idx.empty()) continue;
                        std::stable_sort(grp_idx.begin(), grp_idx.end(), ach_compare);

                        // group header: "DLC Name" or "DLC Name — Sub-group"
                        std::string hdr = grp.dlcAppName.empty() ? "Base Game" : grp.dlcAppName;
                        if (!grp.name.empty()) hdr += " \xe2\x80\x94 " + grp.name; // em-dash

                        // count achieved in group
                        int grp_done = 0;
                        for (size_t gi : grp_idx) if (achievements[gi].achieved) ++grp_done;
                        char grp_count[32]; snprintf(grp_count, sizeof(grp_count), " (%d/%d)", grp_done, (int)grp_idx.size());
                        hdr += grp_count;

                        ImGui::PushStyleColor(ImGuiCol_Header, TC(ImVec4(0.20f, 0.30f, 0.45f, 0.80f)));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TC(ImVec4(0.25f, 0.38f, 0.55f, 0.90f)));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive, TC(ImVec4(0.30f, 0.45f, 0.65f, 1.00f)));
                        bool open = ImGui::CollapsingHeader(hdr.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                        ImGui::PopStyleColor(3);
                        if (open) {
                            for (size_t gi : grp_idx) render_ach_item(achievements[gi]);
                        }
                    }

                    // render any achievements not assigned to any group as "Other"
                    std::vector<size_t> ungrouped;
                    for (size_t fi : filtered_idx)
                        if (rendered_set.find(fi) == rendered_set.end()) ungrouped.push_back(fi);
                    if (!ungrouped.empty()) {
                        std::stable_sort(ungrouped.begin(), ungrouped.end(), ach_compare);
                        int ug_done = 0;
                        for (size_t gi : ungrouped) if (achievements[gi].achieved) ++ug_done;
                        char ug_hdr[64]; snprintf(ug_hdr, sizeof(ug_hdr), "Base Game (%d/%d)##ach_base", ug_done, (int)ungrouped.size());
                        ImGui::PushStyleColor(ImGuiCol_Header, TC(ImVec4(0.20f, 0.30f, 0.45f, 0.80f)));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TC(ImVec4(0.25f, 0.38f, 0.55f, 0.90f)));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive, TC(ImVec4(0.30f, 0.45f, 0.65f, 1.00f)));
                        bool open = ImGui::CollapsingHeader(ug_hdr, ImGuiTreeNodeFlags_DefaultOpen);
                        ImGui::PopStyleColor(3);
                        if (open) {
                            for (size_t gi : ungrouped) render_ach_item(achievements[gi]);
                        }
                    }
                } else {
                    // === FLAT SORTED RENDER ===
                    std::stable_sort(filtered_idx.begin(), filtered_idx.end(), ach_compare);
                    for (size_t si : filtered_idx) render_ach_item(achievements[si]);
                }

                if (filtered_idx.empty()) {
                    if (ach_current_tab == 0)
                        ImGui::TextDisabled("No achievements with active progress.");
                    else if (has_search)
                        ImGui::TextDisabled("No achievements match your search.");
                }

                ImGui::EndChild();
            }

            ImGui::End();
        }

        // SCE asset browser window — ≥50% wide, shown when Browse SCE Assets is toggled on
        if (show_sce_browser) {
            Steam_User_Stats *user_stats = get_steam_client()->steam_user_stats;
            if (!user_stats->sce_data_populated || user_stats->sce_game_data.series.empty()) {
                show_sce_browser = false;
                sce_textures_pending_free = true;  // defer to next frame start
            } else {
                auto local_storage = get_steam_client()->local_storage;

                // Same subfolder mapping as the downloader
                auto type_subfolder = [](int tidx) -> const char * {
                    static constexpr const char *dirs[] = {
                        "cards", "foil_cards", "booster_packs",
                        "badges", "foil_badges", "emoticons",
                        "backgrounds", "animated_backgrounds", "animated_mini_backgrounds",
                        "profiles", "avatar_frames", "animated_avatars",
                        "animated_stickers",
                        "startup_movies"
                    };
                    return (tidx >= 0 && tidx < 14) ? dirs[tidx] : "misc";
                };

                // Same sanitize as the downloader
                auto sanitize = [](std::string s) -> std::string {
                    for (char &c : s) {
                        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                            c == '?' || c == '"'  || c == '<' || c == '>' || c == '|')
                            c = '_';
                    }
                    return s;
                };

                // Extract filename extension from a URL
                auto url_ext = [](const std::string &url) -> std::string {
                    size_t slash = url.rfind('/');
                    std::string name = (slash != std::string::npos) ? url.substr(slash + 1) : url;
                    size_t q = name.find('?');
                    if (q != std::string::npos) name.resize(q);
                    size_t dot = name.rfind('.');
                    return (dot != std::string::npos) ? name.substr(dot) : ".png";
                };

                // Per-type card dimensions: {card_width, image_height}
                // Mirrors SCE website proportions: portrait for cards, landscape for BGs, etc.
                struct CardDims { float cw, ih; };
                static constexpr CardDims kDims[14] = {
                    {160.f, 200.f},  // 0  cards                 (portrait ~0.80)
                    {160.f, 200.f},  // 1  foil_cards
                    {160.f, 160.f},  // 2  booster_packs          (square)
                    {160.f, 160.f},  // 3  badges
                    {160.f, 160.f},  // 4  foil_badges
                    {160.f, 160.f},  // 5  emoticons
                    {220.f, 124.f},  // 6  backgrounds            (16:9)
                    {220.f, 124.f},  // 7  animated_backgrounds
                    {220.f, 124.f},  // 8  animated_mini_backgrounds
                    {160.f, 160.f},  // 9  profiles
                    {160.f, 160.f},  // 10 avatar_frames
                    {160.f, 160.f},  // 11 animated_avatars
                    {160.f, 160.f},  // 12 animated_stickers
                    {220.f, 124.f},  // 13 startup_movies         (16:9 landscape)
                };

                // Rarity colours matching SCE site: common=grey, uncommon=green, rare=red
                auto rarity_color = [](const std::string &r) -> ImVec4 {
                    if (r == "Uncommon") return {0.20f, 0.85f, 0.20f, 1.0f};
                    if (r == "Rare")     return {0.90f, 0.20f, 0.20f, 1.0f};
                    return {0.60f, 0.60f, 0.60f, 1.0f};
                };

                constexpr int MAX_TEX_PER_FRAME = 4;
                int tex_loaded_this_frame = 0;
                constexpr float CARD_GAP = 12.0f;

                const float min_w = io.DisplaySize.x * 0.60f;
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(min_w, io.DisplaySize.y * 0.50f),
                    ImVec2(8192.0f, 8192.0f));
                ImGui::SetNextWindowBgAlpha(1.0f);
                bool browser_open = show_sce_browser;
                if (ImGui::Begin("SCE Assets##sce_browser", &browser_open)) {
                    ImGui::TextDisabled("Assets folder: GSE Saves/%u/%s",
                        user_stats->sce_game_data.appid,
                        Steam_User_Stats::sce_assets_folder);
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Tab grouping: multiple asset types share a tab
                    struct SceTabGroup { const char *label; int types[4]; int ntype; };
                    static constexpr SceTabGroup kTabs[] = {
                        { "Cards",       { 0,  1,  2, -1}, 3 },  // Trading Cards + Foil Cards + Booster Packs
                        { "Badges",      { 3,  4, -1, -1}, 2 },  // Badges + Foil Badges
                        { "Backgrounds", { 6,  7,  8, -1}, 3 },  // Backgrounds + Animated + Animated Mini
                        { "Chat",        { 5, 12, -1, -1}, 2 },  // Emoticons + Animated Stickers
                        { "Profiles",    {11, 10,  9, -1}, 3 },  // Animated Avatars + Avatar Frames + Profiles
                        { "Steam",       {13, -1, -1, -1}, 1 },  // Startup Movies (keyboard themes not on SCE)
                    };

                    for (const auto &series : user_stats->sce_game_data.series) {
                        char ser_label[128]{};
                        if (!series.series_name.empty())
                            snprintf(ser_label, sizeof(ser_label),
                                "Series %d - %s", series.series_number, series.series_name.c_str());
                        else
                            snprintf(ser_label, sizeof(ser_label), "Series %d", series.series_number);

                        char ser_num_str[8]{};
                        snprintf(ser_num_str, sizeof(ser_num_str), "%02d", series.series_number);
                        std::string ser_dir = std::string("Series ") + ser_num_str;
                        if (!series.series_name.empty())
                            ser_dir += " - " + sanitize(series.series_name);

                        if (!ImGui::CollapsingHeader(ser_label, ImGuiTreeNodeFlags_DefaultOpen))
                            continue;

                        // Group items by type
                        std::map<int, std::vector<const Steam_User_Stats::SceItem *>> by_type;
                        for (const auto &item : series.items)
                            by_type[(int)item.type].push_back(&item);

                        char tab_bar_id[32]{};
                        snprintf(tab_bar_id, sizeof(tab_bar_id), "##tb_%d", series.series_number);
                        if (!ImGui::BeginTabBar(tab_bar_id)) { ImGui::Spacing(); continue; }

                        for (int tgi = 0; tgi < (int)(sizeof(kTabs)/sizeof(kTabs[0])); ++tgi) {
                            const auto &tg = kTabs[tgi];

                            // Count total items for this tab group in this series
                            int tab_total = 0;
                            for (int ti = 0; ti < tg.ntype; ++ti) {
                                auto it2 = by_type.find(tg.types[ti]);
                                if (it2 != by_type.end()) tab_total += (int)it2->second.size();
                            }
                            if (tab_total == 0) continue;

                            char tab_lbl[80]{};
                            snprintf(tab_lbl, sizeof(tab_lbl), "%s (%d)##tb_%d_%d",
                                tg.label, tab_total, series.series_number, tgi);
                            if (!ImGui::BeginTabItem(tab_lbl)) continue;

                            bool first_type = true;
                            for (int ti = 0; ti < tg.ntype; ++ti) {
                                int tidx = tg.types[ti];
                                auto it = by_type.find(tidx);
                                if (it == by_type.end() || it->second.empty()) continue;

                                const auto &items_vec = it->second;
                                float card_w = kDims[tidx].cw;
                                float img_h  = kDims[tidx].ih;

                                // Sub-header per type
                                {
                                    if (!first_type) ImGui::Spacing();
                                    ImGui::TextDisabled("%s  (%zu)",
                                        Steam_User_Stats::SCE_TYPE_LABELS[tidx], items_vec.size());
                                    ImGui::Separator();
                                    ImGui::Spacing();
                                }
                                first_type = false;

                                // Wrapping card grid
                                float avail_w = ImGui::GetContentRegionAvail().x;
                                int cols = std::max(1, (int)((avail_w + CARD_GAP) / (card_w + CARD_GAP)));
                                char tbl_id[32]{};
                                snprintf(tbl_id, sizeof(tbl_id), "##cg_%d_%d", series.series_number, tidx);
                                if (ImGui::BeginTable(tbl_id, cols,
                                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody |
                                        ImGuiTableFlags_NoPadOuterX)) {
                                    for (int c = 0; c < cols; ++c)
                                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, card_w);

                                    int slot_idx = 0;
                                    for (const auto *item : items_vec) {
                                        ++slot_idx;
                                        ImGui::TableNextColumn();

                                        // Is this a (animated) background? Use dedicated thumb file.
                                        bool is_bg = (tidx == 6 || tidx == 7 || tidx == 8);

                                        // Build thumbnail file path (mirrors downloader naming)
                                        const std::string &thumb_src = is_bg
                                            ? (item->wallpaper_url.empty() ? item->static_img_url : item->wallpaper_url)
                                            : (!item->icon_url.empty() ? item->icon_url : item->static_img_url);
                                        std::string ext = url_ext(thumb_src);
                                        std::string item_label = sanitize(item->name);
                                        if (item_label.size() > 48) item_label.resize(48);
                                        char prefix[16]{};
                                        snprintf(prefix, sizeof(prefix), "%02d_", slot_idx);
                                        // Backgrounds: "01_thumb_wallpaper_Name.jpg"; others: "01_icon_Name.ext"
                                        std::string filename = is_bg
                                            ? (std::string(prefix) + "thumb_wallpaper_" + item_label + ext)
                                            : (std::string(prefix) + "icon_" + item_label + ext);
                                        std::string type_dir = type_subfolder(tidx);
                                        std::string folder = std::string(Steam_User_Stats::sce_assets_folder)
                                            + PATH_SEPARATOR + ser_dir
                                            + PATH_SEPARATOR + type_dir;
                                        std::string tex_key = folder + PATH_SEPARATOR + filename;

                                        bool is_static = (ext != ".gif" && ext != ".mp4" && ext != ".webm");
                                        auto &tex = sce_textures[tex_key];
                                        if (is_static && !tex.load_attempted
                                                && tex_loaded_this_frame < MAX_TEX_PER_FRAME) {
                                            tex.load_attempted = true;
                                            int pw = 0, ph = 0;
                                            tex.pixels = local_storage->load_image_from_folder(folder, filename, pw, ph);
                                            if (!tex.pixels.empty() && _renderer && pw > 0 && ph > 0) {
                                                tex.resource = _renderer->CreateResource();
                                                tex.w = pw; tex.h = ph;
                                                if (should_use_fp16()) {
                                                    auto fp16 = transform_pixels_to_fp16((const uint8_t*)tex.pixels.data(), pw, ph,
                                                                                          effective_swapchain_cs(), s_sdr_white_scale,
                                                                                          s_img_brightness, s_img_contrast, s_img_gamma_adj);
                                                    tex.fp16_pixels.assign(reinterpret_cast<const char*>(fp16.data()), fp16.size() * sizeof(uint16_t));
                                                    tex.resource->AttachResource((void*)tex.fp16_pixels.data(), (uint32_t)pw, (uint32_t)ph,
                                                                                 InGameOverlay::RendererPixelFormat::RGBA16F);
                                                } else {
                                                    srgb_decode_pixels_if_needed(_renderer, settings->overlay_appearance.image_gamma,
                                                        (uint8_t*)tex.pixels.data(), (size_t)pw * ph);
                                                    tex.resource->AttachResource(tex.pixels.data(), (uint32_t)pw, (uint32_t)ph);
                                                }
                                            }
                                            ++tex_loaded_this_frame;
                                        }

                                        // --- Card ---
                                        ImGui::BeginGroup();

                                        // Slot / total line above image
                                        if (item->slot > 0 && item->total > 0)
                                            ImGui::TextDisabled("#%d of %d", item->slot, item->total);
                                        else if (item->slot > 0)
                                            ImGui::TextDisabled("#%d", item->slot);
                                        else
                                            ImGui::TextDisabled(" ");

                                        // Image area
                                        ImVec2 p0 = ImGui::GetCursorScreenPos();
                                        ImVec2 p1 = ImVec2(p0.x + card_w, p0.y + img_h);
                                        ImDrawList *dl = ImGui::GetWindowDrawList();
                                        dl->AddRectFilled(p0, p1, TC32(IM_COL32(40, 40, 50, 255)));

                                        char btn_id[64]{};
                                        snprintf(btn_id, sizeof(btn_id), "##bg_%d_%d_%d", series.series_number, tidx, slot_idx);
                                        ImGui::InvisibleButton(btn_id, ImVec2(card_w, img_h));
                                        bool clicked = ImGui::IsItemClicked();

                                        if (is_static && tex.resource
                                                && tex.resource->GetResourceId() != 0) {
                                            // Aspect-fit (letterbox) image into card_w × img_h
                                            float sa = (tex.h > 0) ? (float)tex.w / tex.h : 1.0f;
                                            float da = card_w / img_h;
                                            float dw, dh, ox = 0.f, oy = 0.f;
                                            if (sa >= da) {
                                                dw = card_w; dh = card_w / sa;
                                                oy = (img_h - dh) * 0.5f;
                                            } else {
                                                dh = img_h; dw = img_h * sa;
                                                ox = (card_w - dw) * 0.5f;
                                            }
                                            dl->AddImage(tex.resource->GetResourceId(),
                                                ImVec2(p0.x + ox, p0.y + oy),
                                                ImVec2(p0.x + ox + dw, p0.y + oy + dh));
                                            // Hover highlight
                                            if (ImGui::IsItemHovered())
                                                dl->AddRect(p0, p1, TC32(IM_COL32(200, 200, 255, 180)), 0.f, 2.f, 0);
                                        } else if (!is_static) {
                                            // Animated/video: show extension badge centred
                                            const char *badge = ext.size() > 1 ? ext.c_str() + 1 : ext.c_str();
                                            ImVec2 tsz = ImGui::CalcTextSize(badge);
                                            dl->AddText(
                                                ImVec2(p0.x + (card_w - tsz.x) * 0.5f,
                                                       p0.y + (img_h  - tsz.y) * 0.5f),
                                                TC32(IM_COL32(160, 160, 160, 255)), badge);
                                        }

                                        // On click: open full-size preview.
                                        // Animated BGs (types 7/8) use wallpaper_ file; all others use icon_.
                                        bool is_animated_bg = (tidx == 7 || tidx == 8);
                                        bool can_preview = is_animated_bg ? !item->wallpaper_url.empty() : is_static;
                                        if (clicked && can_preview) {
                                            if (is_animated_bg) {
                                                std::string full_ext = url_ext(item->wallpaper_url);
                                                std::string full_filename = std::string(prefix) + "wallpaper_" + item_label + full_ext;
                                                sce_preview_key = folder + PATH_SEPARATOR + full_filename;
                                            } else {
                                                sce_preview_key = tex_key;
                                            }
                                            // Build nav keys: same type within same series
                                            sce_preview_nav_keys.clear();
                                            for (int ni = 0; ni < (int)items_vec.size(); ++ni) {
                                                const auto *nitem = items_vec[ni];
                                                int nslot2 = ni + 1;
                                                char npfx[16]{}; snprintf(npfx, sizeof(npfx), "%02d_", nslot2);
                                                std::string nlbl = sanitize(nitem->name);
                                                if (nlbl.size() > 48) nlbl.resize(48);
                                                std::string npreview_file;
                                                if (is_animated_bg) {
                                                    if (nitem->wallpaper_url.empty()) continue;
                                                    std::string next = url_ext(nitem->wallpaper_url);
                                                    npreview_file = std::string(npfx) + "wallpaper_" + nlbl + next;
                                                } else {
                                                    const std::string &nsrc = !nitem->icon_url.empty() ? nitem->icon_url : nitem->static_img_url;
                                                    std::string next = url_ext(nsrc);
                                                    npreview_file = std::string(npfx) + "icon_" + nlbl + next;
                                                }
                                                sce_preview_nav_keys.push_back(folder + PATH_SEPARATOR + npreview_file);
                                            }
                                        }

                                        // Name (wrapped to card width)
                                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card_w);
                                        ImGui::TextUnformatted(item->name.c_str());
                                        ImGui::PopTextWrapPos();

                                        // Rarity (coloured)
                                        if (!item->rarity.empty() && item->rarity != "Unknown")
                                            ImGui::TextColored(rarity_color(item->rarity),
                                                "%s", item->rarity.c_str());
                                        else
                                            ImGui::TextDisabled(" ");

                                        // Price
                                        if (!item->price_text.empty())
                                            ImGui::TextDisabled("%s", item->price_text.c_str());
                                        else
                                            ImGui::TextDisabled(" ");

                                        // Badge level / XP (types 3 + 4)
                                        if ((tidx == 3 || tidx == 4) && (item->badge_level > 0 || item->badge_xp > 0)) {
                                            if (item->badge_level > 0 && item->badge_xp > 0)
                                                ImGui::TextDisabled("Lv.%d  %d XP", item->badge_level, item->badge_xp);
                                            else if (item->badge_level > 0)
                                                ImGui::TextDisabled("Lv.%d", item->badge_level);
                                            else
                                                ImGui::TextDisabled("%d XP", item->badge_xp);
                                        }

                                        // Emoticon shortcode (type 5)
                                        if (tidx == 5 && !item->emoticon_name.empty())
                                            ImGui::TextDisabled(":%s:", item->emoticon_name.c_str());

                                        // Steam Points cost (profiles, avatar frames, animated avatars, stickers)
                                        if (!item->points_price.empty())
                                            ImGui::TextDisabled("Pts: %s", item->points_price.c_str());

                                        ImGui::EndGroup();
                                    }
                                    ImGui::EndTable();
                                }
                                ImGui::Spacing();
                            }

                            ImGui::EndTabItem();
                        }
                        ImGui::EndTabBar();
                        ImGui::Spacing();
                    }
                }
                ImGui::End();

                // Full-size asset preview — standalone window
                if (!sce_preview_key.empty()) {
                    // Dim everything behind the preview window
                    ImGui::GetBackgroundDrawList()->AddRectFilled(
                        ImVec2(0, 0), io.DisplaySize, TC32(IM_COL32(0, 0, 0, 180)));

                    auto &ftex = sce_textures[sce_preview_key];
                    if (!ftex.load_attempted) {
                        // User explicitly opened this — bypass the per-frame cap
                        ftex.load_attempted = true;
                        size_t sep = sce_preview_key.rfind(PATH_SEPARATOR[0]);
                        if (sep != std::string::npos) {
                            std::string f_folder = sce_preview_key.substr(0, sep);
                            std::string f_file   = sce_preview_key.substr(sep + 1);
                            int pw = 0, ph = 0;
                            ftex.pixels = local_storage->load_image_from_folder(f_folder, f_file, pw, ph);
                            if (!ftex.pixels.empty() && _renderer && pw > 0 && ph > 0) {
                                ftex.resource = _renderer->CreateResource();
                                ftex.w = pw; ftex.h = ph;
                                if (should_use_fp16()) {
                                    auto fp16 = transform_pixels_to_fp16((const uint8_t*)ftex.pixels.data(), pw, ph,
                                                                          effective_swapchain_cs(), s_sdr_white_scale,
                                                                          s_img_brightness, s_img_contrast, s_img_gamma_adj);
                                    ftex.fp16_pixels.assign(reinterpret_cast<const char*>(fp16.data()), fp16.size() * sizeof(uint16_t));
                                    ftex.resource->AttachResource((void*)ftex.fp16_pixels.data(), (uint32_t)pw, (uint32_t)ph,
                                                                   InGameOverlay::RendererPixelFormat::RGBA16F);
                                } else {
                                    srgb_decode_pixels_if_needed(_renderer, settings->overlay_appearance.image_gamma,
                                        (uint8_t*)ftex.pixels.data(), (size_t)pw * ph);
                                    ftex.resource->AttachResource(ftex.pixels.data(), (uint32_t)pw, (uint32_t)ph);
                                }
                            }
                        }
                    }

                    float pw = io.DisplaySize.x * 0.75f;
                    float ph = pw * (9.f / 16.f); // default 16:9
                    if (ftex.w > 0 && ftex.h > 0) {
                        ph = pw * ((float)ftex.h / ftex.w);
                        float max_h = io.DisplaySize.y * 0.85f;
                        if (ph > max_h) { ph = max_h; pw = ph * ((float)ftex.w / ftex.h); }
                    }
                    ImVec2 wpos(
                        (io.DisplaySize.x - pw) * 0.5f,
                        (io.DisplaySize.y - ph) * 0.5f);
                    ImGui::SetNextWindowPos(wpos, ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_Always);
                    ImGui::SetNextWindowBgAlpha(0.97f);
                    bool preview_open = true;
                    if (ImGui::Begin("##sce_preview", &preview_open,
                            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
                        constexpr float BTN_H = 28.f;
                        constexpr float BTN_W = 64.f;
                        constexpr float NAV_PAD = 8.f; // padding above/below nav bar
                        float img_avail_h = ImGui::GetContentRegionAvail().y - BTN_H - NAV_PAD * 2.f;
                        if (ftex.resource && ftex.resource->GetResourceId() != 0) {
                            ImVec2 avail = ImVec2(ImGui::GetContentRegionAvail().x, img_avail_h);
                            float sa = (ftex.h > 0) ? (float)ftex.w / ftex.h : 1.0f;
                            float dw = avail.x, dh = avail.x / sa;
                            if (dh > avail.y) { dh = avail.y; dw = avail.y * sa; }
                            float padx = (avail.x - dw) * 0.5f;
                            float pady = (avail.y - dh) * 0.5f;
                            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + padx,
                                                       ImGui::GetCursorPosY() + pady));
                            ImGui::Image(ftex.resource->GetResourceId(), ImVec2(dw, dh));
                        } else {
                            ImGui::SetCursorPosY(img_avail_h * 0.45f);
                            ImGui::SetCursorPosX((pw - ImGui::CalcTextSize("Loading...").x) * 0.5f);
                            ImGui::TextDisabled("Loading...");
                        }
                        // --- Navigation buttons ---
                        int nav_cur = -1;
                        for (int ni = 0; ni < (int)sce_preview_nav_keys.size(); ++ni)
                            if (sce_preview_nav_keys[ni] == sce_preview_key) { nav_cur = ni; break; }

                        auto nav_to = [&](int ni) {
                            sce_preview_key = sce_preview_nav_keys[ni];
                        };

                        bool can_prev = nav_cur > 0;
                        bool can_next = nav_cur >= 0 && nav_cur < (int)sce_preview_nav_keys.size() - 1;

                        // Arrow / A-D key navigation (NoNav is set but IsKeyPressed still works)
                        if (can_prev && (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)  || ImGui::IsKeyPressed(ImGuiKey_A))) nav_to(nav_cur - 1);
                        if (can_next && (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_D))) nav_to(nav_cur + 1);

                        // Button bar pinned to bottom of window via screen coords
                        ImVec2 cpos = ImGui::GetWindowPos();
                        ImVec2 csz  = ImGui::GetWindowSize();
                        float  by   = cpos.y + csz.y - BTN_H - 6.f;

                        ImGui::SetCursorScreenPos(ImVec2(cpos.x + 6.f, by));
                        ImGui::BeginDisabled(!can_prev);
                        if (ImGui::Button("< Prev", ImVec2(BTN_W, BTN_H)) && can_prev) nav_to(nav_cur - 1);
                        ImGui::EndDisabled();

                        // Counter label centred
                        if (nav_cur >= 0) {
                            char cnt[32]{};
                            snprintf(cnt, sizeof(cnt), "%d / %d",
                                nav_cur + 1, (int)sce_preview_nav_keys.size());
                            ImVec2 tsz = ImGui::CalcTextSize(cnt);
                            ImGui::SetCursorScreenPos(ImVec2(
                                cpos.x + (csz.x - tsz.x) * 0.5f,
                                by + (BTN_H - tsz.y) * 0.5f));
                            ImGui::TextDisabled("%s", cnt);
                        }

                        ImGui::SetCursorScreenPos(ImVec2(cpos.x + csz.x - BTN_W - 6.f, by));
                        ImGui::BeginDisabled(!can_next);
                        if (ImGui::Button("Next >", ImVec2(BTN_W, BTN_H)) && can_next) nav_to(nav_cur + 1);
                        ImGui::EndDisabled();

                        if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                            ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                            (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
                              ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
                            preview_open = false;
                    }
                    ImGui::End();
                    if (!preview_open)
                        sce_preview_key.clear();
                }

                // Free textures when the window is closed (deferred to avoid DX9 crash)
                if (!browser_open) {
                    show_sce_browser = false;
                    sce_preview_key.clear();
                    sce_preview_nav_keys.clear();
                    sce_textures_pending_free = true;  // defer to next frame start
                }
            }
        }

        // user clicked on "settings" button
        if (show_settings) {
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin(translationGlobalSettingsWindow[current_language], &show_settings)) {
                ImGui::Text("%s", translationGlobalSettingsWindowDescription[current_language]);

                ImGui::Separator();

                ImGui::Text("%s", translationUsername[current_language]);
                ImGui::SameLine();
                ImGui::InputText("##username", username_text, sizeof(username_text), 0);

                ImGui::Separator();

                ImGui::Text("%s", translationLanguage[current_language]);
                ImGui::ListBox("##language", &current_language, valid_languages, sizeof(valid_languages) / sizeof(valid_languages[0]), 7);
                ImGui::Text(translationSelectedLanguage[current_language], valid_languages[current_language]);

                ImGui::Separator();

                ImGui::Text("%s", translationRestartTheGameToApply[current_language]);
                if (ImGui::Button(translationSave[current_language])) {
                    save_settings = true;
                    show_settings = false;
                }
            }

            ImGui::End();
        }

        // Networks panel — show all adapters and users grouped by subnet
        if (show_networks) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 20, ImGui::GetFontSize() * 12), ImVec2(8192, 8192));
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin("Networks", &show_networks)) {
                GSE_NetAdapter adapters[8];
                int count = Bridge_GetNetworkInfo(adapters, 8);

                if (count == 0) {
                    ImGui::TextColored(TC(ImVec4(0.5f, 0.5f, 0.5f, 1.0f)), "No network adapters detected");
                }

                for (int ai = 0; ai < count; ++ai) {
                    auto &a = adapters[ai];
                    // Collapsible header per adapter
                    char header[256];
                    // GCC-only suppression: MSVC does not understand `#pragma GCC` and
                    // reports each line as C4068 (unknown pragma), so the block has to be
                    // hidden from it. Same intent as the fread suppression in base.cpp.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
                    if (a.subnet_str[0])
                        snprintf(header, sizeof(header), "%s (%s) - %s", a.name, a.ip_str, a.subnet_str);
                    else
                        snprintf(header, sizeof(header), "%s", a.name);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

                    if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
                        if (a.range_str[0]) {
                            ImGui::TextColored(TC(ImVec4(0.6f, 0.6f, 0.6f, 1.0f)), "  Range: %s", a.range_str);
                        }
                        for (int ui = 0; ui < a.user_count; ++ui) {
                            auto &u = a.users[ui];
                            ImGui::PushID(ai * 100 + ui);

                            if (u.is_self) {
                                ImGui::TextColored(TC(ImVec4(0.5f, 0.7f, 0.9f, 1.0f)), u8"  \u25CF %s", u.name);
                                ImGui::SameLine();
                                ImGui::TextColored(TC(ImVec4(0.5f, 0.7f, 0.9f, 1.0f)), "(%s)", u.ip_str);
                                ImGui::SameLine();
                                ImGui::TextColored(TC(ImVec4(0.6f, 0.6f, 0.6f, 1.0f)), "[You]");
                            } else {
                                ImGui::TextColored(TC(ImVec4(0.9f, 0.9f, 0.2f, 1.0f)), u8"  \u25CF %s", u.name);
                                ImGui::SameLine();
                                ImGui::TextColored(TC(ImVec4(0.5f, 0.7f, 0.9f, 1.0f)), "(%s)", u.ip_str);
                            }
                            // Right-click context menu for copy
                            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                                ImGui::OpenPopup("##net_ctx");
                            if (ImGui::BeginPopup("##net_ctx")) {
                                if (ImGui::MenuItem("Copy IP")) {
                                    ImGui::SetClipboardText(u.ip_str);
                                }
                                if (ImGui::MenuItem("Copy Name")) {
                                    ImGui::SetClipboardText(u.name);
                                }
                                char id_str[32];
                                snprintf(id_str, sizeof(id_str), "%llu", (unsigned long long)u.steam_id);
                                if (ImGui::MenuItem("Copy Steam ID")) {
                                    ImGui::SetClipboardText(id_str);
                                }
                                ImGui::EndPopup();
                            }

                            ImGui::PopID();
                        }
                        if (a.user_count == 0) {
                            ImGui::TextColored(TC(ImVec4(0.5f, 0.5f, 0.5f, 1.0f)), "  (no users detected)");
                        }
                    }
                }
            }
            ImGui::End();
        }

        // Lobby Chat window
        if (show_lobby_chat && i_have_lobby) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 22, ImGui::GetFontSize() * 16), ImVec2(8192, 8192));
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin("Lobby Chat", &show_lobby_chat)) {
                GSE_LobbyChatState lcs{};
                bool got_state = Bridge_GetLobbyChatState(&lcs) != 0;

                if (got_state) {
                    // Header: member list
                    ImGui::TextColored(TC(ImVec4(0.5f, 0.7f, 0.9f, 1.0f)), "Members (%d):", lcs.member_count);
                    ImGui::SameLine();
                    for (int i = 0; i < lcs.member_count; ++i) {
                        if (i > 0) ImGui::SameLine();
                        bool is_self = (lcs.members[i].steam_id == settings->get_local_steam_id().ConvertToUint64());
                        if (is_self)
                            ImGui::TextColored(TC(ImVec4(0.4f, 0.8f, 1.0f, 1.0f)), "%s", lcs.members[i].name);
                        else
                            ImGui::TextColored(TC(ImVec4(0.9f, 0.9f, 0.2f, 1.0f)), "%s", lcs.members[i].name);
                        if (i < lcs.member_count - 1) {
                            ImGui::SameLine();
                            ImGui::TextColored(TC(ImVec4(0.5f, 0.5f, 0.5f, 1.0f)), ",");
                        }
                    }
                    ImGui::Separator();

                    // Chat history area
                    float footer_height = ImGui::GetFrameHeightWithSpacing() + 4;
                    ImGui::BeginChild("##lobby_chat_history", ImVec2(0, -footer_height), true);

                    if (lcs.chat_history[0]) {
                        char *history = lcs.chat_history;
                        char *line = history;
                        while (*line) {
                            char *end = strchr(line, '\n');
                            if (end) *end = '\0';

                            bool is_self = (strncmp(line, "You: ", 5) == 0);
                            if (is_self)
                                ImGui::TextColored(TC(ImVec4(0.4f, 0.8f, 1.0f, 1.0f)), "%s", line);
                            else
                                ImGui::TextColored(TC(ImVec4(0.9f, 0.9f, 0.2f, 1.0f)), "%s", line);

                            if (!end) break;
                            *end = '\n';
                            line = end + 1;
                        }
                    } else {
                        ImGui::TextDisabled("No messages yet.");
                    }

                    // Auto-scroll to bottom when new messages arrive
                    static size_t last_history_len = 0;
                    size_t cur_history_len = lcs.history_len;
                    if (cur_history_len != last_history_len) {
                        ImGui::SetScrollHereY(1.0f);
                        last_history_len = cur_history_len;
                    }

                    ImGui::EndChild();

                    // Input bar
                    float send_btn_w = ImGui::CalcTextSize("Send").x + ImGui::GetStyle().FramePadding.x * 2 + 8;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - send_btn_w - 8);

                    bool send_msg = false;
                    if (ImGui::InputText("##lobby_chat_input", lobby_chat_input, sizeof(lobby_chat_input),
                            ImGuiInputTextFlags_EnterReturnsTrue)) {
                        send_msg = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Send##lobby_send") || send_msg) {
                        if (lobby_chat_input[0]) {
                            Bridge_SendLobbyChatMsg(lobby_chat_input);
                            lobby_chat_input[0] = '\0';
                        }
                    }
                } else {
                    ImGui::TextColored(TC(ImVec4(0.5f, 0.5f, 0.5f, 1.0f)), "Not in a lobby.");
                }
            }
            ImGui::End();
        } else if (!i_have_lobby) {
            show_lobby_chat = false;
        }

        // we have a url to open/display
        if (show_url.size()) {
            std::string url = show_url;
            bool show = true;
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin(URL_WINDOW_NAME, &show)) {
                ImGui::Text("%s", translationSteamOverlayURL[current_language]);
                ImGui::Spacing();

                ImGui::PushItemWidth(ImGui::CalcTextSize(url.c_str()).x + 20);
                ImGui::InputText("##url_copy", (char *)url.data(), url.size(), ImGuiInputTextFlags_ReadOnly);
                ImGui::PopItemWidth();

                ImGui::Spacing();
                if (ImGui::Button(translationClose[current_language]) || !show)
                    show_url = "";
                // ImGui::SetWindowSize(ImVec2(ImGui::CalcTextSize(url.c_str()).x + 10, 0));
            }

            ImGui::End();
        }

        bool show_warning = warn_local_save || warn_bad_appid;
        if (show_warning) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 32, ImGui::GetFontSize() * 32), ImVec2(8192, 8192));
            ImGui::SetNextWindowFocus();
            ImGui::SetNextWindowBgAlpha(1.0f);
            if (ImGui::Begin(translationWarning[current_language], &show_warning)) {
                if (warn_bad_appid) {
                    ImGui::TextColored(TC(ImVec4(255, 0, 0, 255)),
                        "%s %s %s",
                        translationWarning[current_language], translationWarning[current_language], translationWarning[current_language]);
                    ImGui::TextWrapped("%s", translationWarningDescription_badAppid[current_language]);
                    ImGui::TextColored(TC(ImVec4(255, 0, 0, 255)),
                        "%s %s %s",
                        translationWarning[current_language], translationWarning[current_language], translationWarning[current_language]);
                }
                if (warn_local_save) {
                    ImGui::TextColored(TC(ImVec4(255, 0, 0, 255)),
                        "%s %s %s",
                        translationWarning[current_language], translationWarning[current_language], translationWarning[current_language]);
                    ImGui::TextWrapped("%s", translationWarningDescription_localSave[current_language]);
                    ImGui::TextColored(TC(ImVec4(255, 0, 0, 255)),
                        "%s %s %s",
                        translationWarning[current_language], translationWarning[current_language], translationWarning[current_language]);
                }
            }

            ImGui::End();
            // if button closed, don't show the warning again
            if (!show_warning) {
                warn_local_save = false;
                warn_bad_appid = false;
            }
        }
    }

    ImGui::End();

    if (style_color_stack) ImGui::PopStyleColor(style_color_stack);
    ImGui::PopFont();

    if (!show) {
        ShowOverlay(false);
    }

}

}

void Steam_Overlay::load_next_ach_icon()
{
    // this function only works when icons pagination is active, request-based loading is not supported too (pagination=0)
    if (!settings->overlay_upload_achs_icons_to_gpu || settings->paginated_achievements_icons <= 0 || achievements.empty()) return;

    size_t linear_idx = last_loaded_ach_icon / 2; // 2 icons per achievement, 1 achieved, 1 unachieved
    if (linear_idx >= achievements.size()) {
        last_loaded_ach_icon = 0;
        linear_idx = 0;
    }

#ifndef EMU_RELEASE_BUILD
    auto now1 = std::chrono::high_resolution_clock::now();
#endif

    auto &ach = achievements.at(linear_idx);
    ++last_loaded_ach_icon;

    bool achieved = last_loaded_ach_icon % 2 != 0;
    auto &icon_rsrc = achieved ? ach.icon : ach.icon_gray;
    // always force upload to GPU in background-loading mode (pagination > 0)
    bool loaded = try_load_ach_icon(ach, achieved, true);

#ifndef EMU_RELEASE_BUILD
    if (loaded) {
        auto now2 = std::chrono::high_resolution_clock::now();
        auto dd = (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now1).count();
        PRINT_DEBUG("uploaded an achievement icon to GPU in %u ms", dd);
    }
#endif

}

void Steam_Overlay::SetupOverlay()
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    bool not_called_yet = false;
    if (setup_overlay_called.compare_exchange_weak(not_called_yet, true)) {
        // In bridge-only mode (overlay disabled but bridge enabled), skip the
        // native renderer detector — the ReShade addon handles all rendering.
        if (settings->disable_overlay) return;

        if (settings->overlay_hook_delay_sec > 0) {
            PRINT_DEBUG("waiting %i seconds", settings->overlay_hook_delay_sec);
            renderer_detector_delay_thread.start();
        } else {
            // "HITMAN 3" fails if the detector was started later (after a delay)
            // so request the renderer detector immediately (the old behavior)
            request_renderer_detector();
            set_renderer_hook_timeout();
            renderer_hook_init_thread.start();
        }
    }
}

void Steam_Overlay::UnSetupOverlay()
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    bool already_called = true;
    if (setup_overlay_called.compare_exchange_weak(already_called, false)) {
        is_ready = false;

        renderer_hook_init_thread.kill();
        renderer_detector_delay_thread.kill();

        // stop internal frame processing & restore cursor
        if (_renderer) {
            // for some reason this gets triggered after the overlay instance has been destroyed
            // I assume because the game de-initializes DX later after closing Steam APIs
            // this hacky solution just sets it to an empty function
            _renderer->OverlayHookReady = [](InGameOverlay::OverlayHookState){};
            _renderer->OverlayProc = [](){};

            allow_renderer_frame_processing(false, true);
            obscure_game_input(false);

            PRINT_DEBUG("releasing any images resources");
            for (auto &ach : achievements) {
                if (ach.icon->GetResourceId() != 0) {
                    ach.icon->Unload();
                }

                if (ach.icon_gray->GetResourceId() != 0) {
                    ach.icon_gray->Unload();
                }
            }

            // Unload screenshot textures
            for (auto &item : screenshot_items) {
                if (item.texture) {
                    if (item.texture->GetResourceId() != 0)
                        item.texture->Unload();
                    item.texture->Delete();
                }
            }
            screenshot_items.clear();
            preview_pixels.clear();
            preview_pixels_w = 0;
            preview_pixels_h = 0;
            if (preview_texture) {
                if (preview_texture->GetResourceId() != 0)
                    preview_texture->Unload();
                preview_texture->Delete();
                preview_texture = nullptr;
            }
            unpin_all_screenshots();

            // manually calling this dtor looks bad, but it actually prevents a lot of crashes on exit, don't remove it!
            // many DX12 games will crash on exit if the hook wasn't manually removed (ex appid 2933080, 1583230)
            _renderer->~RendererHook_t();
            _renderer = nullptr;
        }

        cleanup_renderer_hook();
    }

    PRINT_DEBUG("done *********");
}

bool Steam_Overlay::Ready() const
{
    // Bridge-only mode: Ready if bridge is connected (it sets is_ready + late_init_imgui)
    if (settings->disable_overlay && settings->enable_overlay_bridge)
        return is_ready && late_init_imgui;
    return !settings->disable_overlay && is_ready && late_init_imgui;
}

bool Steam_Overlay::NeedPresent() const
{
    PRINT_DEBUG_ENTRY();
    return !settings->disable_overlay || settings->enable_overlay_bridge;
}

void Steam_Overlay::SetNotificationPosition(ENotificationPosition eNotificationPosition)
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG("TODO %i", (int)eNotificationPosition);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    notif_position = eNotificationPosition;
}

void Steam_Overlay::SetNotificationInset(int nHorizontalInset, int nVerticalInset)
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG("TODO x=%i y=%i", nHorizontalInset, nVerticalInset);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    h_inset = nHorizontalInset;
    v_inset = nVerticalInset;
}

void Steam_Overlay::OpenOverlayInvite(CSteamID lobbyId)
{
    PRINT_DEBUG("%llu", lobbyId.ConvertToUint64());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    // remember the lobby the game wants the invitations to be sent to,
    // the actual invite happens later when the user clicks "invite" on a friend
    if (lobbyId.IsValid()) {
        invite_lobby_id = lobbyId.ConvertToUint64();
    }
    ShowOverlay(true);
}

void Steam_Overlay::OpenOverlay(const char* pchDialog)
{
    PRINT_DEBUG("TODO '%s'", pchDialog);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    // TODO: Show pages depending on pchDialog
    if ((strncmp(pchDialog, "Friends", sizeof("Friends") - 1) == 0) && (settings->overlayAutoAcceptInvitesCount() > 0)) {
        PRINT_DEBUG("won't open overlay's friends list because some friends are defined in the auto accept list");
        add_auto_accept_invite_notification();
    } else {
        ShowOverlay(true);
    }
}

void Steam_Overlay::OpenOverlayWebpage(const char* pchURL)
{
    PRINT_DEBUG("TODO '%s'", pchURL);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    show_url = pchURL;
    ShowOverlay(true);
}

bool Steam_Overlay::ShowOverlay() const
{
    return show_overlay;
}

void Steam_Overlay::ShowOverlay(bool state)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready() || show_overlay == state) return;

    show_overlay = state;
    overlay_state_changed = true;

    // Refresh display HDR/SDR info and swapchain detection every time the overlay opens.
    // This picks up any SDR white level changes the user may have made in Windows settings
    // (HDR brightness slider) as well as HDR on/off toggles, without requiring a restart.
    if (state) {
        refresh_sdr_white_scale();
        if (_renderer)
            arm_swapchain_format_detect(_renderer);
    }

    // On close: free GPU-side resources so the D3D heap descriptors and VRAM are
    // reclaimed while the game is running without the overlay.
    // CPU-side pixel data (icon_decoded_data) is also cleared so it doesn't sit in
    // RAM until the next open; it will be re-decoded from the settings image cache on demand.
    // The RendererResource_t objects themselves (icon / icon_gray) are kept alive because
    // they own no GPU memory when unloaded — the library releases the underlying texture
    // automatically when the last weak_ptr expires.
    if (!state) {
        show_sce_browser = false;
        sce_textures_free_all();
        for (auto &ach : achievements) {
            ach.icon_decoded_data.clear();
            ach.icon_decoded_data.shrink_to_fit();
            ach.icon_gray_decoded_data.clear();
            ach.icon_gray_decoded_data.shrink_to_fit();
            // Detach the pixel buffer from the resource so the GPU texture is released.
            // The resource object stays alive; next GetResourceId() will re-upload.
            if (ach.icon)      ach.icon->ClearAttachedResource();
            if (ach.icon_gray) ach.icon_gray->ClearAttachedResource();
        }
        last_loaded_ach_icon = 0;
    }

    PRINT_DEBUG("%i", (int)state);

    Steam_Overlay::allow_renderer_frame_processing(state);
    Steam_Overlay::obscure_game_input(state);

}

void Steam_Overlay::SetLobbyInvite(Friend friendId, uint64 lobbyId)
{
    PRINT_DEBUG("%" PRIu64 " %llu", friendId.id(), lobbyId);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    auto i = friends.find(friendId);
    if (i != friends.end())
    {
        // Block lobby invites from cross-app friends
        if (settings->get_local_game_id().AppID() != friendId.appid()) return;

        auto& frd = i->second;
        frd.lobbyId = lobbyId;
        frd.window_state |= window_state_lobby_invite;
        // Make sure don't have rich presence invite and a lobby invite (it should not happen but who knows)
        frd.window_state &= ~window_state_rich_invite;
        
        // Add invite to chat history
        std::string invite_msg = "[INVITE] " + i->first.name() + " invited you to join their lobby";
        frd.chat_history.append(invite_msg).append("\n", 1);
        
        add_invite_notification(*i);
        notify_sound_user_invite(i->second);
    }
}

void Steam_Overlay::SetRichInvite(Friend friendId, const char* connect_str)
{
    PRINT_DEBUG("%" PRIu64 " '%s'", friendId.id(), connect_str);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    auto i = friends.find(friendId);
    if (i != friends.end())
    {
        // Block rich invites from cross-app friends
        if (settings->get_local_game_id().AppID() != friendId.appid()) return;

        auto& frd = i->second;
        strncpy(frd.connect, connect_str, k_cchMaxRichPresenceValueLength - 1);
        frd.window_state |= window_state_rich_invite;
        // Make sure don't have rich presence invite and a lobby invite (it should not happen but who knows)
        frd.window_state &= ~window_state_lobby_invite;
        
        // Add invite to chat history
        std::string invite_msg = "[INVITE] " + i->first.name() + " invited you to join their game";
        frd.chat_history.append(invite_msg).append("\n", 1);
        
        add_invite_notification(*i);
        notify_sound_user_invite(i->second);
    }
}

void Steam_Overlay::FriendConnect(Friend _friend)
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG("%" PRIu64 "", _friend.id());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    // players connections might happen earlier before the overlay is ready
    // we don't want to miss them
    //if (!Ready()) return;

    int id = find_free_friend_id(friends);
    if (id != 0) {
        auto& item = friends[_friend];
        item.window_title = std::move(_friend.name() + " " + translationPlaying[current_language] + " " + std::to_string(_friend.appid()));
        item.window_state = window_state_none;
        item.id = id;
        memset(item.chat_input, 0, max_chat_len);
        item.joinable = false;
    } else {
        PRINT_DEBUG("error no free id to create a friend window");
    }
}

void Steam_Overlay::FriendDisconnect(Friend _friend)
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG("%" PRIu64 "", _friend.id());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    // players connections might happen earlier before the overlay is ready
    // we don't want to miss them
    //if (!Ready()) return;

    auto it = friends.find(_friend);
    if (it != friends.end())
        friends.erase(it);

    // Clean up pending lobby join request tracking for the disconnecting peer
    uint64 disc_id = _friend.id();
    for (auto jit = notified_lobby_join_requests.begin(); jit != notified_lobby_join_requests.end(); ) {
        if (jit->second == disc_id)
            jit = notified_lobby_join_requests.erase(jit);
        else
            ++jit;
    }
    notified_friend_lobbies.erase(disc_id);
}

void Steam_Overlay::FriendUpdate(Friend _friend)
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG("%" PRIu64 " lobby_id=%" PRIu64 "", _friend.id(), _friend.lobby_id());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    // Find existing friend entry by ID
    std::pair<const Friend, friend_window_state>* updated_frd = nullptr;
    for (auto it = friends.begin(); it != friends.end(); ++it) {
        if (it->first.id() == _friend.id()) {
            // Update the Friend key in-place (Friend_Less only compares by id(),
            // so map ordering is preserved — no need to erase and reinsert)
            Friend &frd_ref = const_cast<Friend&>(it->first);
            frd_ref = _friend;
            // Update window title with new appid
            it->second.window_title = _friend.name() + " " + translationPlaying[current_language] + " " + std::to_string(_friend.appid());
            updated_frd = &(*it);
            break;
        }
    }

    if (!updated_frd) return;

    // Check if a same-app friend entered a lobby we haven't notified about yet
    uint64 friend_id = _friend.id();
    uint64 friend_lobby = _friend.lobby_id();
    bool same_app = (settings->get_local_game_id().AppID() == _friend.appid());

    // Skip if we're already in that lobby
    uint64 my_lobby = settings->get_lobby().ConvertToUint64();

    if (same_app && friend_lobby != 0 && friend_lobby != my_lobby) {
        auto tracked = notified_friend_lobbies.find(friend_id);
        if (tracked == notified_friend_lobbies.end() || tracked->second != friend_lobby) {
            // New lobby or different lobby — show notification
            notified_friend_lobbies[friend_id] = friend_lobby;
            std::string msg = _friend.name() + " is in a lobby";
            Notification notif{};
            notif.start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
            notif.steady_start_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
            notif.id = find_free_notification_id(notifications);
            if (notif.id != 0) {
                notif.type = (uint8)notification_type::friend_lobby_available;
                notif.message = msg;
                notif.frd = updated_frd;
                notif.source_friend_id = friend_id;
                notif.join_request_lobby_id = friend_lobby;
                notifications.emplace_back(notif);
                allow_renderer_frame_processing(true);
                obscure_game_input(true);
                notify_sound_friend_lobby();
                PRINT_DEBUG("friend %" PRIu64 " lobby notification for lobby %" PRIu64 "", friend_id, friend_lobby);
            }
        }
    } else {
        // Friend left lobby or different app — clear tracking
        notified_friend_lobbies.erase(friend_id);
    }
}

// show a notification when the user unlocks an achievement
void Steam_Overlay::AddAchievementNotification(const std::string &ach_name, nlohmann::json const &ach, bool for_progress)
{
    if (settings->disable_overlay && !settings->enable_overlay_bridge) return;

    PRINT_DEBUG("'%s' %i", ach_name.c_str(), (int)for_progress);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    // Phase 1: Update achievement data regardless of Ready() state.
    // This keeps the achievement list accurate even if the overlay isn't
    // fully initialized yet (e.g., during the startup window before late_init_imgui).
    for (auto &a : achievements) {
        if (a.name == ach_name) {
            try {
                // lock to prevent modifications to this json object
                std::lock_guard<std::recursive_mutex> lock2(global_mutex);

                a.achieved = ach.value("earned", false);
                a.unlock_time = ach.value("earned_time", static_cast<uint32>(0));
                a.progress = ach.value("progress", static_cast<uint32>(0));
                a.max_progress = ach.value("max_progress", static_cast<uint32>(0));
            } catch(...) {}

            // Phase 2: Only show notification if overlay is ready
            if (!Ready()) return;

            if (a.achieved && !for_progress) { // here we don't show the progress indications
                post_achievement_notification(a, for_progress);
                // sound is now played when notification is actually shown (delayed with queue)
            } else if (for_progress && !settings->disable_overlay_achievement_progress) { // progress indication is shown for locked achievements only
                // post notification if this isn't a progress, or a progress and the user didn't disable these notifications
                post_achievement_notification(a, for_progress);
                notify_sound_achievement_progress();
            }
            break;
        }
    }
}



// -- steam run callbacks --
void Steam_Overlay::steam_run_callback_update_my_lobby()
{
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    Steam_Friends* steamFriends = get_steam_client()->steam_friends;

    // Detect lobby state transitions
    bool had_lobby = i_have_lobby;
    if (std::string(steamFriends->get_friend_rich_presence_silent(settings->get_local_steam_id(), "connect")).length() > 0) {
        i_have_lobby = true;
    } else if (settings->get_lobby().IsValid()) {
        i_have_lobby = true;
    } else {
        i_have_lobby = false;
    }
    if (!had_lobby && i_have_lobby) {
        CSteamID lobby = settings->get_lobby();
        bool is_owner = lobby.IsValid() && get_steam_client()->steam_matchmaking &&
            get_steam_client()->steam_matchmaking->GetLobbyOwner(lobby) == settings->get_local_steam_id();
        if (is_owner) {
            std::string msg = "Lobby created (" + std::to_string(lobby.ConvertToUint64()) + ")";
            if (submit_notification(notification_type::lobby_status, msg)) {
                notify_sound_lobby_status();
            } else {
                pending_lobby_notifications.push_back(msg);
            }
        }
    } else if (had_lobby && !i_have_lobby) {
        if (submit_notification(notification_type::lobby_status, "Lobby closed")) {
            notify_sound_lobby_status();
        } else {
            pending_lobby_notifications.push_back("Lobby closed");
        }
        // Clear lobby chat history
        lobby_chat_history.clear();
        lobby_chat_last_entry_count = 0;
        lobby_chat_input[0] = '\0';
    }

    // Detect game server state transitions
    bool had_server = i_have_game_server;
    Steam_GameServer *gs = get_steam_client()->steam_gameserver;
    bool have_server = gs && gs->BLoggedOn();
    i_have_game_server = have_server;
    if (!had_server && have_server) {
        const auto &sd = gs->get_server_data();
        std::string sname = sd.server_name();
        std::string msg = "Game server started";
        if (!sname.empty()) msg += " (" + sname + ")";
        if (submit_notification(notification_type::lobby_status, msg)) {
            notify_sound_lobby_status();
        } else {
            pending_lobby_notifications.push_back(msg);
        }
    } else if (had_server && !have_server) {
        if (submit_notification(notification_type::lobby_status, "Game server stopped")) {
            notify_sound_lobby_status();
        } else {
            pending_lobby_notifications.push_back("Game server stopped");
        }
    }
}

bool Steam_Overlay::is_friend_joinable(std::pair<const Friend, friend_window_state> &f)
{
    PRINT_DEBUG("%" PRIu64 "", f.first.id());
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    Steam_Friends* steamFriends = get_steam_client()->steam_friends;

    if (std::string(steamFriends->get_friend_rich_presence_silent((uint64)f.first.id(), "connect")).length() > 0 ) {
        PRINT_DEBUG("%" PRIu64 " true (connect string)", f.first.id());
        return true;
    }

    FriendGameInfo_t friend_game_info{};
    steamFriends->GetFriendGamePlayed((uint64)f.first.id(), &friend_game_info);
    if (friend_game_info.m_steamIDLobby.IsValid()) {
        PRINT_DEBUG("%" PRIu64 " true (friend in a lobby)", f.first.id());
        return true;
    }

    PRINT_DEBUG("%" PRIu64 " false", f.first.id());
    return false;
}

void Steam_Overlay::invite_friend(uint64 friend_id, class Steam_Friends* steamFriends, class Steam_Matchmaking* steamMatchmaking)
{
    std::string connect_str = steamFriends->get_friend_rich_presence_silent(settings->get_local_steam_id(), "connect");
    if (connect_str.length() > 0) {
        steamFriends->InviteUserToGame(friend_id, connect_str.c_str());
        PRINT_DEBUG("sent game invitation to friend with id = %llu", friend_id);
    } else {
        // prefer the lobby the game explicitly passed to OpenOverlayInvite(),
        // otherwise fall back to the last joined lobby
        CSteamID lobby_id( invite_lobby_id.load(std::memory_order_relaxed) );
        if (!lobby_id.IsValid()) {
            lobby_id = settings->get_lobby();
        }

        if (lobby_id.IsValid()) {
            steamMatchmaking->InviteUserToLobby(lobby_id, friend_id);
            PRINT_DEBUG("sent lobby invitation (%llu) to friend with id = %llu", lobby_id.ConvertToUint64(), friend_id);
        }
    }
}

void Steam_Overlay::steam_run_callback_friends_actions()
{
    Steam_Friends* steamFriends = get_steam_client()->steam_friends;
    Steam_Matchmaking* steamMatchmaking = get_steam_client()->steam_matchmaking;

    CSteamID my_lobby_action = settings->get_lobby();
    std::for_each(friends.begin(), friends.end(), [this, &my_lobby_action](std::pair<Friend const, friend_window_state> &i) {
        i.second.joinable = is_friend_joinable(i);

        // Auto-clear stale/invalid invites
        if (i.second.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
            bool clear_invite = false;

            // Already in the friend's lobby — no need for the invite
            if (my_lobby_action.IsValid() && my_lobby_action.ConvertToUint64() == i.first.lobby_id()) {
                clear_invite = true;
            }

            // Lobby invite but friend no longer has a lobby
            if ((i.second.window_state & window_state_lobby_invite) && i.first.lobby_id() == 0) {
                clear_invite = true;
            }

            // Rich invite but friend no longer has a connect string
            if (i.second.window_state & window_state_rich_invite) {
                Steam_Friends* sf = get_steam_client()->steam_friends;
                if (sf && std::string(sf->get_friend_rich_presence_silent((uint64)i.first.id(), "connect")).empty()) {
                    clear_invite = true;
                }
            }

            // Friend is no longer joinable at all
            if (!i.second.joinable) {
                clear_invite = true;
            }

            if (clear_invite) {
                i.second.window_state &= ~(window_state_lobby_invite | window_state_rich_invite);
            }
        }
    });

    while (!has_friend_action.empty()) {
        auto friend_info = friends.find(has_friend_action.front());
        if (friend_info != friends.end()) {
            uint64 friend_id = (uint64)friend_info->first.id();
            // The user clicked on "Send"
            if (friend_info->second.window_state & window_state_send_message) {
                char* input = friend_info->second.chat_input;
                char* end_input = input + strlen(input);
                char* printable_char = std::find_if(input, end_input, [](char c) { return std::isgraph(c); });

                // Check if the message contains something else than blanks
                if (printable_char != end_input) {
                    // Handle chat send
                    Common_Message msg;
                    Steam_Messages* steam_messages = new Steam_Messages;
                    steam_messages->set_type(Steam_Messages::FRIEND_CHAT);
                    steam_messages->set_message(friend_info->second.chat_input);
                    msg.set_allocated_steam_messages(steam_messages);
                    msg.set_source_id(settings->get_local_steam_id().ConvertToUint64());
                    msg.set_dest_id(friend_id);
                    network->sendTo(&msg, true, NULL, settings->enable_crossapp_messaging);

                    friend_info->second.chat_history.append(get_steam_client()->settings_client->get_local_name()).append(": ").append(input).append("\n", 1);
                }
                *input = 0; // Reset the input field

                friend_info->second.window_state &= ~window_state_send_message;
            }
            // The user clicked on "Invite" (but invite all wasn't clicked)
            if (friend_info->second.window_state & window_state_invite) {
                // Only allow invite for same-app friends
                if (settings->get_local_game_id().AppID() == friend_info->first.appid()) {
                    invite_friend(friend_id, steamFriends, steamMatchmaking);
                }
                friend_info->second.window_state &= ~window_state_invite;
            }
            // The user clicked on "Join"
            if (friend_info->second.window_state & window_state_join) {
                // Only allow join for same-app friends
                if (settings->get_local_game_id().AppID() != friend_info->first.appid()) {
                    friend_info->second.window_state &= ~(window_state_join | window_state_lobby_invite | window_state_rich_invite);
                } else {
                    std::string connect = steamFriends->get_friend_rich_presence_silent(friend_id, "connect");
                    // The user got a lobby invite and accepted it
                    if (friend_info->second.window_state & window_state_lobby_invite) {
                        GameLobbyJoinRequested_t data;
                        data.m_steamIDLobby.SetFromUint64(friend_info->second.lobbyId);
                        data.m_steamIDFriend.SetFromUint64(friend_id);
                        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));

                        friend_info->second.window_state &= ~window_state_lobby_invite;
                    } else {
                        // The user got a rich presence invite and accepted it
                        if (friend_info->second.window_state & window_state_rich_invite) {
                            GameRichPresenceJoinRequested_t data = {};
                            data.m_steamIDFriend.SetFromUint64(friend_id);
                            strncpy(data.m_rgchConnect, friend_info->second.connect, k_cchMaxRichPresenceValueLength - 1);
                            callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                            
                            friend_info->second.window_state &= ~window_state_rich_invite;
                        } else if (connect.length() > 0) {
                            GameRichPresenceJoinRequested_t data = {};
                            data.m_steamIDFriend.SetFromUint64(friend_id);
                            strncpy(data.m_rgchConnect, connect.c_str(), k_cchMaxRichPresenceValueLength - 1);
                            callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                        }

                        //Not sure about this but it fixes sonic racing transformed invites
                        FriendGameInfo_t friend_game_info = {};
                        steamFriends->GetFriendGamePlayed(friend_id, &friend_game_info);
                        uint64 lobby_id = friend_game_info.m_steamIDLobby.ConvertToUint64();
                        if (lobby_id) {
                            GameLobbyJoinRequested_t data;
                            data.m_steamIDLobby.SetFromUint64(lobby_id);
                            data.m_steamIDFriend.SetFromUint64(friend_id);
                            callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                        }
                    }
                }

                friend_info->second.window_state &= ~window_state_join;
            }
        }
        has_friend_action.pop();
    }

}

void Steam_Overlay::steam_run_callback()
{
    // Track lobby/server state even before overlay is ready so we don't miss transitions
    steam_run_callback_update_my_lobby();

    if (!Ready()) return;

    // Flush any notifications that were queued before the overlay was ready
    if (!pending_lobby_notifications.empty()) {
        for (auto &msg : pending_lobby_notifications) {
            if (submit_notification(notification_type::lobby_status, msg)) {
                notify_sound_lobby_status();
            }
        }
        pending_lobby_notifications.clear();
    }

    if (overlay_state_changed) {
        overlay_state_changed = false;

        if (!settings->disable_overlay_activated_callback) {
            GameOverlayActivated_t data{};
            data.m_bActive = show_overlay;
            data.m_bUserInitiated = true;
            data.m_dwOverlayPID = 123;
            data.m_nAppID = settings->get_local_game_id().AppID();
            callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
        }
    }

    Steam_Friends* steamFriends = get_steam_client()->steam_friends;
    Steam_Matchmaking* steamMatchmaking = get_steam_client()->steam_matchmaking;

    if (save_settings) {
        save_settings = false;

        const char *language_text = valid_languages[current_language];
        save_global_settings(get_steam_client()->local_storage, username_text, language_text);
        get_steam_client()->settings_client->set_local_name(username_text);
        get_steam_client()->settings_server->set_local_name(username_text);
        get_steam_client()->settings_client->set_language(language_text);
        get_steam_client()->settings_server->set_language(language_text);
        steamFriends->resend_friend_data();
    }

    // if variable == true, then set it to false and return true (because state was changed) in that case
    bool yes_clicked = true;
    if (invite_all_friends_clicked.compare_exchange_weak(yes_clicked, false)) {
        PRINT_DEBUG("Steam_Overlay will send invitations to [%zu] friends if they're using the same app", friends.size());
        uint32 current_appid = settings->get_local_game_id().AppID();
        for (auto &fr : friends) {
            if (fr.first.appid() == current_appid) { // friend is playing the same game
                uint64 friend_id = (uint64)fr.first.id();
                invite_friend(friend_id, steamFriends, steamMatchmaking);
            }
        }
    }

    // don't wait to lock the overlay mutex
    // * the overlay proc might be active and holding the overlay mutex
    // * this steam callback will be blocked, but it has the global mutex locked
    // * the overlay proc tries to lock the global mutex, but since we have it, it will be blocked forever
    if (overlay_mutex.try_lock()) {
        if (Ready()) {
            // ==============================================================
            // call steam callbacks that has to change the overlay state here
            // ==============================================================

            steam_run_callback_friends_actions();
            poll_lobby_join_requests();
        }

        overlay_mutex.unlock();
    }
}



// -- steam networking callbacks --
void Steam_Overlay::networking_msg_received(Common_Message *msg)
{
    if (msg->has_steam_messages()) {
        std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

        Friend frd;
        frd.set_id(msg->source_id());
        auto friend_info = friends.find(frd);
        if (friend_info != friends.end()) {
            Steam_Messages const& steam_message = msg->steam_messages();
            // Change color to cyan for friend
            friend_info->second.chat_history.append(friend_info->first.name() + ": " + steam_message.message()).append("\n", 1);

            // Auto-open the friend's chat tab if the chat window is already visible
            if (show_chat) {
                friend_info->second.window_state |= window_state_show;
            }

            if (!(friend_info->second.window_state & window_state_show)) {
                friend_info->second.window_state |= window_state_need_attention;
            }

            add_chat_message_notification(friend_info->first.name() + ": " + steam_message.message(), &(*friend_info));
            notify_sound_chat_message(friend_info->second);
        }
    }
}

// -- Screenshot capture callback --
void Steam_Overlay::on_screenshot_captured(const InGameOverlay::ScreenshotCallbackParameter_t* screenshot, void* userParameter)
{
    auto* self = static_cast<Steam_Overlay*>(userParameter);
    if (!screenshot || !screenshot->Data || screenshot->Width == 0 || screenshot->Height == 0)
        return;

    auto pixels = ScreenshotFormat::ConvertToRGBA(screenshot, 4);
    if (pixels.empty())
        return;

    CapturedScreenshot item;
    item.width = screenshot->Width;
    item.height = screenshot->Height;
    item.pixels_rgb = std::move(pixels);

    std::lock_guard<std::mutex> lock(self->captured_screenshots_mutex);
    self->captured_screenshots_queue.push_back(std::move(item));
}

void Steam_Overlay::process_captured_screenshots()
{
    std::vector<CapturedScreenshot> batch;
    {
        std::lock_guard<std::mutex> lock(captured_screenshots_mutex);
        if (captured_screenshots_queue.empty()) return;
        batch.swap(captured_screenshots_queue);
    }

    for (auto& item : batch) {
        char buff[128];
        auto now = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);
        struct tm local_tm{};
#ifdef _MSC_VER
        localtime_s(&local_tm, &now_time);
#else
        localtime_r(&now_time, &local_tm);
#endif
        size_t written = std::strftime(buff, sizeof(buff), settings->overlay_appearance.screenshot_datetime_format.c_str(), &local_tm);
        if (!written) {
            std::strftime(buff, sizeof(buff), "%Y/%m/%d - %H:%M:%S", &local_tm);
        }
        std::string filename = buff;
        for (char& c : filename) {
            if (c == ':' || c == '/' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
                c = '.';
            }
        }
        filename += ".png";

        if (local_storage->save_screenshot(filename, item.pixels_rgb.data(), item.width, item.height, 4)) {
            PRINT_DEBUG("Screenshot saved: %s", filename.c_str());
            submit_notification(notification_type::screenshot, translationScreenshotSaved[current_language] + filename);
        } else {
            PRINT_DEBUG("Failed to save screenshot!");
        }
    }

    refresh_screenshots_list();
}

// -- Screenshots directory scanning --
void Steam_Overlay::refresh_screenshots_list()
{
    // Unload existing textures
    for (auto& item : screenshot_items) {
        if (item.texture) {
            if (item.texture->GetResourceId() != 0)
                item.texture->Unload();
            item.texture->Delete();
        }
    }
    screenshot_items.clear();

    if (!local_storage->dir_exists(Local_Storage::screenshots_folder)) {
        return;
    }

    std::string path = local_storage->get_path(Local_Storage::screenshots_folder);
    auto filenames = Local_Storage::get_filenames_path(path);

    for (auto& f : filenames) {
        if (f.size() < 4) continue;
        std::string ext = f.substr(f.size() - 4);
        if (ext != ".png" && ext != ".PNG") continue;

        ScreenshotItem item;
        item.filename = f;
        item.full_path = path + PATH_SEPARATOR + f;
        // Read file modification time (and size) so the gallery can sort
        // chronologically and the bridge can report sizes without re-stat'ing.
#ifdef __WINDOWS__
        struct _stat st;
        auto wstat_path = common_helpers::to_wstr(item.full_path);
        if (_wstat(wstat_path.c_str(), &st) == 0) {
            item.mtime = st.st_mtime;
            item.size  = (uint64_t)st.st_size;
        }
#else
        struct stat st;
        if (stat(item.full_path.c_str(), &st) == 0) {
            item.mtime = st.st_mtime;
            item.size  = (uint64_t)st.st_size;
        }
#endif
        if (_renderer)
            item.texture = _renderer->CreateResource();
        screenshot_items.push_back(std::move(item));
    }

    // Sort oldest first so the gallery shows screenshots in chronological order
    std::sort(screenshot_items.begin(), screenshot_items.end(),
              [](const ScreenshotItem& a, const ScreenshotItem& b) {
                  return a.mtime < b.mtime;
              });

    screenshots_loaded = true;
}

// -- Preview popup state cleanup --
// Releases the preview's GPU texture and resets all state flags. Callers are responsible
// for calling ImGui::CloseCurrentPopup() (or doing so in the same scope) if the popup is
// currently open — this helper only tears down the C++ state.
void Steam_Overlay::clear_preview_state()
{
    preview_screenshot_path.clear();
    preview_open_active = false;
    preview_delete_pending = false;
    preview_crop_mode = false;
    preview_crop_rect = ImVec4(0, 0, 0, 0);
    preview_crop_rect_prev = ImVec4(0, 0, 0, 0);
    preview_crop_drag = CropDragState{};
    preview_index = -1;
    if (preview_texture) {
        if (preview_texture->GetResourceId() != 0)
            preview_texture->Unload();
        preview_texture->Delete();
        preview_texture = nullptr;
    }
    preview_pixels.clear();
    preview_pixels_w = 0;
    preview_pixels_h = 0;
}

// -- Gallery window --
void Steam_Overlay::render_gallery_window()
{
    if (!show_screenshots_window) return;

    ImGui::PushFont(font_default, 0.0f);
    uint32 style_color_stack = apply_global_style_color();

    ImGui::SetNextWindowSizeConstraints(ImVec2(400, 300), ImVec2(8192, 8192));
    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (ImGui::Begin(translationScreenshots[current_language], &show_screenshots_window)) {
        // Delete/Unpin toolbar
        bool has_selection = false;
        for (auto& item : screenshot_items) {
            if (item.selected) { has_selection = true; break; }
        }

        if (!pinned_screenshots.empty()) {
            if (ImGui::Button(translationUnpinAll[current_language])) {
                unpin_all_screenshots();
            }
            ImGui::SameLine();
        }

        if (has_selection) {
            if (ImGui::Button(translationDeleteSelected[current_language])) {
                delete_all_selected = true;
                show_delete_confirmation = true;
                delete_confirm_open_active = true;
            }
            ImGui::SameLine();
        }

        if (!screenshot_items.empty()) {
            if (ImGui::Button(translationOpenFolder[current_language])) {
                std::string path = local_storage->get_path(Local_Storage::screenshots_folder);
#ifdef __WINDOWS__
                auto wpath = common_helpers::to_wstr(path);
                ShellExecuteW(NULL, L"open", wpath.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__linux__)
                std::string cmd = "xdg-open \"" + path + "\"";
                // std::system is declared warn_unused_result on glibc: ignoring it is both a
                // warning and a lost diagnostic, so report a failure instead
                if (std::system(cmd.c_str()) != 0) {
                    PRINT_DEBUG("open folder: 'xdg-open' failed for '%s'", path.c_str());
                }
#elif defined(__APPLE__)
                std::string cmd = "open \"" + path + "\"";
                if (std::system(cmd.c_str()) != 0) {
                    PRINT_DEBUG("open folder: 'open' failed for '%s'", path.c_str());
                }
#endif
            }
        }

        ImGui::Separator();

        if (screenshot_items.empty()) {
            if (!screenshots_loaded)
                refresh_screenshots_list();
            if (screenshot_items.empty()) {
                ImGui::TextDisabled("%s", translationNoScreenshotsYet[current_language]);
            }
        }

        // Thumbnail grid — use a fixed-column-count table so items don't
        // slide around when the window is resized.  Each column is sized
        // to exactly one thumbnail + checkbox + small gap.
        if (!screenshot_items.empty()) {
            const float thumb_width  = 160.0f;
            const float thumb_height = 90.0f;
            const float check_w = ImGui::GetFrameHeight()
                                  + ImGui::GetStyle().FramePadding.x * 2.0f;
            const float cell_w = thumb_width + check_w
                                 + ImGui::GetStyle().ItemSpacing.x;

            float avail_x = ImGui::GetContentRegionAvail().x;
            int columns = std::max(1, (int)(avail_x / cell_w));

            if (ImGui::BeginTable("##screenshot_grid", columns,
                    ImGuiTableFlags_SizingFixedFit)) {
                // Each column gets the same fixed width so the grid doesn't
                // reflow on tiny width changes.
                for (int c = 0; c < columns; ++c) {
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, cell_w);
                }
                for (int idx = 0; idx < (int)screenshot_items.size(); ++idx) {
                    ImGui::TableNextColumn();

                    auto& item = screenshot_items[idx];
                    ImGui::PushID(item.filename.c_str());

                // Load thumbnail texture lazily
                if (item.texture && item.texture->GetResourceId() == 0 && !item.failed_to_load) {
                    int img_w = 0, img_h = 0;
                    unsigned char* img = stbi_load(item.full_path.c_str(), &img_w, &img_h, nullptr, 4);
                    if (img) {
                        item.thumbnail_pixels.assign((size_t)thumb_width * (size_t)thumb_height * 4, 0);
                        stbir_resize_uint8_linear(img, img_w, img_h, 0,
                            item.thumbnail_pixels.data(), (int)thumb_width, (int)thumb_height, 0, STBIR_RGBA);
                        item.texture->AttachResource(item.thumbnail_pixels.data(), (uint32_t)thumb_width, (uint32_t)thumb_height);
                        stbi_image_free(img);
                    } else {
                        item.failed_to_load = true;
                    }
                }

                // Thumbnail image button
                if (item.texture && item.texture->GetResourceId() != 0) {
                    if (ImGui::ImageButton("##thumb", item.texture->GetResourceId(), ImVec2(thumb_width, thumb_height))) {
                        preview_index = idx;
                        preview_screenshot_path = item.full_path;
                    }
                } else {
                    ImGui::InvisibleButton("##thumb_placeholder", ImVec2(thumb_width, thumb_height));
                }

                // Right-click context menu
                if (ImGui::BeginPopupContextItem("##screenshot_ctx")) {
                    if (ImGui::Selectable(translationPin[current_language])) {
                        PinnedScreenshot pin;
                        pin.id = next_pin_id++;
                        pin.path = item.full_path;

                        int img_w = 0, img_h = 0;
                        unsigned char* img = stbi_load(item.full_path.c_str(), &img_w, &img_h, nullptr, 4);
                        if (img) {
                            pin.pixels.assign(img, img + ((size_t)img_w * (size_t)img_h * 4));
                            pin.pixels_w = (uint32_t)img_w;
                            pin.pixels_h = (uint32_t)img_h;
                            pin.size = ImVec2((float)img_w, (float)img_h);
                            if (pin.size.x > kContextPinMaxDim || pin.size.y > kContextPinMaxDim) {
                                float scale = std::min(kContextPinMaxDim / pin.size.x,
                                                       kContextPinMaxDim / pin.size.y);
                                pin.size.x *= scale;
                                pin.size.y *= scale;
                            }
                            if (_renderer) {
                                pin.texture = _renderer->CreateResource();
                                pin.texture->AttachResource(pin.pixels.data(), pin.pixels_w, pin.pixels_h);
                            }
                            stbi_image_free(img);
                        }
                        pin.focus_requested = true;
                        pinned_screenshots.push_back(std::move(pin));
                    }
                    if (ImGui::Selectable(translationDelete[current_language])) {
                        single_delete_path = item.full_path;
                        delete_all_selected = false;
                        show_delete_confirmation = true;
                        delete_confirm_open_active = true;
                    }
                    ImGui::EndPopup();
                }

                // Checkbox with text beside it, width limited to thumbnail width
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
                ImGui::Checkbox("##sel", &item.selected);
                ImGui::PopStyleVar();
                ImGui::SameLine(0, 0);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thumb_width);
                // Show filename (without .png extension) below thumbnail
                {
                    std::string label = item.filename;
                    if (label.size() > 4)
                        label.resize(label.size() - 4);
                    ImGui::TextUnformatted(label.c_str());
                }
                ImGui::PopTextWrapPos();

                ImGui::PopID();
            }
            ImGui::EndTable();
            }
        }

        // -- Preview popup --
        if (!preview_screenshot_path.empty() || preview_open_active) {
            // Open the popup on the first frame only (not while navigating via Prev/Next)
            if (!preview_screenshot_path.empty() && !preview_open_active) {
                ImGui::OpenPopup(translationScreenshotPreview[current_language]);
                preview_open_active = true;

                // Find the index for Prev/Next navigation
                if (preview_index < 0 || preview_index >= (int)screenshot_items.size() ||
                    screenshot_items[preview_index].full_path != preview_screenshot_path)
                {
                    preview_index = -1;
                    for (int i = 0; i < (int)screenshot_items.size(); ++i) {
                        if (screenshot_items[i].full_path == preview_screenshot_path) {
                            preview_index = i;
                            break;
                        }
                    }
                }
                // Set initial window size proportional to display
                ImVec2 disp = ImGui::GetIO().DisplaySize;
                ImGui::SetNextWindowSize(ImVec2(disp.x * 0.8f, disp.y * 0.8f), ImGuiCond_Appearing);
            }
            // Removed AlwaysAutoResize so the user can resize the preview window.
            // Center the popup on screen so it never appears at a weird position.
            ImVec2 disp_center = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(disp_center.x * 0.5f, disp_center.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGuiWindowFlags preview_flags = ImGuiWindowFlags_NoScrollbar;
            if (preview_crop_mode) {
                preview_flags |= ImGuiWindowFlags_NoMove;
            }
            bool preview_modal_open = true;
            if (ImGui::BeginPopupModal(translationScreenshotPreview[current_language], &preview_modal_open, preview_flags)) {
                // Navigate to a different screenshot
                // (path changed via Prev/Next → unload current texture so it reloads next frame)
                if (preview_texture && preview_index >= 0 && preview_index < (int)screenshot_items.size() &&
                    screenshot_items[preview_index].full_path != preview_screenshot_path)
                {
                    preview_screenshot_path = screenshot_items[preview_index].full_path;
                    // Reset crop state — old selection no longer applies to new image
                    preview_crop_mode = false;
                    preview_crop_rect = ImVec4(0, 0, 0, 0);
                    preview_crop_rect_prev = ImVec4(0, 0, 0, 0);
                    preview_crop_drag = CropDragState{};
                    if (preview_texture) {
                        if (preview_texture->GetResourceId() != 0)
                            preview_texture->Unload();
                        preview_texture->Delete();
                        preview_texture = nullptr;
                    }
                    preview_pixels.clear();
                    preview_pixels_w = 0;
                    preview_pixels_h = 0;
                }

                // Load full-resolution preview texture (not downscaled — scaling is done
                // at render time so the image fills the user-resizable window)
                if (preview_texture == nullptr) {
                    preview_texture = _renderer ? _renderer->CreateResource() : nullptr;
                }
                if (preview_texture && preview_texture->GetResourceId() == 0 &&
                    preview_index >= 0 && preview_index < (int)screenshot_items.size()) {
                    int img_w = 0, img_h = 0;
                    unsigned char* img = stbi_load(screenshot_items[preview_index].full_path.c_str(), &img_w, &img_h, nullptr, 4);
                    if (img) {
                        preview_pixels.assign(img, img + ((size_t)img_w * (size_t)img_h * 4));
                        preview_pixels_w = (uint32_t)img_w;
                        preview_pixels_h = (uint32_t)img_h;
                        preview_texture->AttachResource(preview_pixels.data(), preview_pixels_w, preview_pixels_h);
                        stbi_image_free(img);
                    }
                }

                // Display image scaled to fit available content, maintaining aspect ratio.
                bool preview_texture_ok = preview_texture && preview_texture->GetResourceId() != 0 && preview_pixels_w > 0 && preview_pixels_h > 0;
                if (preview_texture_ok) {
                    ImVec2 avail = ImGui::GetContentRegionAvail();
                    // Reserve space for the date + button bar at the bottom (only when not in crop mode)
                    if (!preview_crop_mode) {
                        float btn_h = ImGui::GetTextLineHeightWithSpacing() * 3.0f + ImGui::GetStyle().ItemSpacing.y * 4.0f;
                        avail.y -= btn_h;
                    }
                    float scale = std::min(avail.x / (float)preview_pixels_w, avail.y / (float)preview_pixels_h);
                    ImVec2 preview_display_size = ImVec2((float)preview_pixels_w * scale, (float)preview_pixels_h * scale);
                    // Center the image horizontally so portrait images don't leave a gap on the right
                    float off_x = (avail.x - preview_display_size.x) * 0.5f;
                    if (off_x > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off_x);
                    ImVec2 preview_img_pos = ImGui::GetCursorScreenPos();
                    ImGui::Image(preview_texture->GetResourceId(), preview_display_size);

                    // If in crop mode, draw the crop editor over the image. The
                    // Confirm action creates a new pin with preview_crop_rect.
                    // Cancel exits crop mode and restores the previous rect.
                    if (preview_crop_mode) {
                        // (0,0,0,0) = no selection — user will click-and-drag.
                        // Non-zero degenerate rect: clamp to bounds as safety net.
                        if (preview_crop_rect.z <= preview_crop_rect.x
                            || preview_crop_rect.w <= preview_crop_rect.y) {
                            if (preview_crop_rect.x != 0 || preview_crop_rect.y != 0 ||
                                preview_crop_rect.z != 0 || preview_crop_rect.w != 0) {
                                preview_crop_rect.x = std::max(0.0f, std::min((float)preview_pixels_w, preview_crop_rect.x));
                                preview_crop_rect.y = std::max(0.0f, std::min((float)preview_pixels_h, preview_crop_rect.y));
                                preview_crop_rect.z = std::max(preview_crop_rect.x, std::min((float)preview_pixels_w, preview_crop_rect.z));
                                preview_crop_rect.w = std::max(preview_crop_rect.y, std::min((float)preview_pixels_h, preview_crop_rect.w));
                            }
                            // Zero rect (0,0,0,0) left as-is — means "no selection"
                        }
                        CropAction act = render_crop_editor(preview_crop_rect,
                            preview_crop_drag,
                            preview_texture->GetResourceId(),
                            preview_pixels_w, preview_pixels_h,
                            preview_img_pos, preview_display_size);
                        if (act == CropAction::Confirm) {
                            // Create a new pin with the selected crop region
                            PinnedScreenshot pin;
                            pin.id = next_pin_id++;
                            pin.path = screenshot_items[preview_index].full_path;
                            if (preview_pixels_w > 0 && preview_pixels_h > 0) {
                                pin.pixels = preview_pixels;
                                pin.pixels_w = preview_pixels_w;
                                pin.pixels_h = preview_pixels_h;
                            }
                            // Initial size = the crop region (not the full image)
                            float crop_w = std::max(1.0f, preview_crop_rect.z - preview_crop_rect.x);
                            float crop_h = std::max(1.0f, preview_crop_rect.w - preview_crop_rect.y);
                            pin.size = ImVec2(crop_w, crop_h);
                            if (pin.size.x > kContextPinMaxDim || pin.size.y > kContextPinMaxDim) {
                                float ss = std::min(kContextPinMaxDim / pin.size.x,
                                                    kContextPinMaxDim / pin.size.y);
                                pin.size.x *= ss;
                                pin.size.y *= ss;
                            }
                            if (_renderer) {
                                pin.texture = _renderer->CreateResource();
                                if (pin.texture && !pin.pixels.empty())
                                    pin.texture->AttachResource(pin.pixels.data(),
                                        pin.pixels_w, pin.pixels_h);
                            }
                            pin.crop_rect = preview_crop_rect;
                            pin.focus_requested = true;
                            pinned_screenshots.push_back(std::move(pin));

                            // Exit crop mode and close the preview
                            preview_crop_mode = false;
                            ImGui::CloseCurrentPopup();
                            clear_preview_state();
                        } else if (act == CropAction::Cancel) {
                            preview_crop_rect = preview_crop_rect_prev;
                            preview_crop_mode = false;
                        }
                    } else {
                        ImGui::Separator();

                        // Screenshot date — only shown when texture is loaded (avoids flash on navigation)
                        if (!screenshot_items.empty() && preview_index >= 0 && preview_index < (int)screenshot_items.size()) {
                            auto& src_item = screenshot_items[preview_index];
                            if (src_item.mtime > 0) {
                                char time_buf[64];
                                struct tm local_tm{};
#ifdef _MSC_VER
                                localtime_s(&local_tm, &src_item.mtime);
#else
                                localtime_r(&src_item.mtime, &local_tm);
#endif
                                size_t written = std::strftime(time_buf, sizeof(time_buf), settings->overlay_appearance.screenshot_datetime_format.c_str(), &local_tm);
                                if (!written) {
                                    std::strftime(time_buf, sizeof(time_buf), "%Y/%m/%d - %H:%M:%S", &local_tm);
                                }
                                ImGui::TextUnformatted(time_buf);
                            } else {
                                ImGui::TextUnformatted(src_item.filename.c_str());
                            }
                        }

                        // Button bar: < Prev | Pin  Crop  Delete | Next >
                        ImGui::BeginGroup();

                        // Prev — always visible, wraps to last
                        if (ImGui::Button(translationPrev[current_language])) {
                            if (preview_index <= 0)
                                preview_index = (int)screenshot_items.size() - 1;
                            else
                                preview_index--;
                        }
                        ImGui::SameLine();

                        ImGui::Text("|");
                        ImGui::SameLine();

                        if (ImGui::Button(translationPin[current_language])) {
                            PinnedScreenshot pin;
                            pin.id = next_pin_id++;
                            pin.path = screenshot_items[preview_index].full_path;

                            // Repurpose the already-loaded preview pixels to avoid a second stbi_load
                            if (preview_pixels_w > 0 && preview_pixels_h > 0) {
                                pin.pixels = preview_pixels; // copies — small enough for a single frame
                                pin.pixels_w = preview_pixels_w;
                                pin.pixels_h = preview_pixels_h;
                            } else {
                                int img_w = 0, img_h = 0;
                                unsigned char* img = stbi_load(pin.path.c_str(), &img_w, &img_h, nullptr, 4);
                                if (img) {
                                    pin.pixels.assign(img, img + ((size_t)img_w * (size_t)img_h * 4));
                                    pin.pixels_w = (uint32_t)img_w;
                                    pin.pixels_h = (uint32_t)img_h;
                                    stbi_image_free(img);
                                }
                            }

                            // Same initial sizing as context-menu pin (kContextPinMaxDim)
                            pin.size = ImVec2((float)pin.pixels_w, (float)pin.pixels_h);
                            if (pin.size.x > kContextPinMaxDim || pin.size.y > kContextPinMaxDim) {
                                float scale = std::min(kContextPinMaxDim / pin.size.x,
                                                       kContextPinMaxDim / pin.size.y);
                                pin.size.x *= scale;
                                pin.size.y *= scale;
                            }

                            if (_renderer) {
                                pin.texture = _renderer->CreateResource();
                                if (pin.texture && !pin.pixels.empty())
                                    pin.texture->AttachResource(pin.pixels.data(), pin.pixels_w, pin.pixels_h);
                            }
                            pin.focus_requested = true;
                            pinned_screenshots.push_back(std::move(pin));

                            // Leave preview open — pin is immediately visible in its own window
                        }
                        ImGui::SameLine();

                        if (ImGui::Button(translationCrop[current_language])) {
                            preview_crop_rect_prev = preview_crop_rect;
                            preview_crop_mode = true;
                        }
                        ImGui::SameLine();

                        if (!preview_delete_pending && ImGui::Button(translationDelete[current_language])) {
                            preview_delete_pending = true;
                        }
                        ImGui::SameLine();

                        ImGui::Text("|");
                        ImGui::SameLine();

                        // Next — always visible, wraps to first
                        if (ImGui::Button(translationNext[current_language])) {
                            if (preview_index >= (int)screenshot_items.size() - 1)
                                preview_index = 0;
                            else
                                preview_index++;
                        }

                        ImGui::EndGroup();
                    }

                    // Inline delete confirmation (avoids stacking modals which closes the preview)
                    if (preview_delete_pending) {
                        ImGui::Separator();
                        ImGui::Text("%s", translationDeleteThisScreenshot[current_language]);
                        ImGui::SameLine();
                        if (ImGui::Button(translationYes[current_language])) {
                            preview_delete_pending = false;
                            // Perform the delete inline
                            auto& del_item = screenshot_items[preview_index];
                            std::string base_path = local_storage->get_path(Local_Storage::screenshots_folder) + PATH_SEPARATOR;
                            std::string filename;
                            if (del_item.full_path.find(base_path) == 0) {
                                filename = del_item.full_path.substr(base_path.size());
                            } else {
                                auto pos = del_item.full_path.find_last_of("/\\");
                                filename = (pos != std::string::npos) ? del_item.full_path.substr(pos + 1) : del_item.full_path;
                            }
                            if (!filename.empty()) {
                                local_storage->file_delete(Local_Storage::screenshots_folder, filename);
                                std::string json_name = filename.substr(0, filename.size() - 4) + ".json";
                                local_storage->file_delete(Local_Storage::screenshots_folder, json_name);
                            }
                            // Remove pin if the deleted file was pinned
                            for (auto pit = pinned_screenshots.begin(); pit != pinned_screenshots.end(); ) {
                                if (pit->path == del_item.full_path) {
                                    if (pit->texture) {
                                        if (pit->texture->GetResourceId() != 0)
                                            pit->texture->Unload();
                                        pit->texture->Delete();
                                    }
                                    pit = pinned_screenshots.erase(pit);
                                } else {
                                    ++pit;
                                }
                            }
                            // Refresh and advance to next
                            refresh_screenshots_list();
                            if (screenshot_items.empty()) {
                                ImGui::CloseCurrentPopup();
                                clear_preview_state();
                            } else {
                                if (preview_index >= (int)screenshot_items.size())
                                    preview_index = (int)screenshot_items.size() - 1;
                                preview_screenshot_path = screenshot_items[preview_index].full_path;
                                if (preview_texture) {
                                    if (preview_texture->GetResourceId() != 0)
                                        preview_texture->Unload();
                                    preview_texture->Delete();
                                    preview_texture = nullptr;
                                }
                                preview_pixels.clear();
                                preview_pixels_w = 0;
                                preview_pixels_h = 0;
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(translationNo[current_language])) {
                            preview_delete_pending = false;
                        }
                    }
                }

                ImGui::EndPopup();
            } else {
                // BeginPopupModal returned false. Only clear state if the popup is
                // truly closed (X/Escape), not just covered by another window like a new pin.
                if (preview_open_active && !ImGui::IsPopupOpen(translationScreenshotPreview[current_language])) {
                    clear_preview_state();
                }
            }
            // X button on modal title bar → ImGui sets preview_modal_open to false
            if (!preview_modal_open && preview_open_active) {
                clear_preview_state();
            }
        }

        // -- Delete confirmation modal --
        if (show_delete_confirmation || delete_confirm_open_active) {
            if (show_delete_confirmation) {
                ImGui::OpenPopup(translationConfirmDelete[current_language]);
                delete_confirm_open_active = true;
                show_delete_confirmation = false; // consumed - the active flag carries the state
            }
            if (ImGui::BeginPopupModal(translationConfirmDelete[current_language], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                if (delete_all_selected) {
                    ImGui::Text("%s", translationDeleteAllScelectedScreenshots[current_language]);
                } else {
                    ImGui::Text("%s", translationDeleteThisScreenshot[current_language]);
                }
                ImGui::Separator();
                if (ImGui::Button(translationYes[current_language])) {
                    if (delete_all_selected) {
                        // Delete all selected
                        for (auto& item : screenshot_items) {
                            if (!item.selected) continue;
                            local_storage->file_delete(Local_Storage::screenshots_folder, item.filename);
                            // Also delete the .json metadata if it exists
                            std::string json_name = item.filename.substr(0, item.filename.size() - 4) + ".json";
                            local_storage->file_delete(Local_Storage::screenshots_folder, json_name);
                        }
                        refresh_screenshots_list();
                        // Remove pins for files that no longer exist
                        for (auto it = pinned_screenshots.begin(); it != pinned_screenshots.end(); ) {
                            bool exists = false;
                            for (auto& si : screenshot_items) {
                                if (si.full_path == it->path) { exists = true; break; }
                            }
                            if (!exists) {
                                if (it->texture) {
                                    if (it->texture->GetResourceId() != 0)
                                        it->texture->Unload();
                                    it->texture->Delete();
                                }
                                it = pinned_screenshots.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    } else if (!single_delete_path.empty()) {
                        // Find filename from path
                        std::string path = local_storage->get_path(Local_Storage::screenshots_folder) + PATH_SEPARATOR;
                        std::string filename;
                        if (single_delete_path.find(path) == 0) {
                            filename = single_delete_path.substr(path.size());
                        } else {
                            // Try just the basename
                            auto pos = single_delete_path.find_last_of("/\\");
                            filename = (pos != std::string::npos) ? single_delete_path.substr(pos + 1) : single_delete_path;
                        }
                        if (!filename.empty()) {
                            local_storage->file_delete(Local_Storage::screenshots_folder, filename);
                            std::string json_name = filename.substr(0, filename.size() - 4) + ".json";
                            local_storage->file_delete(Local_Storage::screenshots_folder, json_name);
                        }
                        refresh_screenshots_list();
                        // Remove pin if the deleted file was pinned
                        for (auto it = pinned_screenshots.begin(); it != pinned_screenshots.end(); ) {
                            if (it->path == single_delete_path) {
                                if (it->texture) {
                                    if (it->texture->GetResourceId() != 0)
                                        it->texture->Unload();
                                    it->texture->Delete();
                                }
                                it = pinned_screenshots.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                    single_delete_path.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(translationNo[current_language])) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            } else {
                if (delete_confirm_open_active) {
                    // User closed it (Yes or No handled inside the modal). Clear state.
                    show_delete_confirmation = false;
                    single_delete_path.clear();
                    delete_all_selected = false;
                    delete_confirm_open_active = false;
                }
            }
        }
    }
    ImGui::End();

    if (style_color_stack) ImGui::PopStyleColor(style_color_stack);
    ImGui::PopFont();
}

// -- Pin helpers --
void Steam_Overlay::unpin_screenshot(uint64_t id)
{
    for (auto it = pinned_screenshots.begin(); it != pinned_screenshots.end(); ++it) {
        if (it->id == id) {
            if (it->texture) {
                if (it->texture->GetResourceId() != 0)
                    it->texture->Unload();
                it->texture->Delete();
            }
            pinned_screenshots.erase(it);
            return;
        }
    }
}

void Steam_Overlay::unpin_all_screenshots()
{
    for (auto& pin : pinned_screenshots) {
        if (pin.texture) {
            if (pin.texture->GetResourceId() != 0)
                pin.texture->Unload();
            pin.texture->Delete();
        }
    }
    pinned_screenshots.clear();
}

// -- Floating pinned screenshots --

// Render the crop-rectangle editor overlay. The caller renders the FULL
// image with UV (0,0)-(1,1) at img_min/img_size before calling this. This
// function draws:
//   1) a dim layer over the whole image,
//   2) the image AGAIN, clipped to the selection rect, so the selection
//      region looks "un-dimmed" while everything else is darkened,
//   3) a bright border and 8 drag handles,
//   4) a small toolbar with Confirm and Cancel buttons.
// `tex_id` is the texture's GPU resource ID used for step 2.
// `rect` is in source-pixel coordinates and is mutated in place. Returns
// Active until the user clicks Confirm or Cancel.
Steam_Overlay::CropAction Steam_Overlay::render_crop_editor(
    ImVec4& rect, CropDragState& st,
    ImTextureID tex_id,
    uint32_t src_w, uint32_t src_h,
    ImVec2 img_min, ImVec2 img_size)
{
    constexpr float kMinCropPx = 20.0f;
    constexpr float kHandlePx = 8.0f;
    constexpr float kHandleHit = 10.0f;     // generous hit area for handles

    if (src_w == 0 || src_h == 0 || img_size.x <= 0 || img_size.y <= 0)
        return CropAction::Active;

    // Helper: convert source pixel coords -> screen rect
    auto src_to_screen = [&](ImVec4 r) -> ImVec4 {
        float sx = img_size.x / (float)src_w;
        float sy = img_size.y / (float)src_h;
        return ImVec4(img_min.x + r.x * sx,
                      img_min.y + r.y * sy,
                      img_min.x + r.z * sx,
                      img_min.y + r.w * sy);
    };
    // Helper: convert screen point -> source pixel coords
    auto screen_to_src = [&](ImVec2 p) -> ImVec2 {
        float sx = (float)src_w / img_size.x;
        float sy = (float)src_h / img_size.y;
        return ImVec2((p.x - img_min.x) * sx,
                      (p.y - img_min.y) * sy);
    };
    // Helper: clamp rect to image bounds
    auto clamp_rect = [&](ImVec4 r) -> ImVec4 {
        // When clamping x/y to 0, shift z/w by the same delta so the rect
        // keeps its original width/height instead of collapsing to the
        // minimum size.  This prevents a visible "collapse" when the user
        // body-drags the selection off the left or top edge.
        if (r.x < 0) {
            r.z -= r.x;     // r.x is negative, so this adds |r.x| to r.z
            r.x = 0;
        }
        if (r.y < 0) {
            r.w -= r.y;     // r.y is negative, so this adds |r.y| to r.w
            r.y = 0;
        }
        if (r.z > (float)src_w) r.z = (float)src_w;
        if (r.w > (float)src_h) r.w = (float)src_h;
        // Enforce minimum size
        if (r.z - r.x < kMinCropPx) {
            float c = (r.x + r.z) * 0.5f;
            r.x = std::max(0.0f, c - kMinCropPx * 0.5f);
            r.z = std::min((float)src_w, r.x + kMinCropPx);
        }
        if (r.w - r.y < kMinCropPx) {
            float c = (r.y + r.w) * 0.5f;
            r.y = std::max(0.0f, c - kMinCropPx * 0.5f);
            r.w = std::min((float)src_h, r.y + kMinCropPx);
        }
        return r;
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 1) Dim overlay over the whole image
    dl->AddRectFilled(img_min,
                      ImVec2(img_min.x + img_size.x, img_min.y + img_size.y),
                      IM_COL32(0, 0, 0, 140));

    // 2) Selection rect (clamped, drawn in screen coords)
    rect = clamp_rect(rect);
    ImVec4 sel_screen = src_to_screen(rect);
    ImVec2 s0(sel_screen.x, sel_screen.y);
    ImVec2 s1(sel_screen.z, sel_screen.w);

    // Redraw the image in the selection area so the dim layer doesn't
    // cover it. UV is mapped from the source's crop region.
    if (tex_id) {
        ImVec2 uv0(rect.x / (float)src_w, rect.y / (float)src_h);
        ImVec2 uv1(rect.z / (float)src_w, rect.w / (float)src_h);
        dl->AddImage(tex_id, s0, s1, uv0, uv1,
                     IM_COL32(255, 255, 255, 255));
    } else {
        // Fallback: just a translucent white wash so the user can see the
        // selection area is "different" from the dim surroundings.
        dl->AddRectFilled(s0, s1, IM_COL32(255, 255, 255, 30));
    }
    // Border
    dl->AddRect(s0, s1, IM_COL32(255, 255, 255, 255), 0.0f, 2.0f, 0);

    // 3) 8 handles (4 corners + 4 edge midpoints)
    ImVec2 corners[8] = {
        ImVec2(s0.x, s0.y),                 // 0: TL
        ImVec2(s1.x, s0.y),                 // 1: TR
        ImVec2(s1.x, s1.y),                 // 2: BR
        ImVec2(s0.x, s1.y),                 // 3: BL
        ImVec2((s0.x + s1.x) * 0.5f, s0.y), // 4: T
        ImVec2(s1.x, (s0.y + s1.y) * 0.5f), // 5: R
        ImVec2((s0.x + s1.x) * 0.5f, s1.y), // 6: B
        ImVec2(s0.x, (s0.y + s1.y) * 0.5f), // 7: L
    };
    for (int i = 0; i < 8; ++i) {
        dl->AddRectFilled(
            ImVec2(corners[i].x - kHandlePx * 0.5f, corners[i].y - kHandlePx * 0.5f),
            ImVec2(corners[i].x + kHandlePx * 0.5f, corners[i].y + kHandlePx * 0.5f),
            IM_COL32(255, 255, 255, 255));
        dl->AddRect(
            ImVec2(corners[i].x - kHandlePx * 0.5f, corners[i].y - kHandlePx * 0.5f),
            ImVec2(corners[i].x + kHandlePx * 0.5f, corners[i].y + kHandlePx * 0.5f),
            IM_COL32(0, 0, 0, 255), 0.0f, 1.0f, 0);
    }

    // 4) Toolbar (Confirm/Cancel) — rendered directly in the parent window
    //    (no separate child window) to avoid input-routing issues where
    //    clicks on the image get intercepted by the parent window's title
    //    bar or the toolbar steals focus.  The background is drawn first
    //    (via the draw list) so the buttons render on top of it.
    const float tb_fp_x = 6.0f, tb_fp_y = 4.0f;
    const float tb_gap = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 confirm_sz = ImGui::CalcTextSize(translationConfirm[current_language]);
    const ImVec2 cancel_sz  = ImGui::CalcTextSize(translationCancel[current_language]);
    const float tb_w = (confirm_sz.x + tb_fp_x * 2) + tb_gap
                     + (cancel_sz.x  + tb_fp_x * 2);
    const float tb_h = std::max(confirm_sz.y, cancel_sz.y) + tb_fp_y * 2;
    const ImVec2 tb_tl = img_min;
    const ImVec2 tb_br(tb_tl.x + tb_w, tb_tl.y + tb_h);

    // Draw background rounded rect BEFORE buttons so they stack on top
    dl->AddRectFilled(tb_tl, tb_br, IM_COL32(0, 0, 0, 178), 4.0f);

    // Render buttons into the parent window at the toolbar position
    ImGui::SetCursorScreenPos(tb_tl);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(tb_fp_x, tb_fp_y));
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1, 1, 1, 1));

    CropAction action = CropAction::Active;
    if (ImGui::Button(translationConfirm[current_language])) {
        action = CropAction::Confirm;
    }
    ImGui::SameLine();
    if (ImGui::Button(translationCancel[current_language])) {
        action = CropAction::Cancel;
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    ImVec2 toolbar_min = tb_tl;
    ImVec2 toolbar_max = tb_br;

    // 5) Mouse input — skip clicks that fall inside the toolbar so the
    //    Confirm/Cancel buttons aren't treated as crop drags.
    ImVec2 mouse = ImGui::GetMousePos();
    bool mouse_in_toolbar = mouse.x >= toolbar_min.x && mouse.x <= toolbar_max.x
                         && mouse.y >= toolbar_min.y && mouse.y <= toolbar_max.y;
    bool mouse_in_image = !mouse_in_toolbar
                       && mouse.x >= img_min.x && mouse.x <= img_min.x + img_size.x
                       && mouse.y >= img_min.y && mouse.y <= img_min.y + img_size.y;
    bool mouse_down = ImGui::IsMouseDown(0);
    bool mouse_clicked = ImGui::IsMouseClicked(0);

    // Determine which handle (if any) the mouse is over
    int hit_handle = -1;
    if (mouse_in_image) {
        for (int i = 0; i < 8; ++i) {
            if (fabsf(mouse.x - corners[i].x) <= kHandleHit * 0.5f
             && fabsf(mouse.y - corners[i].y) <= kHandleHit * 0.5f) {
                hit_handle = i;
                break;
            }
        }
    }
    st.handle_hover = hit_handle;

    // Cursor feedback based on what's being hovered
    if (st.dragging) {
        if (st.handle == 8) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        else if (st.handle >= 0 && st.handle <= 3) {
            // Diagonal resize cursors
            int diag = (st.handle == 0 || st.handle == 2) ? 0 : 1;
            ImGui::SetMouseCursor(diag == 0 ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
        } else if (st.handle >= 4 && st.handle <= 7) {
            ImGui::SetMouseCursor((st.handle == 4 || st.handle == 6)
                ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
        }
    } else if (hit_handle >= 0) {
        if (hit_handle <= 3) {
            int diag = (hit_handle == 0 || hit_handle == 2) ? 0 : 1;
            ImGui::SetMouseCursor(diag == 0 ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
        } else if (hit_handle >= 4 && hit_handle <= 7) {
            ImGui::SetMouseCursor((hit_handle == 4 || hit_handle == 6)
                ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
        }
    } else if (mouse_in_image && mouse.x >= s0.x && mouse.x <= s1.x
            && mouse.y >= s0.y && mouse.y <= s1.y) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    if (!st.dragging) {
        if (mouse_clicked && mouse_in_image) {
            if (hit_handle >= 0) {
                // Start handle drag
                st.dragging = true;
                st.handle = hit_handle;
                st.start = mouse;
                // Anchor: for corners, opposite corner. For edges, we
                // preserve the opposite edge automatically by only mutating
                // the dragged edge in the drag code below, so anchor isn't
                // needed for edges.
                if (hit_handle == 0) st.anchor = ImVec2(rect.z, rect.w);
                else if (hit_handle == 1) st.anchor = ImVec2(rect.x, rect.w);
                else if (hit_handle == 2) st.anchor = ImVec2(rect.x, rect.y);
                else if (hit_handle == 3) st.anchor = ImVec2(rect.z, rect.y);
            } else if (mouse.x >= s0.x && mouse.x <= s1.x
                    && mouse.y >= s0.y && mouse.y <= s1.y) {
                // Start body move
                st.dragging = true;
                st.handle = 8;
                ImVec2 src_mouse = screen_to_src(mouse);
                st.anchor = ImVec2(src_mouse.x - rect.x, src_mouse.y - rect.y);
            } else {
                // Click outside selection: start a fresh draw anchored at the
                // click. The user drags to define the area.
                ImVec2 src_mouse = screen_to_src(mouse);
                st.dragging = true;
                st.handle = 9;  // 9 = initial-draw mode
                rect = ImVec4(src_mouse.x, src_mouse.y, src_mouse.x, src_mouse.y);
                st.anchor = src_mouse;
            }
        }
    } else {
        // In-progress drag
        if (!mouse_down) {
            st.dragging = false;
            st.handle = -1;
        } else if (st.handle == 8) {
            // Body move
            ImVec2 src_mouse = screen_to_src(mouse);
            float w = rect.z - rect.x;
            float h = rect.w - rect.y;
            float nx = src_mouse.x - st.anchor.x;
            float ny = src_mouse.y - st.anchor.y;
            rect = ImVec4(nx, ny, nx + w, ny + h);
        } else if (st.handle == 9) {
            // Initial draw
            ImVec2 src_mouse = screen_to_src(mouse);
            rect = ImVec4(st.anchor.x, st.anchor.y, src_mouse.x, src_mouse.y);
            // Normalize in case the user drags right-to-left or bottom-to-top
            if (rect.z < rect.x) std::swap(rect.x, rect.z);
            if (rect.w < rect.y) std::swap(rect.y, rect.w);
        } else {
        // Handle drag. Corners (0-3) use the stored anchor; edges (4-7)
        // mutate only the dragged edge and preserve the rest of the rect.
        ImVec2 src_mouse = screen_to_src(mouse);
        float ax = st.anchor.x, ay = st.anchor.y;
        float mx = src_mouse.x, my = src_mouse.y;
        switch (st.handle) {
            case 0: rect = ImVec4(mx, my, ax, ay); break; // TL drag
            case 1: rect = ImVec4(ax, my, mx, ay); break; // TR drag
            case 2: rect = ImVec4(ax, ay, mx, my); break; // BR drag
            case 3: rect = ImVec4(mx, ay, ax, my); break; // BL drag
            case 4: rect = ImVec4(rect.x, my, rect.z, rect.w); break; // T
            case 5: rect = ImVec4(rect.x, rect.y, mx, rect.w); break; // R
            case 6: rect = ImVec4(rect.x, rect.y, rect.z, my); break; // B
            case 7: rect = ImVec4(mx, rect.y, rect.z, rect.w); break; // L
        }
        // Normalize in case the drag inverted the rect
        if (rect.z < rect.x) std::swap(rect.x, rect.z);
        if (rect.w < rect.y) std::swap(rect.y, rect.w);
        }
    }

    rect = clamp_rect(rect);
    return action;
}

void Steam_Overlay::render_pinned_screenshot()
{
    if (pinned_screenshots.empty())
        return;

    ImGui::PushFont(font_default, 0.0f);

    // Track overlay state transitions once — applies to all pin windows.
    static bool prev_overlay_state = false;
    bool overlay_opened = show_overlay && !prev_overlay_state;
    bool overlay_closed = !show_overlay && prev_overlay_state;
    prev_overlay_state = show_overlay;

    // Window-decoration overhead (needed for stable sizing)
    const float pad_x = 2.0f * ImGui::GetStyle().WindowPadding.x;
    const float pad_y = 2.0f * ImGui::GetStyle().WindowPadding.y;
    const float title_bar_h = show_overlay ? ImGui::GetFrameHeight() : 0;
    // Controls (separator + opacity slider) — accurately measured so no gap shows
    const float controls_h = ImGui::GetFrameHeightWithSpacing()        // slider + its trailing spacing
        + ImGui::GetStyle().ItemSpacing.y;                              // spacing above separator

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (!show_overlay) {
        flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove
               | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar
               | ImGuiWindowFlags_NoScrollbar
               | ImGuiWindowFlags_NoBringToFrontOnFocus
               | ImGuiWindowFlags_NoFocusOnAppearing
               | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoNavInputs;
    } else {
        flags |= ImGuiWindowFlags_NoScrollbar;
    }

    // Phase 1: render each pin in its own window
    for (auto& pin : pinned_screenshots) {
        if (!pin.texture || pin.texture->GetResourceId() == 0)
            continue;

        char wnd_id[64];
        snprintf(wnd_id, sizeof(wnd_id), translationPinnedScreenshots[current_language],
                 (unsigned long long)pin.id);

        // Position (deferred until first manual move)
        bool first_frame = !pin.pos_set;

        // Always snap window to image display size when not actively resizing
        // (lets user resize while mouse is held, trims excess on release)
        bool mouse_down = ImGui::IsMouseDown(0);
        if (!mouse_down || first_frame) {
            ImVec2 img = pin.image_disp;
            bool valid = img.x >= 50.0f && img.y >= 30.0f;
            if (!valid && pin.pixels_w > 0 && pin.pixels_h > 0 && pin.size.x > 0 && pin.size.y > 0) {
                // Fallback: derive from pin.size via aspect ratio.
                // If no crop rect set, use the full image dimensions.
                float fw = (float)pin.pixels_w, fh = (float)pin.pixels_h;
                float crop_w = (pin.crop_rect.z > pin.crop_rect.x)
                             ? (pin.crop_rect.z - pin.crop_rect.x) : fw;
                float crop_h = (pin.crop_rect.w > pin.crop_rect.y)
                             ? (pin.crop_rect.w - pin.crop_rect.y) : fh;
                crop_w = std::max(1.0f, crop_w);
                crop_h = std::max(1.0f, crop_h);
                float s = std::min(pin.size.x / crop_w, pin.size.y / crop_h);
                img = ImVec2(crop_w * s, crop_h * s);
                valid = img.x >= 50.0f && img.y >= 30.0f;
            }
            if (!valid) {
                img = ImVec2(std::max(pin.size.x, 50.0f), std::max(pin.size.y, 30.0f));
            }

            // New outer window size after snap
            ImVec2 new_size = show_overlay
                ? ImVec2(img.x + pad_x, img.y + controls_h + pad_y + title_bar_h)
                : ImVec2(img.x + pad_x, img.y + pad_y);

            // Re-center window on the image so both left and right edges
            // adjust toward the image when the resize grip is released.
            // pin.size is the content area from the previous frame; the
            // previous outer size adds the same overhead we just applied.
            if (pin.size.x > 0 && pin.size.y > 0) {
                pin.pos.x += (pin.size.x - img.x) * 0.5f;
                pin.pos.y += (pin.size.y - img.y) * 0.5f;
            }

            ImGui::SetNextWindowPos(pin.pos, ImGuiCond_Always);

            // Hysteresis: only change window size when the difference is
            // meaningful (>0.5px).  The snap+rendering feedback loop
            // (SetNextWindowSize → outer → avail → image_disp → new_size)
            // can accumulate sub-pixel float error frame by frame,
            // causing continuous visible shrinking/growing.  Skipping
            // SetNextWindowSize when the change is negligible locks the
            // window to its current actual size and breaks the loop.
            if (first_frame ||
                pin.last_outer.x == 0.0f || pin.last_outer.y == 0.0f ||
                fabsf(new_size.x - pin.last_outer.x) > 0.5f ||
                fabsf(new_size.y - pin.last_outer.y) > 0.5f) {
                ImGui::SetNextWindowSize(new_size, ImGuiCond_Always);
            }
        } else {
            ImGui::SetNextWindowPos(pin.pos, first_frame
                ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
        }
        pin.pos_set = true;

        ImGui::SetNextWindowSizeConstraints(ImVec2(100, 60), ImVec2(8192, 8192));
        ImGui::SetNextWindowBgAlpha(pin.opacity);

        // In crop mode, prevent the user from accidentally moving/resizing the
        // pin window (window movement during drag-to-select is confusing).
        ImGuiWindowFlags crop_flags = flags;
        if (pin.crop_mode) {
            crop_flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
        }

        if (ImGui::Begin(wnd_id, &pin.open, crop_flags)) {
            // Bring pin to front when requested (new pin, overlay opens)
            if (overlay_opened || pin.focus_requested) {
                ImGui::SetWindowFocus(wnd_id);
                pin.focus_requested = false;
            }

            ImVec2 avail = ImGui::GetContentRegionAvail();
            float image_avail_y = show_overlay ? avail.y - controls_h : avail.y;

            // Normalize crop_rect: (0,0,0,0) = no crop (show full image).
            // Non-zero but inverted rects are clamped to image bounds as a safety net.
            if (pin.crop_rect.z <= pin.crop_rect.x || pin.crop_rect.w <= pin.crop_rect.y) {
                if (pin.crop_rect.x != 0 || pin.crop_rect.y != 0 ||
                    pin.crop_rect.z != 0 || pin.crop_rect.w != 0) {
                    // Non-zero degenerate rect — clamp to bounds
                    pin.crop_rect.x = std::max(0.0f, std::min((float)pin.pixels_w,  pin.crop_rect.x));
                    pin.crop_rect.y = std::max(0.0f, std::min((float)pin.pixels_h,  pin.crop_rect.y));
                    pin.crop_rect.z = std::max(pin.crop_rect.x, std::min((float)pin.pixels_w,  pin.crop_rect.z));
                    pin.crop_rect.w = std::max(pin.crop_rect.y, std::min((float)pin.pixels_h,  pin.crop_rect.w));
                }
                // Zero rect (0,0,0,0) is left as-is — means "no crop"
            }

            // In crop_mode we render the FULL image so the user can see the source
            // to crop from; the dim overlay + selection rect is drawn on top.
            // Otherwise we render only the cropped region via UV mapping.
            // (0,0,0,0) crop_rect = no crop = show full image.
            bool has_crop = pin.crop_rect.z > pin.crop_rect.x
                         && pin.crop_rect.w > pin.crop_rect.y;
            float src_w = (pin.crop_mode || !has_crop) ? (float)pin.pixels_w
                                                       : (pin.crop_rect.z - pin.crop_rect.x);
            float src_h = (pin.crop_mode || !has_crop) ? (float)pin.pixels_h
                                                       : (pin.crop_rect.w - pin.crop_rect.y);
            ImVec2 uv0 = (pin.crop_mode || !has_crop) ? ImVec2(0, 0)
                                                       : ImVec2(pin.crop_rect.x / (float)pin.pixels_w,
                                                                pin.crop_rect.y / (float)pin.pixels_h);
            ImVec2 uv1 = (pin.crop_mode || !has_crop) ? ImVec2(1, 1)
                                                       : ImVec2(pin.crop_rect.z / (float)pin.pixels_w,
                                                                pin.crop_rect.w / (float)pin.pixels_h);

            // Draw image at correct aspect ratio within available space
            ImVec2 img_screen_pos = ImVec2(0, 0);
            ImVec2 img_screen_size = ImVec2(0, 0);
            if (pin.pixels_w > 0 && pin.pixels_h > 0 && avail.x > 0 && image_avail_y > 0
                && src_w > 0 && src_h > 0) {
                float scale = std::min(avail.x / src_w, image_avail_y / src_h);
                float disp_w = src_w * scale;
                float disp_h = src_h * scale;

                // Snap the constrained axis to the exact avail dimension to
                // prevent float-arithmetic drift: src_w * (avail.x / src_w)
                // may not equal avail.x exactly in IEEE 754, causing a tiny
                // sub-pixel error every frame that compounds.
                if (avail.x * src_h <= image_avail_y * src_w)
                    disp_w = avail.x;   // width-constrained
                else
                    disp_h = image_avail_y;  // height-constrained

                float off_x = (avail.x - disp_w) * 0.5f;
                if (off_x > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off_x);

                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                dl->AddImage(pin.texture->GetResourceId(), p0,
                    ImVec2(p0.x + disp_w, p0.y + disp_h),
                    uv0, uv1,
                    IM_COL32(255, 255, 255, (int)(pin.opacity * 255.0f)));
                ImGui::Dummy(ImVec2(disp_w, disp_h));

                // Track actual image display size for closed-state shrink-wrapping
                pin.image_disp = ImVec2(disp_w, disp_h);
                img_screen_pos = p0;
                img_screen_size = ImVec2(disp_w, disp_h);
            }

            if (pin.crop_mode) {
                if (img_screen_size.x > 0 && img_screen_size.y > 0
                    && pin.pixels_w > 0 && pin.pixels_h > 0) {
                    ImTextureID tex = pin.texture ? pin.texture->GetResourceId() : 0;
                    CropAction act = render_crop_editor(pin.crop_rect, pin.crop_drag,
                        tex, pin.pixels_w, pin.pixels_h,
                        img_screen_pos, img_screen_size);
                    if (act == CropAction::Confirm) {
                        // Clamp the final rect to source bounds (clamp_rect is
                        // already called inside render_crop_editor, so this is
                        // a no-op safety net)
                        pin.crop_mode = false;
                        pin.focus_requested = false;
                        // Reset image_disp so the next frame recalculates the
                        // window size from the (now smaller) crop_rect instead
                        // of reusing the last crop-mode full-image size.
                        pin.image_disp = ImVec2(0, 0);
                    } else if (act == CropAction::Cancel) {
                        pin.crop_rect = pin.crop_rect_prev;
                        pin.crop_mode = false;
                    }
                }
            } else if (show_overlay) {
                ImGui::Separator();
                ImGui::SliderFloat(translationOpacity[current_language], &pin.opacity, 0.1f, 1.0f, "%.2f");
                ImGui::SameLine();
                if (ImGui::Button(translationCrop[current_language])) {
                    pin.crop_rect_prev = pin.crop_rect;
                    pin.crop_mode = true;
                }
            }

            if (show_overlay) {
                pin.pos = ImGui::GetWindowPos();
                // Use the actual content region avail (from GetContentRegionAvail)
                // instead of deriving from outer size minus estimated overhead,
                // so there is zero mismatch between what the rendering sees
                // and what the snap uses for sizing and re-centering next frame.
                pin.size = ImVec2(avail.x, image_avail_y);
                if (pin.size.x < 50.0f) pin.size.x = 50.0f;
                if (pin.size.y < 30.0f) pin.size.y = 30.0f;
            } else {
                // Track position and size even with overlay closed so the first
                // overlay-on frame doesn't work with stale values.
                pin.pos = ImGui::GetWindowPos();
                pin.size = ImVec2(avail.x, avail.y);
            }
            // Record the actual window outer size for the next frame's
            // hysteresis comparison.  Must come after the window has been
            // fully sized and positioned by ImGui::Begin+SetNextWindowSize.
            pin.last_outer = ImGui::GetWindowSize();
        }
        ImGui::End();
    }

    // Phase 2: remove any pins whose X button was clicked
    for (auto it = pinned_screenshots.begin(); it != pinned_screenshots.end(); ) {
        if (!it->open) {
            if (it->texture) {
                if (it->texture->GetResourceId() != 0)
                    it->texture->Unload();
                it->texture->Delete();
            }
            it = pinned_screenshots.erase(it);
        } else {
            ++it;
        }
    }

    ImGui::PopFont();
}


/* ══════════════════════════════════════════════════════════════════════════
 *  Bridge accessor methods — called by overlay_bridge.cpp (C ABI exports).
 *  These run on whatever thread the addon calls from (ReShade render thread).
 *  We lock overlay_mutex for thread safety.
 * ══════════════════════════════════════════════════════════════════════════ */

bool Steam_Overlay::Bridge_GetWarnLocalSave() const
{
    return warn_local_save;
}

bool Steam_Overlay::Bridge_GetWarnBadAppId() const
{
    return warn_bad_appid;
}

int Steam_Overlay::Bridge_GetNotifPosition() const
{
    // Map ENotificationPosition to GSE_NotifPosition
    switch (notif_position) {
    case k_EPositionTopLeft:     return GSE_NOTIF_POS_TOP_LEFT;
    case k_EPositionTopRight:    return GSE_NOTIF_POS_TOP_RIGHT;
    case k_EPositionBottomLeft:  return GSE_NOTIF_POS_BOT_LEFT;
    case k_EPositionBottomRight: return GSE_NOTIF_POS_BOT_RIGHT;
    default:                     return GSE_NOTIF_POS_BOT_LEFT;
    }
}

bool Steam_Overlay::Bridge_SetNotifPosition(int gse_pos)
{
    // Inverse of Bridge_GetNotifPosition(). Steam's ENotificationPosition only has
    // the four corners, so the two center values cannot be expressed here — those
    // are available through the per-type options (GSE_OPT_NOTIF_POS_*), which map
    // to Overlay_Appearance::NotificationPosition and do have center variants.
    ENotificationPosition p;
    switch (gse_pos) {
    case GSE_NOTIF_POS_TOP_LEFT:  p = k_EPositionTopLeft; break;
    case GSE_NOTIF_POS_TOP_RIGHT: p = k_EPositionTopRight; break;
    case GSE_NOTIF_POS_BOT_LEFT:  p = k_EPositionBottomLeft; break;
    case GSE_NOTIF_POS_BOT_RIGHT: p = k_EPositionBottomRight; break;
    default: return false;  // TOP_CENTER / BOT_CENTER: not representable
    }

    // Reuse the public Steam-facing setter so the settings check, lock and
    // debug logging all stay in one place.
    SetNotificationPosition(p);
    return true;
}

Steam_Overlay::BridgeStatsSnapshot Steam_Overlay::Bridge_GetStatsState() const
{
    BridgeStatsSnapshot s{};
    s.show_fps       = stats.show_fps;
    s.show_frametime = stats.show_frametime;
    s.show_playtime  = stats.show_playtime;
    s.show_fps_graph       = stats.show_fps_graph;
    s.show_frametime_graph = stats.show_frametime_graph;
    s.show_min_max_avg     = stats.show_min_max_avg;
    s.show_percentile_1    = stats.show_percentile_1;
    s.show_percentile_5    = stats.show_percentile_5;
    s.show_percentile_01   = stats.show_percentile_01;
    s.graph_timeframe_sec  = stats.graph_timeframe_sec;
    // Stats values are updated on the render thread; we read them directly.
    // The addon will compute its own FPS/frametime since it has its own frame loop.
    s.fps = 0;
    s.frametime_ms = 0;
    s.playtime_hr = 0;
    s.playtime_min = 0;
    s.playtime_sec = 0;
    return s;
}

int Steam_Overlay::Bridge_GetAchievementCount() const
{
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(overlay_mutex));
    return static_cast<int>(achievements.size());
}

static void bridge_safe_copy(char *dst, size_t dst_sz, const std::string &src)
{
    if (!dst || !dst_sz) return;
    strncpy(dst, src.c_str(), dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

int Steam_Overlay::Bridge_GetAchievements(GSE_Achievement *out, int max_count)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    // Build a map from achievement API name to group index
    Steam_User_Stats *steamUserStats = get_steam_client()->steam_user_stats;
    std::unordered_map<std::string, int16_t> name_to_group;
    if (steamUserStats && steamUserStats->steamhunters_data_populated) {
        int16_t group_idx = 0;
        for (const auto &grp : steamUserStats->steamhunters_achievement_groups) {
            for (const auto &api_name : grp.achievementApiNames) {
                name_to_group[api_name] = group_idx;
            }
            ++group_idx;
        }
    }

    int count = (std::min)(max_count, static_cast<int>(achievements.size()));
    for (int i = 0; i < count; ++i) {
        auto &a = achievements[i];
        auto &o = out[i];
        memset(&o, 0, sizeof(o));

        bridge_safe_copy(o.name, sizeof(o.name), a.name);
        bridge_safe_copy(o.title, sizeof(o.title), a.title);
        bridge_safe_copy(o.description, sizeof(o.description), a.description);
        o.progress = a.progress;
        o.max_progress = a.max_progress;
        o.unlock_time = a.unlock_time;
        o.hidden = a.hidden ? 1 : 0;
        o.achieved = a.achieved ? 1 : 0;

        // Group index (-1 = ungrouped/base game)
        auto git = name_to_group.find(a.name);
        o.group_index = (git != name_to_group.end()) ? git->second : -1;

        // Global percentage
        auto it = ach_global_percentages.find(a.name);
        o.global_percent = (it != ach_global_percentages.end()) ? it->second : -1.0f;

        // SteamHunters per-achievement data
        o.sh_local_percent = -1.0f;
        o.sh_points = 0;
        o.obtainability = 0;
        o._pad = 0;
        if (steamUserStats && steamUserStats->steamhunters_data_populated) {
            auto sit = steamUserStats->steamhunters_achievement_data.find(a.name);
            if (sit != steamUserStats->steamhunters_achievement_data.end()) {
                o.sh_local_percent = sit->second.localPercentage;
                o.sh_points = static_cast<int16_t>(sit->second.points);
                o.obtainability = static_cast<uint8_t>(sit->second.obtainability);
            }
        }

        // Decode icon pixel data on demand for the bridge.
        // When the ReShade addon is connected, overlay_render_proc() returns early
        // and never calls try_load_ach_icon(), so icon_decoded_data stays empty.
        // We decode from the settings image cache here (CPU-only, no GPU upload).
        auto bridge_ensure_icon = [&](bool achieved) {
            auto &decoded = achieved ? a.icon_decoded_data : a.icon_gray_decoded_data;
            if (!decoded.empty()) return; // already decoded

            int &handle = achieved ? a.icon_handle : a.icon_gray_handle;
            if (Settings::UNLOADED_IMAGE_HANDLE == handle) {
                handle = get_steam_client()->steam_user_stats->get_achievement_icon_handle(a.name, achieved);
            }
            auto *img = settings->get_image(handle);
            if (!img) return;
            int iw = static_cast<int>(img->width);
            int ih = static_cast<int>(img->height);
            if (iw <= 0 || ih <= 0 || img->data.size() < (size_t)iw * ih * 4) return;
            decoded = img->data;
        };
        bridge_ensure_icon(true);
        bridge_ensure_icon(false);

        // Icon pixel data — point directly into decoded data buffers.
        // These are valid until the next call (the emu won't modify them while
        // the bridge is being called, because the render thread is us).
        if (!a.icon_decoded_data.empty()) {
            o.icon_pixels = reinterpret_cast<const uint8_t*>(a.icon_decoded_data.data());
            o.icon_w = 64;  // achievement icons are always 64x64
            o.icon_h = 64;
        }
        if (!a.icon_gray_decoded_data.empty()) {
            o.icon_gray_pixels = reinterpret_cast<const uint8_t*>(a.icon_gray_decoded_data.data());
            o.icon_gray_w = 64;
            o.icon_gray_h = 64;
        }
    }
    return count;
}

int Steam_Overlay::Bridge_HasAchievementGroups()
{
    Steam_User_Stats *steamUserStats = get_steam_client()->steam_user_stats;
    if (!steamUserStats) return 0;
    return (steamUserStats->steamhunters_data_populated
            && !steamUserStats->steamhunters_achievement_groups.empty()) ? 1 : 0;
}

int Steam_Overlay::Bridge_GetAchievementGroupCount()
{
    Steam_User_Stats *steamUserStats = get_steam_client()->steam_user_stats;
    if (!steamUserStats || !steamUserStats->steamhunters_data_populated) return 0;
    return static_cast<int>(steamUserStats->steamhunters_achievement_groups.size());
}

int Steam_Overlay::Bridge_GetAchievementGroups(GSE_AchievementGroup *out, int max_count)
{
    if (!out || max_count <= 0) return 0;

    Steam_User_Stats *steamUserStats = get_steam_client()->steam_user_stats;
    if (!steamUserStats || !steamUserStats->steamhunters_data_populated) return 0;

    const auto &groups = steamUserStats->steamhunters_achievement_groups;
    int count = (std::min)(max_count, static_cast<int>(groups.size()));
    for (int i = 0; i < count; ++i) {
        const auto &g = groups[i];
        auto &o = out[i];
        memset(&o, 0, sizeof(o));

        bridge_safe_copy(o.name, sizeof(o.name), g.name);
        bridge_safe_copy(o.dlc_app_name, sizeof(o.dlc_app_name), g.dlcAppName);
        o.dlc_app_id = g.dlcAppId;
        o.achievement_count = static_cast<int32_t>(g.achievementApiNames.size());
    }
    return count;
}

int Steam_Overlay::Bridge_GetNotifications(GSE_Notification *out, int max_count)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    int written = 0;
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    for (auto &n : notifications) {
        if (n.expired) continue;
        if (written >= max_count) break;

        auto &o = out[written];
        memset(&o, 0, sizeof(o));

        o.id = n.id;
        o.type = n.type;
        o.start_time_ms = n.steady_start_time.count();

        auto duration = get_notification_duration(static_cast<notification_type>(n.type));
        o.duration_ms = duration.count();

        bridge_safe_copy(o.message, sizeof(o.message), n.message);

        // Achievement data if present
        if (n.ach.has_value()) {
            auto &a = n.ach.value();
            bridge_safe_copy(o.ach_title, sizeof(o.ach_title), a.title);
            o.ach_progress = a.progress;
            o.ach_max_progress = a.max_progress;
            o.ach_achieved = a.achieved ? 1 : 0;

            // On-demand icon decode for the bridge (same pattern as Bridge_GetAchievements)
            if (a.icon_decoded_data.empty() && a.icon_handle != Settings::UNLOADED_IMAGE_HANDLE) {
                auto *img = settings->get_image(a.icon_handle);
                if (img && img->width > 0 && img->height > 0 &&
                    img->data.size() >= (size_t)img->width * img->height * 4)
                    a.icon_decoded_data = img->data;
            }
            if (a.icon_decoded_data.empty() && !a.name.empty()) {
                // Try to find the source achievement and decode from its handle
                for (auto &src : achievements) {
                    if (src.name == a.name) {
                        int &h = a.achieved ? src.icon_handle : src.icon_gray_handle;
                        if (Settings::UNLOADED_IMAGE_HANDLE == h)
                            h = get_steam_client()->steam_user_stats->get_achievement_icon_handle(src.name, a.achieved);
                        auto *img = settings->get_image(h);
                        if (img && img->width > 0 && img->height > 0 &&
                            img->data.size() >= (size_t)img->width * img->height * 4)
                            a.icon_decoded_data = img->data;
                        break;
                    }
                }
            }

            if (!a.icon_decoded_data.empty()) {
                o.ach_icon_pixels = reinterpret_cast<const uint8_t*>(a.icon_decoded_data.data());
                o.ach_icon_w = 64;
                o.ach_icon_h = 64;
            }
        }

        // Lobby join request data
        o.join_request_lobby_id = n.join_request_lobby_id;
        o.join_request_requester_id = n.join_request_requester_id;

        // Source friend for invite/message notifications
        if (n.frd) {
            o.source_friend_id = n.frd->first.id();
        } else if (n.source_friend_id) {
            o.source_friend_id = n.source_friend_id;
        }

        ++written;
    }

    // In bridge mode, build_notifications() never runs, so expired notifications
    // would pile up forever. Clean them up here.
    notifications.erase(
        std::remove_if(notifications.begin(), notifications.end(),
            [this](const Notification &item) {
                if (!item.expired) return false;
                // Clean up lobby join request tracking (same as native expiry)
                if ((notification_type)item.type == notification_type::lobby_join_request) {
                    notified_lobby_join_requests.erase({item.join_request_lobby_id, item.join_request_requester_id});
                }
                return true;
            }),
        notifications.end());

    return written;
}

void Steam_Overlay::Bridge_ExpireNotification(int id)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    for (auto &n : notifications) {
        if (n.id == id && !n.expired) {
            // Clean up lobby join request tracking on expiry
            if ((notification_type)n.type == notification_type::lobby_join_request) {
                notified_lobby_join_requests.erase({n.join_request_lobby_id, n.join_request_requester_id});
            }
            n.expired = true;
            allow_renderer_frame_processing(false);
            break;
        }
    }
}

int Steam_Overlay::Bridge_GetDisplayInfo(GSE_DisplayInfo *out, int max_count) const
{
    // Call the static display query function (thread-safe, reads from Windows API)
    auto displays = query_display_hdr_details();
    int count = (std::min)(max_count, static_cast<int>(displays.size()));

    for (int i = 0; i < count; ++i) {
        const auto &d = displays[i];
        auto &o = out[i];
        memset(&o, 0, sizeof(o));

        bridge_safe_copy(o.name, sizeof(o.name), d.name);
        bridge_safe_copy(o.encoding, sizeof(o.encoding), d.encoding);
        bridge_safe_copy(o.gamut, sizeof(o.gamut), d.gamut);
        bridge_safe_copy(o.transfer, sizeof(o.transfer), d.transfer);
        bridge_safe_copy(o.range, sizeof(o.range), d.range);
        o.hdr_supported = d.hdr_supported ? 1 : 0;
        o.hdr_enabled = d.hdr_enabled ? 1 : 0;
        o.wide_color = d.wide_color ? 1 : 0;
        o.force_disabled = d.force_disabled ? 1 : 0;
        o.bpc = d.bpc;
        o.sdr_white_nits = d.sdr_white_nits;
    }
    return count;
}

float Steam_Overlay::Bridge_GetSDRWhiteScale() const
{
    return s_sdr_white_scale;
}

// Helper: format a host-byte-order uint32 IP as dotted-quad (e.g. "192.168.1.1")
static void format_ip_address(uint32 ip, char *buf, size_t len)
{
    if (!buf || len == 0) return;
    if (ip == 0) { buf[0] = '\0'; return; }
    snprintf(buf, len, "%u.%u.%u.%u",
        (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

int Steam_Overlay::Bridge_GetFriendCount() const
{
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(overlay_mutex));
    return static_cast<int>(friends.size());
}

int Steam_Overlay::Bridge_GetFriends(GSE_Friend *out, int max_count) const
{
    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(overlay_mutex));

    uint32 local_appid = settings->get_local_game_id().AppID();

    int written = 0;
    for (const auto &[frd, state] : friends) {
        if (written >= max_count) break;

        auto &o = out[written];
        memset(&o, 0, sizeof(o));

        o.steam_id = frd.id();
        bridge_safe_copy(o.name, sizeof(o.name), frd.name());
        o.is_online = 1; // Friends in this map are online (FriendConnect adds, FriendDisconnect removes)
        o.is_joinable = state.joinable ? 1 : 0;
        o.same_app = (frd.appid() == local_appid) ? 1 : 0;
        o.window_state = state.window_state;
        o.has_pending_invite = (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) ? 1 : 0;
        o.appid = frd.appid();
        o.lobby_id = frd.lobby_id();
        o.in_lobby = (frd.lobby_id() != 0) ? 1 : 0;

        // Check if this friend is actually a member of the local user's lobby
        // (using the lobby member list, which is more reliable than frd.lobby_id())
        CSteamID my_lobby = settings->get_lobby();
        o.in_my_lobby = 0;
        if (my_lobby.IsValid()) {
            Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
            if (mm) {
                int member_count = mm->GetNumLobbyMembers(my_lobby);
                for (int mi = 0; mi < member_count; ++mi) {
                    if (mm->GetLobbyMemberByIndex(my_lobby, mi).ConvertToUint64() == frd.id()) {
                        o.in_my_lobby = 1;
                        break;
                    }
                }
            }
        }

        // Resolve appid → game name
        if (o.appid != 0) {
            auto it = steam_preowned_app_ids.find(o.appid);
            if (it != steam_preowned_app_ids.end()) {
                bridge_safe_copy(o.app_name, sizeof(o.app_name), it->second);
            }
        }

        // Get friend's connect string
        Steam_Friends *steamFriends = get_steam_client()->steam_friends;
        if (steamFriends) {
            std::string connect = steamFriends->get_friend_rich_presence_silent(CSteamID((uint64)frd.id()), "connect");
            if (!connect.empty()) {
                strncpy(o.connect_string, connect.c_str(), GSE_CONNECT_STRING_SIZE - 1);
                o.connect_string[GSE_CONNECT_STRING_SIZE - 1] = '\0';
            }
        }

        // Get friend's lobby details (owner name, member count/limit)
        if (o.in_lobby) {
            // Try local matchmaking data first (available for same-app lobbies)
            Steam_Matchmaking *matchmaking = get_steam_client()->steam_matchmaking;
            if (matchmaking) {
                CSteamID friend_lobby((uint64)frd.lobby_id());
                CSteamID owner = matchmaking->GetLobbyOwner(friend_lobby);
                if (owner.IsValid() && steamFriends) {
                    const char *oname = steamFriends->GetFriendPersonaName(owner);
                    if (oname && oname[0]) {
                        bridge_safe_copy(o.lobby_owner_name, sizeof(o.lobby_owner_name), std::string(oname));
                    }
                }
                o.lobby_member_count = matchmaking->GetNumLobbyMembers(friend_lobby);
                o.lobby_member_limit = matchmaking->GetLobbyMemberLimit(friend_lobby);
            }

            // Fall back to proto fields for cross-app lobbies
            if (o.lobby_member_count == 0 && frd.lobby_member_count() > 0)
                o.lobby_member_count = frd.lobby_member_count();
            if (o.lobby_member_limit == 0 && frd.lobby_member_limit() > 0)
                o.lobby_member_limit = frd.lobby_member_limit();
            if (o.lobby_owner_name[0] == '\0' && frd.lobby_owner_name().size() > 0)
                bridge_safe_copy(o.lobby_owner_name, sizeof(o.lobby_owner_name), frd.lobby_owner_name());
        }

        // Get friend's detected IP addresses from the networking layer
        if (network) {
            uint32 frd_ips[16];
            int frd_ip_count = network->getIPs(CSteamID((uint64)frd.id()), frd_ips, 16);
            o.ip_str[0] = '\0';
            size_t pos = 0;
            for (int k = 0; k < frd_ip_count && pos < sizeof(o.ip_str) - 20; ++k) {
                if (frd_ips[k] == 0) continue;
                if (pos > 0) { o.ip_str[pos++] = ','; o.ip_str[pos++] = ' '; }
                char tmp[24];
                format_ip_address(frd_ips[k], tmp, sizeof(tmp));
                size_t len = strlen(tmp);
                memcpy(o.ip_str + pos, tmp, len);
                pos += len;
            }
            o.ip_str[pos] = '\0';
        }

        ++written;
    }
    return written;
}

int Steam_Overlay::Bridge_HasLobby() const
{
    // Same logic as i_have_lobby in steam_run_callback_update_my_lobby()
    return i_have_lobby ? 1 : 0;
}

int Steam_Overlay::Bridge_GetLocalLobbyInfo(GSE_LocalLobbyInfo *out) const
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    if (!i_have_lobby) return 0;

    CSteamID lobby = settings->get_lobby();
    if (!lobby.IsValid()) return 0;

    Steam_Matchmaking *matchmaking = get_steam_client()->steam_matchmaking;
    if (!matchmaking) return 0;

    out->lobby_id = lobby.ConvertToUint64();
    out->lobby_owner = matchmaking->GetLobbyOwner(lobby).ConvertToUint64();
    out->member_count = matchmaking->GetNumLobbyMembers(lobby);
    out->member_limit = matchmaking->GetLobbyMemberLimit(lobby);
    out->is_owner = (out->lobby_owner == settings->get_local_steam_id().ConvertToUint64()) ? 1 : 0;

    // Also include connect string if set
    Steam_Friends *steamFriends = get_steam_client()->steam_friends;
    if (steamFriends) {
        std::string connect = steamFriends->get_friend_rich_presence_silent(settings->get_local_steam_id(), "connect");
        if (!connect.empty()) {
            strncpy(out->connect_string, connect.c_str(), GSE_CONNECT_STRING_SIZE - 1);
            out->connect_string[GSE_CONNECT_STRING_SIZE - 1] = '\0';
        }
    }

    // Include game exe filename
    {
        std::string full_path = get_full_exe_path();
        std::string::size_type pos = full_path.find_last_of("/\\");
        std::string name = (pos != std::string::npos) ? full_path.substr(pos + 1) : full_path;
        strncpy(out->exe_name, name.c_str(), GSE_EXE_NAME_SIZE - 1);
        out->exe_name[GSE_EXE_NAME_SIZE - 1] = '\0';
    }

    return 1;
}

int Steam_Overlay::Bridge_GetConnectString(char *out, int out_size) const
{
    if (!out || out_size <= 0) return 0;
    out[0] = '\0';

    Steam_Friends *steamFriends = get_steam_client()->steam_friends;
    if (!steamFriends) return 0;

    std::string connect = steamFriends->get_friend_rich_presence_silent(settings->get_local_steam_id(), "connect");
    if (connect.empty()) return 0;

    strncpy(out, connect.c_str(), out_size - 1);
    out[out_size - 1] = '\0';
    return 1;
}

int Steam_Overlay::Bridge_GetExeName(char *out, int out_size) const
{
    if (!out || out_size <= 0) return 0;
    out[0] = '\0';

    std::string full_path = get_full_exe_path();
    std::string::size_type pos = full_path.find_last_of("/\\");
    std::string name = (pos != std::string::npos) ? full_path.substr(pos + 1) : full_path;
    if (name.empty()) return 0;

    strncpy(out, name.c_str(), out_size - 1);
    out[out_size - 1] = '\0';
    return 1;
}

int Steam_Overlay::Bridge_GetGameServerInfo(GSE_GameServerInfo *out) const
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    Steam_GameServer *gs = get_steam_client()->steam_gameserver;
    if (!gs || !gs->BLoggedOn()) return 0;

    const auto &sd = gs->get_server_data();
    out->active = 1;
    out->num_players = sd.num_players();
    out->max_players = sd.max_player_count();
    out->bot_players = sd.bot_player_count();
    bridge_safe_copy(out->server_name, sizeof(out->server_name), sd.server_name());
    bridge_safe_copy(out->map_name, sizeof(out->map_name), sd.map_name());
    return 1;
}

int Steam_Overlay::Bridge_GetLanguage() const
{
    return current_language;
}

void Steam_Overlay::Bridge_RequestSaveSettings()
{
    save_settings = true;
}

// If the addon hasn't called GetState() in this long, consider it disconnected
// and let the native overlay resume rendering (when enabled).
static constexpr int64_t BRIDGE_HEARTBEAT_TIMEOUT_MS = 5000;

static int64_t bridge_now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Steam_Overlay::Bridge_MarkConnected()
{
    auto prev = bridge_last_heartbeat_ms.exchange(bridge_now_ms(), std::memory_order_relaxed);
    if (prev == 0) {
        // First connection — set is_ready and late_init_imgui so Ready() returns true.
        // Needed for bridge-only mode where native hooks don't set these.
        std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
        late_init_imgui.store(true, std::memory_order_relaxed);
        is_ready = true;

        // In bridge-only mode renderer_hook_proc never runs, so achievements
        // and audio were never loaded. Do it now (skips GPU resource creation
        // since _renderer is null).
        if (!_renderer && achievements.empty()) {
            load_achievements_data();
            load_audio();
        }
    }
}

bool Steam_Overlay::Bridge_IsConnected() const
{
    auto last = bridge_last_heartbeat_ms.load(std::memory_order_relaxed);
    if (last == 0) return false;
    return (bridge_now_ms() - last) < BRIDGE_HEARTBEAT_TIMEOUT_MS;
}

void Steam_Overlay::Bridge_TestAchievement()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    show_test_achievement();
}

void Steam_Overlay::Bridge_ResetAchievements()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    if (achievements_snapshot.empty()) return;
    for (auto &ax : achievements) {
        for (const auto &snap : achievements_snapshot) {
            if (snap.name == ax.name) {
                ax.achieved    = snap.achieved;
                ax.progress    = snap.progress;
                ax.unlock_time = snap.unlock_time;
                break;
            }
        }
    }
    ach_global_percentages = ach_global_percentages_snapshot;
}

void Steam_Overlay::Bridge_SimulateAchievements()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint32_t> percent_dist(0, 100);
    std::uniform_real_distribution<float> global_dist(0.5f, 99.9f);
    std::uniform_int_distribution<uint32_t> time_dist(0, 5u * 24u * 3600u);
    const uint32_t now_ts = (uint32_t)std::time(nullptr);

    for (auto &ax : achievements) {
        if (ax.hidden) continue;
        uint32_t roll = percent_dist(rng);
        if (ax.max_progress > 0) {
            if (roll < 40) {
                ax.achieved = true;
                ax.progress = ax.max_progress;
                ax.unlock_time = now_ts - time_dist(rng);
            } else if (roll < 80) {
                ax.achieved = false;
                std::uniform_int_distribution<uint32_t> prog_dist(1, ax.max_progress > 1 ? ax.max_progress - 1 : 1);
                ax.progress = prog_dist(rng);
                ax.unlock_time = 0;
            } else {
                ax.achieved = false;
                ax.progress = 0;
                ax.unlock_time = 0;
            }
        } else {
            // regular achievement: ~40% achieved, ~30% in-progress (fake), ~30% locked
            if (roll < 40) {
                ax.achieved = true;
                ax.unlock_time = now_ts - time_dist(rng);
                ax.max_progress = 0;
                ax.progress = 0;
            } else if (roll < 70) {
                ax.achieved = false;
                ax.unlock_time = 0;
                std::uniform_int_distribution<uint32_t> fake_max(5, 50);
                ax.max_progress = fake_max(rng);
                std::uniform_int_distribution<uint32_t> fake_prog(1, ax.max_progress - 1);
                ax.progress = fake_prog(rng);
            } else {
                ax.achieved = false;
                ax.unlock_time = 0;
                ax.max_progress = 0;
                ax.progress = 0;
            }
        }
        if (ach_global_percentages.find(ax.name) == ach_global_percentages.end())
            ach_global_percentages[ax.name] = global_dist(rng);
    }
}

void Steam_Overlay::Bridge_InviteAllFriends()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready() || !i_have_lobby) return;
    invite_all_friends_clicked = true;
}

void Steam_Overlay::Bridge_FriendAction(uint64_t steam_id, int action)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    for (auto &[frd, state] : friends) {
        if (frd.id() == steam_id) {
            switch (action) {
            case 1: // invite
                if (i_have_lobby) {
                    state.window_state |= window_state_invite;
                    has_friend_action.push(frd);
                }
                break;
            case 2: // join
                if (state.joinable) {
                    state.window_state |= window_state_join;
                    has_friend_action.push(frd);
                }
                break;
            case 3: // copy ID — client-side only (addon handles clipboard), no backend action needed
                break;
            case 4: // chat
                state.window_state |= window_state_show;
                break;
            case 5: // accept invite
                if (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
                    state.window_state |= window_state_join;
                    has_friend_action.push(frd);
                    state.chat_history.append("[INVITE ACCEPTED] You accepted the invite\n");
                }
                break;
            case 6: // refuse invite
                if (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
                    state.window_state &= ~(window_state_lobby_invite | window_state_rich_invite);
                    state.chat_history.append("[INVITE REFUSED] You declined the invite\n");
                }
                break;
            case 7: // kick from lobby
                {
                    CSteamID my_lobby = settings->get_lobby();
                    if (my_lobby.IsValid()) {
                        Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
                        mm->KickLobbyMember(my_lobby.ConvertToUint64(), steam_id);
                    }
                }
                break;
            default: break;
            }
            break;
        }
    }
}

void Steam_Overlay::Bridge_AcceptLobbyJoinRequest(int notification_id)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    for (auto &n : notifications) {
        if (n.id == notification_id && (notification_type)n.type == notification_type::lobby_join_request) {
            Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
            mm->AcceptLobbyJoinRequest(n.join_request_lobby_id, n.join_request_requester_id);
            notified_lobby_join_requests.erase({n.join_request_lobby_id, n.join_request_requester_id});
            n.start_time = {};
            n.expired = true;
            break;
        }
    }
}

void Steam_Overlay::Bridge_DeclineLobbyJoinRequest(int notification_id)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    for (auto &n : notifications) {
        if (n.id == notification_id && (notification_type)n.type == notification_type::lobby_join_request) {
            Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
            mm->DeclineLobbyJoinRequest(n.join_request_lobby_id, n.join_request_requester_id);
            notified_lobby_join_requests.erase({n.join_request_lobby_id, n.join_request_requester_id});
            n.start_time = {};
            n.expired = true;
            break;
        }
    }
}

void Steam_Overlay::Bridge_RequestJoinFriendLobby(int notification_id)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    for (auto &n : notifications) {
        if (n.id == notification_id && (notification_type)n.type == notification_type::friend_lobby_available) {
            Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
            PRINT_DEBUG("requesting join to lobby %" PRIu64 " (friend %" PRIu64 ")", n.join_request_lobby_id, n.source_friend_id);
            mm->JoinLobby(CSteamID((uint64)n.join_request_lobby_id));
            n.start_time = {};
            n.expired = true;
            break;
        }
    }
}

void Steam_Overlay::Bridge_KickAllLobbyMembers()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    CSteamID my_lobby = settings->get_lobby();
    if (!my_lobby.IsValid()) return;

    Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
    mm->KickAllLobbyMembers(my_lobby.ConvertToUint64());
}

void Steam_Overlay::Bridge_LeaveLobby()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    CSteamID my_lobby = settings->get_lobby();
    if (!my_lobby.IsValid()) return;

    Steam_Matchmaking *mm = get_steam_client()->steam_matchmaking;
    mm->LeaveLobby(my_lobby);
}

void Steam_Overlay::Bridge_SetShowFps(bool v)
{
    stats.show_fps = v;
    allow_renderer_frame_processing(v);
}

void Steam_Overlay::Bridge_SetShowFrametime(bool v)
{
    stats.show_frametime = v;
    allow_renderer_frame_processing(v);
}

void Steam_Overlay::Bridge_SetShowPlaytime(bool v)
{
    stats.show_playtime = v;
    allow_renderer_frame_processing(v);
}

void Steam_Overlay::Bridge_SetShowFpsGraph(bool v)       { stats.show_fps_graph = v; }
void Steam_Overlay::Bridge_SetShowFrametimeGraph(bool v) { stats.show_frametime_graph = v; }
void Steam_Overlay::Bridge_SetShowMinMaxAvg(bool v)      { stats.show_min_max_avg = v; }
void Steam_Overlay::Bridge_SetShowPercentile1(bool v)    { stats.show_percentile_1 = v; }
void Steam_Overlay::Bridge_SetShowPercentile5(bool v)    { stats.show_percentile_5 = v; }
void Steam_Overlay::Bridge_SetShowPercentile01(bool v)   { stats.show_percentile_01 = v; }
void Steam_Overlay::Bridge_SetGraphTimeframe(int sec)    { stats.graph_timeframe_sec = (sec >= 1 && sec <= 30) ? sec : 5; }

/* ── Chat support for ReShade addon ─────────────────────────────────────── */

int Steam_Overlay::Bridge_GetChatState(uint64_t steam_id, GSE_ChatState *out)
{
    if (!out) return 0;
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    for (auto &[frd, state] : friends) {
        if (frd.id() == steam_id) {
            out->steam_id = steam_id;
            out->is_open = (state.window_state & window_state_show) ? 1 : 0;
            out->has_pending_invite = (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) ? 1 : 0;
            out->needs_attention = (state.window_state & window_state_need_attention) ? 1 : 0;
            
            // Copy chat history (truncate if needed)
            size_t hist_len = state.chat_history.size();
            if (hist_len >= GSE_CHAT_HISTORY_SIZE) hist_len = GSE_CHAT_HISTORY_SIZE - 1;
            memcpy(out->chat_history, state.chat_history.c_str(), hist_len);
            out->chat_history[hist_len] = '\0';
            
            // Copy window title
            strncpy(out->window_title, state.window_title.c_str(), sizeof(out->window_title) - 1);
            out->window_title[sizeof(out->window_title) - 1] = '\0';
            
            return 1;
        }
    }
    return 0;
}

void Steam_Overlay::Bridge_SendChatMessage(uint64_t steam_id, const char *msg)
{
    if (!msg || !msg[0]) return;
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    for (auto &[frd, state] : friends) {
        if (frd.id() == steam_id) {
            // Copy message to chat input
            strncpy(state.chat_input, msg, max_chat_len - 1);
            state.chat_input[max_chat_len - 1] = '\0';
            
            // Trigger send
            state.window_state |= window_state_send_message;
            has_friend_action.push(frd);
            return;
        }
    }
}

void Steam_Overlay::Bridge_OpenChat(uint64_t steam_id)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    for (auto &[frd, state] : friends) {
        if (frd.id() == steam_id) {
            state.window_state |= window_state_show;
            state.window_state &= ~window_state_need_attention;
            return;
        }
    }
}

void Steam_Overlay::Bridge_CloseChat(uint64_t steam_id)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    for (auto &[frd, state] : friends) {
        if (frd.id() == steam_id) {
            state.window_state &= ~window_state_show;
            return;
        }
    }
}

/* ── Avatar support for ReShade addon ───────────────────────────────────── */

int Steam_Overlay::Bridge_GetAvatar(uint64_t steam_id, GSE_AvatarData *out)
{
    if (!out) return 0;
    out->steam_id = steam_id;
    out->valid = 0;
    
    // Get avatar image handle from Steam Friends
    Steam_Friends *steamFriends = get_steam_client()->steam_friends;
    if (!steamFriends) return 0;
    
    // Get medium avatar (64x64)
    int avatar_handle = steamFriends->GetMediumFriendAvatar(CSteamID((uint64)steam_id));
    if (avatar_handle == 0) return 0;
    
    // Get image RGBA data
    Image_Data *img = settings->get_image(avatar_handle);
    if (!img || img->data.empty()) return 0;
    
    uint32 w = img->width;
    uint32 h = img->height;
    if (w == 0 || h == 0) return 0;
    
    // Copy image data (resize if needed)
    if (w == GSE_AVATAR_SIZE && h == GSE_AVATAR_SIZE) {
        // Perfect size, direct copy
        size_t data_size = (std::min)(img->data.size(), (size_t)GSE_AVATAR_BYTES);
        memcpy(out->pixels, img->data.data(), data_size);
        out->valid = 1;
    } else {
        // Need to resize - for simplicity use nearest neighbor
        // This is a basic resize, consider using stb_image_resize for better quality
        for (int y = 0; y < GSE_AVATAR_SIZE; ++y) {
            for (int x = 0; x < GSE_AVATAR_SIZE; ++x) {
                int src_x = x * w / GSE_AVATAR_SIZE;
                int src_y = y * h / GSE_AVATAR_SIZE;
                size_t src_idx = (src_y * w + src_x) * 4;
                size_t dst_idx = (y * GSE_AVATAR_SIZE + x) * 4;
                if (src_idx + 3 < img->data.size()) {
                    out->pixels[dst_idx + 0] = (uint8_t)img->data[src_idx + 0];
                    out->pixels[dst_idx + 1] = (uint8_t)img->data[src_idx + 1];
                    out->pixels[dst_idx + 2] = (uint8_t)img->data[src_idx + 2];
                    out->pixels[dst_idx + 3] = (uint8_t)img->data[src_idx + 3];
                }
            }
        }
        out->valid = 1;
    }
    
    return out->valid;
}

int Steam_Overlay::Bridge_GetLocalAvatar(GSE_AvatarData *out)
{
    if (!out) return 0;
    
    // Get local user's steam ID
    CSteamID local_id = settings->get_local_steam_id();
    out->steam_id = local_id.IsValid() ? local_id.ConvertToUint64() : 0;
    out->valid = 0;
    
    // Get avatar image handle from Steam Friends
    Steam_Friends *steamFriends = get_steam_client()->steam_friends;
    if (!steamFriends) return 0;
    
    // Get medium avatar for local user (64x64)
    int avatar_handle = steamFriends->GetMediumFriendAvatar(local_id);
    if (avatar_handle == 0) return 0;
    
    // Get image RGBA data
    Image_Data *img = settings->get_image(avatar_handle);
    if (!img || img->data.empty()) return 0;
    
    uint32 w = img->width;
    uint32 h = img->height;
    if (w == 0 || h == 0) return 0;
    
    // Copy/resize image data
    if (w == GSE_AVATAR_SIZE && h == GSE_AVATAR_SIZE) {
        size_t data_size = (std::min)(img->data.size(), (size_t)GSE_AVATAR_BYTES);
        memcpy(out->pixels, img->data.data(), data_size);
        out->valid = 1;
    } else {
        for (int y = 0; y < GSE_AVATAR_SIZE; ++y) {
            for (int x = 0; x < GSE_AVATAR_SIZE; ++x) {
                int src_x = x * w / GSE_AVATAR_SIZE;
                int src_y = y * h / GSE_AVATAR_SIZE;
                size_t src_idx = (src_y * w + src_x) * 4;
                size_t dst_idx = (y * GSE_AVATAR_SIZE + x) * 4;
                if (src_idx + 3 < img->data.size()) {
                    out->pixels[dst_idx + 0] = (uint8_t)img->data[src_idx + 0];
                    out->pixels[dst_idx + 1] = (uint8_t)img->data[src_idx + 1];
                    out->pixels[dst_idx + 2] = (uint8_t)img->data[src_idx + 2];
                    out->pixels[dst_idx + 3] = (uint8_t)img->data[src_idx + 3];
                }
            }
        }
        out->valid = 1;
    }
    
    return out->valid;
}

int Steam_Overlay::Bridge_GetLocalIP(char *out, int max_len) const
{
    if (!out || max_len <= 0) return 0;
    out[0] = '\0';
    if (!network) return 0;
    Networking::AdapterInfo adapters[16];
    int count = network->getAdapters(adapters, 16);
    size_t pos = 0;
    int written = 0;
    for (int i = 0; i < count && pos < (size_t)max_len - 60; ++i) {
        if (adapters[i].ip == 0) continue;
        if (pos > 0) { out[pos++] = '\n'; }
        // Format: "AdapterName: IP"
        size_t name_len = strlen(adapters[i].name);
        if (name_len > 0) {
            memcpy(out + pos, adapters[i].name, name_len);
            pos += name_len;
            out[pos++] = ':';
            out[pos++] = ' ';
        }
        char tmp[24];
        format_ip_address(adapters[i].ip, tmp, sizeof(tmp));
        size_t len = strlen(tmp);
        memcpy(out + pos, tmp, len);
        pos += len;
        ++written;
    }
    out[pos] = '\0';
    return written;
}

int Steam_Overlay::Bridge_GetNetworkInfo(GSE_NetAdapter *out, int max_adapters) const
{
    if (!out || max_adapters <= 0 || !network) return 0;
    memset(out, 0, sizeof(GSE_NetAdapter) * max_adapters);

    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(overlay_mutex));

    // Get adapter info
    Networking::AdapterInfo adapters[16];
    int adapter_count = network->getAdapters(adapters, 16);
    if (adapter_count > max_adapters) adapter_count = max_adapters;

    // Build name→friend map from friends list
    std::unordered_map<uint64, std::string> id_to_name;
    for (const auto &[frd, state] : friends) {
        id_to_name[(uint64)frd.id()] = frd.name();
    }

    // Helper to check if an IP (host byte order) falls in an adapter's subnet
    // lower/upper are stored in network byte order, so convert to host for proper range comparison
    auto ip_in_subnet = [](uint32 ip_host, const Networking::AdapterInfo &a) -> bool {
        if (a.lower == 0 && a.upper == 0) return false;
        uint32 lower_host = ntohl(a.lower);
        uint32 upper_host = ntohl(a.upper);
        return ip_host >= lower_host && ip_host <= upper_host;
    };

    uint64 local_id = settings->get_local_steam_id().ConvertToUint64();
    std::string local_name = settings->get_local_name();

    // Track which friends were assigned to at least one adapter
    std::unordered_set<uint64> assigned;

    for (int i = 0; i < adapter_count; ++i) {
        auto &a = adapters[i];
        auto &o = out[i];

        strncpy(o.name, a.name, sizeof(o.name) - 1);
        format_ip_address(a.ip, o.ip_str, sizeof(o.ip_str));

        // Build CIDR string
        uint32 net_ip = ntohl(a.lower);
        snprintf(o.subnet_str, sizeof(o.subnet_str), "%u.%u.%u.%u/%u",
            (net_ip >> 24) & 0xFF, (net_ip >> 16) & 0xFF, (net_ip >> 8) & 0xFF, net_ip & 0xFF,
            a.prefix_len);

        // Build IP range string
        uint32 upper_ip = ntohl(a.upper);
        snprintf(o.range_str, sizeof(o.range_str), "%u.%u.%u.%u - %u.%u.%u.%u",
            (net_ip >> 24) & 0xFF, (net_ip >> 16) & 0xFF, (net_ip >> 8) & 0xFF, net_ip & 0xFF,
            (upper_ip >> 24) & 0xFF, (upper_ip >> 16) & 0xFF, (upper_ip >> 8) & 0xFF, upper_ip & 0xFF);

        o.user_count = 0;

        // Add ourselves (our adapter IP)
        if (o.user_count < GSE_NET_MAX_USERS_PER_ADAPTER) {
            auto &u = o.users[o.user_count++];
            u.steam_id = local_id;
            strncpy(u.name, local_name.c_str(), sizeof(u.name) - 1);
            format_ip_address(a.ip, u.ip_str, sizeof(u.ip_str));
            u.is_self = 1;
        }

        // Add friends — check ALL known IPs per friend, not just the primary one
        for (const auto &[frd, state] : friends) {
            if (o.user_count >= GSE_NET_MAX_USERS_PER_ADAPTER) break;
            uint32 frd_ips[16];
            int frd_ip_count = network->getIPs(CSteamID((uint64)frd.id()), frd_ips, 16);
            for (int k = 0; k < frd_ip_count; ++k) {
                if (frd_ips[k] != 0 && ip_in_subnet(frd_ips[k], a)) {
                    auto &u = o.users[o.user_count++];
                    u.steam_id = (uint64)frd.id();
                    strncpy(u.name, frd.name().c_str(), sizeof(u.name) - 1);
                    format_ip_address(frd_ips[k], u.ip_str, sizeof(u.ip_str));
                    u.is_self = 0;
                    assigned.insert((uint64)frd.id());
                    break; // one entry per friend per adapter
                }
            }
        }
    }

    // Add an "Other" adapter for friends with no IP matching any adapter subnet
    int result_count = adapter_count;
    bool need_other = false;
    for (const auto &[frd, state] : friends) {
        if (assigned.find((uint64)frd.id()) == assigned.end()) {
            uint32 frd_ip = network->getIP(CSteamID((uint64)frd.id()));
            if (frd_ip != 0) { need_other = true; break; }
        }
    }
    if (need_other && result_count < max_adapters) {
        auto &o = out[result_count];
        strncpy(o.name, "Other", sizeof(o.name) - 1);
        o.ip_str[0] = '\0';
        o.subnet_str[0] = '\0';
        o.user_count = 0;

        for (const auto &[frd, state] : friends) {
            if (o.user_count >= GSE_NET_MAX_USERS_PER_ADAPTER) break;
            if (assigned.find((uint64)frd.id()) != assigned.end()) continue;
            uint32 frd_ip = network->getIP(CSteamID((uint64)frd.id()));
            if (frd_ip != 0) {
                auto &u = o.users[o.user_count++];
                u.steam_id = (uint64)frd.id();
                strncpy(u.name, frd.name().c_str(), sizeof(u.name) - 1);
                format_ip_address(frd_ip, u.ip_str, sizeof(u.ip_str));
                u.is_self = 0;
            }
        }
        result_count++;
    }

    return result_count;
}

int Steam_Overlay::Bridge_GetLobbyChatState(GSE_LobbyChatState *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!i_have_lobby) return 0;

    CSteamID lobby = settings->get_lobby();
    if (!lobby.IsValid()) return 0;

    Steam_Matchmaking *matchmaking = get_steam_client()->steam_matchmaking;
    if (!matchmaking) return 0;

    out->lobby_id = lobby.ConvertToUint64();

    // Fill member list
    int num_members = matchmaking->GetNumLobbyMembers(lobby);
    int member_write = 0;
    for (int i = 0; i < num_members && member_write < GSE_LOBBY_CHAT_MAX_MEMBERS; ++i) {
        CSteamID member_id = matchmaking->GetLobbyMemberByIndex(lobby, i);
        if (!member_id.IsValid()) continue;
        auto &m = out->members[member_write];
        m.steam_id = member_id.ConvertToUint64();

        // Get display name
        if (member_id == settings->get_local_steam_id()) {
            strncpy(m.name, settings->get_local_name(), sizeof(m.name) - 1);
        } else {
            Steam_Friends *steamFriends = get_steam_client()->steam_friends;
            const char *name = steamFriends ? steamFriends->GetFriendPersonaName(member_id) : nullptr;
            if (name && name[0])
                strncpy(m.name, name, sizeof(m.name) - 1);
            else
                snprintf(m.name, sizeof(m.name), "%llu", (unsigned long long)member_id.ConvertToUint64());
        }
        m.name[sizeof(m.name) - 1] = '\0';
        member_write++;
    }
    out->member_count = member_write;

    // Build chat history from chat_entries
    const auto &entries = matchmaking->GetChatEntries();
    size_t entry_count = entries.size();

    // Process new entries since last poll
    if (entry_count > lobby_chat_last_entry_count) {
        Steam_Friends *steamFriends = get_steam_client()->steam_friends;
        for (size_t i = lobby_chat_last_entry_count; i < entry_count; ++i) {
            const auto &e = entries[i];
            if (e.lobby_id != lobby) continue;  // only show current lobby's messages

            // Get sender name
            std::string sender_name;
            if (e.user_id == settings->get_local_steam_id()) {
                sender_name = "You";
            } else {
                const char *name = steamFriends ? steamFriends->GetFriendPersonaName(e.user_id) : nullptr;
                if (name && name[0])
                    sender_name = name;
                else
                    sender_name = std::to_string(e.user_id.ConvertToUint64());
            }

            // Append to history (strip any embedded nulls from the message)
            std::string msg_text = e.message;
            while (!msg_text.empty() && msg_text.back() == '\0') msg_text.pop_back();
            if (!msg_text.empty()) {
                if (!lobby_chat_history.empty())
                    lobby_chat_history += '\n';
                lobby_chat_history += sender_name + ": " + msg_text;
            }
        }
        lobby_chat_last_entry_count = entry_count;
    }

    // Copy history to output
    size_t hist_len = lobby_chat_history.size();
    if (hist_len >= GSE_LOBBY_CHAT_HISTORY_SIZE) hist_len = GSE_LOBBY_CHAT_HISTORY_SIZE - 1;
    memcpy(out->chat_history, lobby_chat_history.c_str() + (lobby_chat_history.size() - hist_len), hist_len);
    out->chat_history[hist_len] = '\0';
    out->history_len = (int32_t)hist_len;

    return 1;
}

void Steam_Overlay::Bridge_SendLobbyChatMsg(const char *msg)
{
    if (!msg || !msg[0]) return;
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!i_have_lobby) return;

    CSteamID lobby = settings->get_lobby();
    if (!lobby.IsValid()) return;

    Steam_Matchmaking *matchmaking = get_steam_client()->steam_matchmaking;
    if (!matchmaking) return;

    size_t len = strlen(msg);
    if (len > 4000) len = 4000;  // Steam's 4KB limit
    matchmaking->SendLobbyChatMsg(lobby, msg, (int)len + 1);
}

// ── Screenshots (ABI v16) ───────────────────────────────────────────────
//
// The bridge hands out on-disk paths; the client decodes and uploads them
// itself. That keeps per-frame ABI traffic tiny and lets the client pick its
// own thumbnail resolution.

// Stable 64-bit id for a screenshot path (FNV-1a). Never returns 0 so the
// client can use 0 as "invalid".
static uint64_t bridge_screenshot_id(const std::string &path)
{
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : path) {
        h ^= (uint64_t)c;
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

int Steam_Overlay::Bridge_IsScreenshotSupported() const
{
    // Capture goes through the emu's own renderer hook, which only exists when
    // the native overlay path is active. In bridge-only mode there is no
    // renderer to grab a back buffer from, so report unsupported.
    return _renderer ? 1 : 0;
}

void Steam_Overlay::Bridge_TakeScreenshot()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!_renderer) return;

    // BeforeOverlay == the game frame without our overlay drawn on top, which is
    // what the native F12 hotkey uses.
    _renderer->TakeScreenshot(InGameOverlay::ScreenshotType_t::BeforeOverlay);
    PRINT_DEBUG("bridge: screenshot requested");
}

int Steam_Overlay::Bridge_GetScreenshotCount()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!screenshots_loaded)
        refresh_screenshots_list();
    return (int)screenshot_items.size();
}

int Steam_Overlay::Bridge_GetScreenshots(GSE_ScreenshotInfo *out, int max_count)
{
    if (!out || max_count <= 0) return 0;

    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!screenshots_loaded)
        refresh_screenshots_list();

    int written = 0;
    for (auto &item : screenshot_items) {
        if (written >= max_count) break;

        auto &o = out[written];
        memset(&o, 0, sizeof(o));
        o.id         = bridge_screenshot_id(item.full_path);
        o.mtime      = (int64_t)item.mtime;
        o.size_bytes = item.size;
        bridge_safe_copy(o.filename, sizeof(o.filename), item.filename);
        bridge_safe_copy(o.full_path, sizeof(o.full_path), item.full_path);
        ++written;
    }
    return written;
}

int Steam_Overlay::Bridge_DeleteScreenshot(uint64_t id)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!screenshots_loaded)
        refresh_screenshots_list();

    // Resolve the id first: refresh_screenshots_list() invalidates all iterators.
    std::string victim_path, victim_name;
    for (auto &item : screenshot_items) {
        if (bridge_screenshot_id(item.full_path) == id) {
            victim_path = item.full_path;
            victim_name = item.filename;
            break;
        }
    }
    if (victim_path.empty()) return 0;

    // Release GPU resources. These only exist when the native overlay owns the
    // renderer; in bridge mode item.texture is always null.
    for (auto &item : screenshot_items) {
        if (item.texture && item.full_path == victim_path) {
            if (item.texture->GetResourceId() != 0) item.texture->Unload();
            item.texture->Delete();
            item.texture = nullptr;
        }
    }
    // A pin holds its own texture for the same file — release that too, exactly
    // like the native gallery does when it deletes a pinned screenshot.
    for (auto pit = pinned_screenshots.begin(); pit != pinned_screenshots.end(); ) {
        if (pit->path == victim_path) {
            if (pit->texture) {
                if (pit->texture->GetResourceId() != 0) pit->texture->Unload();
                pit->texture->Delete();
            }
            pit = pinned_screenshots.erase(pit);
        } else {
            ++pit;
        }
    }

    // Remove the same two files the native gallery removes: the image and its
    // .json sidecar (screenshot metadata).
    local_storage->file_delete(Local_Storage::screenshots_folder, victim_name);
    if (victim_name.size() > 4) {
        std::string json_name = victim_name.substr(0, victim_name.size() - 4) + ".json";
        local_storage->file_delete(Local_Storage::screenshots_folder, json_name);
    }

    // Rescan so the caller sees the updated list on its very next query.
    screenshots_loaded = false;
    refresh_screenshots_list();
    return 1;
}

int Steam_Overlay::Bridge_GetScreenshotsFolder(char *out, int out_size)
{
    if (!out || out_size <= 0) return 0;
    out[0] = '\0';

    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    std::string path = local_storage->get_path(Local_Storage::screenshots_folder);
    if (path.empty()) return 0;

    bridge_safe_copy(out, (size_t)out_size, path);
    return 1;
}

// ── Notification history (ABI v16) ──────────────────────────────────────

int Steam_Overlay::Bridge_GetNotificationHistoryCount()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    return (int)notification_history.size();
}

int Steam_Overlay::Bridge_GetNotificationHistory(GSE_NotificationHistoryEntry *out, int max_count)
{
    if (!out || max_count <= 0) return 0;

    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    // Newest first — matches the order the native history panel renders in.
    int written = 0;
    for (auto it = notification_history.rbegin(); it != notification_history.rend(); ++it) {
        if (written >= max_count) break;

        auto &o = out[written];
        memset(&o, 0, sizeof(o));
        o.timestamp_ms = it->timestamp.count();
        o.type         = it->type;
        bridge_safe_copy(o.message, sizeof(o.message), it->message);
        ++written;
    }
    return written;
}

void Steam_Overlay::Bridge_ClearNotificationHistory()
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    // Mirrors the native "Clear All" button, including dropping the formatted cache.
    notification_history.clear();
    notification_history_cache.clear();
    notification_history_cache_dirty = false;
}

// ── Toggle hotkey (ABI v17) ─────────────────────────────────────────────

int Steam_Overlay::Bridge_GetToggleKeys(GSE_ToggleKeyInfo *out) const
{
    if (!out) return 0;

    std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(overlay_mutex));

    memset(out, 0, sizeof(*out));

    // `vk` carries Windows virtual-key codes and is only meaningful there: the
    // ReShade addon that reads it is Windows-only. `count` and `label` are
    // platform-neutral, so the configured combo is reported correctly everywhere and
    // only `vk` degrades to zero elsewhere. Deciding whether a key is usable must
    // therefore not depend on `vk`, or Linux would always fall through to the default.
    std::string label;
    for (auto key : toggle_keys) {
        if (out->count >= GSE_MAX_TOGGLE_KEYS) break;

        out->vk[out->count++] = toggle_key_to_vk(key);

        if (!label.empty()) label += " + ";
        label += toggle_key_name(key);
    }

    // Nothing usable came through (misconfigured combo). Report the same default
    // the native overlay falls back to so the client always has a working combo.
    if (out->count == 0) {
        out->vk[out->count++] = toggle_key_to_vk(InGameOverlay::ToggleKey::SHIFT);
        out->vk[out->count++] = toggle_key_to_vk(InGameOverlay::ToggleKey::TAB);
        label = "SHIFT + TAB";
    }

    bridge_safe_copy(out->label, sizeof(out->label), label);
    return 1;
}

// ── Identity / localisation (ABI v17) ───────────────────────────────────

int Steam_Overlay::Bridge_GetLanguageCount() const
{
    return (int)(sizeof(valid_languages) / sizeof(valid_languages[0]));
}

int Steam_Overlay::Bridge_GetLanguageName(int index, char *out, int out_size) const
{
    if (!out || out_size <= 0) return 0;
    out[0] = '\0';

    const int count = (int)(sizeof(valid_languages) / sizeof(valid_languages[0]));
    if (index < 0 || index >= count) return 0;

    bridge_safe_copy(out, (size_t)out_size, std::string(valid_languages[index]));
    return 1;
}

int Steam_Overlay::Bridge_SetLanguageIndex(int index)
{
    const int count = (int)(sizeof(valid_languages) / sizeof(valid_languages[0]));
    if (index < 0 || index >= count) return 0;

    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    // Same field the native settings window's language ListBox writes.
    // Persisted by Bridge_RequestSaveSettings().
    current_language = index;
    return 1;
}

int Steam_Overlay::Bridge_SetUsername(const char *name)
{
    if (!name) return 0;

    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    // Same buffer the native settings window's username InputText edits.
    // Persisted by Bridge_RequestSaveSettings().
    strncpy(username_text, name, sizeof(username_text) - 1);
    username_text[sizeof(username_text) - 1] = '\0';
    return 1;
}

#endif
