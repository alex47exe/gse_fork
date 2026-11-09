#ifndef __INCLUDED_STEAM_OVERLAY_H__
#define __INCLUDED_STEAM_OVERLAY_H__

#include "dll/base.h"
#include <map>
#include <queue>
#include <deque>

#ifdef EMU_OVERLAY

#include <future>
#include <atomic>
#include <chrono>
#include <memory>
#include "dll/playtime.h"
#include "InGameOverlay/RendererHook.h"
#include "InGameOverlay/ImGui/imgui.h"
#include "overlay/steam_overlay_stats.h"

static constexpr size_t max_chat_len = 768;

enum window_state
{
    window_state_none           = 0,
    window_state_show           = 1<<0,
    window_state_invite         = 1<<1,
    window_state_join           = 1<<2,
    window_state_lobby_invite   = 1<<3,
    window_state_rich_invite    = 1<<4,
    window_state_send_message   = 1<<5,
    window_state_need_attention = 1<<6,

};

struct friend_window_state
{
    int id;
    uint8 window_state;
    std::string window_title;
    union // The invitation (if any)
    {
        uint64 lobbyId;
        char connect[k_cchMaxRichPresenceValueLength];
    };
    std::string chat_history;
    char chat_input[max_chat_len];

    bool joinable;
    
    // Avatar texture (lazy loaded)
    InGameOverlay::RendererResource_t* avatar_resource{nullptr};
    std::string avatar_pixels{};  // kept alive for AttachResource
    int avatar_handle{-1};        // Settings image handle, -1 = not loaded
};

struct Friend_Less
{
    bool operator()(const Friend& lhs, const Friend& rhs) const
    {
        return lhs.id() < rhs.id();
    }
};

enum class notification_type
{
    message,
    invite,
    achievement,
    achievement_progress,
    auto_accept_invite,
    lobby_join_request,
    lobby_join_request_response,
    lobby_kicked,
    friend_lobby_available,
    lobby_status,
    screenshot,
};

struct Overlay_Achievement
{
    std::string name{}; // internal schema name
    std::string title{}; // displayName
    std::string description{}; // description
    uint32 progress{};
    uint32 max_progress{};
    bool hidden{};
    bool achieved{};
    uint32 unlock_time{};
    float unlock_percentage = -1.0f; // from achievements.json, -1 = unknown
    InGameOverlay::RendererResource_t* icon{};
    InGameOverlay::RendererResource_t* icon_gray{};
    int icon_handle = Settings::UNLOADED_IMAGE_HANDLE;
    int icon_gray_handle = Settings::UNLOADED_IMAGE_HANDLE;
    std::string icon_decoded_data{};
    std::string icon_gray_decoded_data{};
};

struct Notification
{
    static constexpr float width_percent = 0.25f; // percentage from total width
    static constexpr std::chrono::milliseconds default_show_time = std::chrono::milliseconds(6000);

    int id{};
    uint8 type{};
    bool expired = false;
    std::chrono::milliseconds start_time{};        // system_clock (used by native overlay)
    std::chrono::milliseconds steady_start_time{};  // steady_clock (used by bridge/addon)
    std::string message{};
    std::pair<const Friend, friend_window_state>* frd{};
    std::optional<Overlay_Achievement> ach{};
    // For lobby_join_request notifications
    uint64 join_request_lobby_id{};
    uint64 join_request_requester_id{};
    // Source friend ID for notifications that involve a specific friend
    uint64 source_friend_id{};
    // Cached rendered size from previous frame (for stacking calculations)
    mutable float last_width{};
    mutable float last_height{};
};

// Lightweight archive entry -- no pointers, no GPU resources, safe to store long-term
struct NotificationHistoryEntry
{
    std::chrono::milliseconds timestamp{};
    uint8 type{};
    std::string message{};
};

// notification coordinates { x, y }
struct NotificationsCoords
{
    std::pair<float, float> top_left{}, top_center{}, top_right{};
    std::pair<float, float> bot_left{}, bot_center{}, bot_right{};
};

class Steam_Overlay
{
    constexpr static const char ACH_SOUNDS_FOLDER[] = "sounds";
    constexpr static const int renderer_detector_polling_ms = 100;

    class Settings* settings;
    class Local_Storage* local_storage;
    class SteamCallResults* callback_results;
    class SteamCallBacks* callbacks;
    class RunEveryRunCB* run_every_runcb;
    class Networking* network;
    class PlaytimeCounter* playtime_counter;
    class Steam_Overlay_Stats stats;

    // friend id, show client window (to chat and accept invite maybe)
    std::map<Friend, friend_window_state, Friend_Less> friends{};

    bool is_ready = false;

    ENotificationPosition notif_position = ENotificationPosition::k_EPositionBottomLeft;
    int h_inset = 0;
    int v_inset = 0;
    std::string show_url{};

    std::vector<Overlay_Achievement> achievements{};
    std::map<std::string, float> ach_global_percentages{}; // fetched from Steam Web API
    size_t last_loaded_ach_icon{};

    // snapshot of original achievement data for the "Reset" button (overlay debug use only)
    struct AchievementSnapshot {
        std::string name{};
        bool achieved{};
        uint32 progress{};
        uint32 unlock_time{};
    };
    std::vector<AchievementSnapshot> achievements_snapshot{};
    std::map<std::string, float> ach_global_percentages_snapshot{};
    
    bool show_overlay = false;
    bool show_user_info = false;
    bool show_friends = false;
    bool show_chat = false;
    bool show_achievements = false;
    bool show_settings = false;
    bool show_networks = false;
    bool show_lobby_chat = false;
    bool show_sce_browser = false;

    // SCE asset download progress (set by NotifySceAssetsReady, read on render thread)
    struct SceAssetProgress {
        bool   pending_notification{false};
        uint32_t downloaded{0};
        uint32_t skipped{0};
        uint32_t total{0};
    } sce_asset_progress{};

    // Per-item GPU texture cache for the SCE asset browser.
    // Key: "folder/filename" (relative to app storage root).
    // Lifecycle: created lazily on first render, freed when show_sce_browser is closed.
    struct SceTexture {
        InGameOverlay::RendererResource_t *resource{nullptr}; // owned, must be Delete()d
        std::vector<image_pixel_t> pixels{};                  // kept alive for AttachResource (RGBA8)
        std::string fp16_pixels{};                            // kept alive for AttachResource (FP16)
        int   w{0};
        int   h{0};
        bool  load_attempted{false};
    };
    std::map<std::string, SceTexture> sce_textures{};
    std::string sce_preview_key{};              // tex_key of the asset currently shown in the preview popup
    std::vector<std::string> sce_preview_nav_keys{};  // ordered keys for same-type same-series nav
    bool sce_textures_pending_free{false};      // deferred cleanup flag to avoid mid-frame Delete()
    void sce_textures_free_all()
    {
        for (auto &[k, t] : sce_textures)
            if (t.resource) { t.resource->Delete(); t.resource = nullptr; }
        sce_textures.clear();
    }

    // Local user avatar (loaded once, displayed in user info)
    InGameOverlay::RendererResource_t* local_avatar_resource{nullptr};
    std::string local_avatar_pixels{};
    int local_avatar_handle{-1};
    bool try_load_avatar(friend_window_state &state, uint64 steam_id);
    bool try_load_local_avatar();

    // achievement list display options
    int  ach_sort_mode{0};             // 0=Global %, 1=Schema Order, 2=Alphabetical
    int  ach_current_tab{1};           // 0=In Progress, 1=My Achievements, 2=Global Stats
    char ach_search_buf[256]{};        // search filter for achievements

    // warn when using local save
    bool warn_local_save = false;
    // warn when app ID = 0
    bool warn_bad_appid = false;

    char username_text[256]{};
    std::atomic<bool> save_settings = false;

    int current_language = 0;

    std::string warning_message{};

    // Callback infos
    std::queue<Friend> has_friend_action{};
    std::vector<Notification> notifications{};
    static constexpr size_t MAX_NOTIFICATION_HISTORY = 50;
    std::deque<NotificationHistoryEntry> notification_history{};
    bool show_notification_history = false;
    // Cache for pre-formatted history lines — avoids rebuilding every frame
    std::vector<std::string> notification_history_cache{};
    bool notification_history_cache_dirty = false;
    // used when the button "Invite all" is clicked
    std::atomic<bool> invite_all_friends_clicked = false;
    // track lobby join requests we've already shown a notification for
    std::set<std::pair<uint64, uint64>> notified_lobby_join_requests{};

    // track which friend lobbies we've already notified about (friend_id -> lobby_id)
    std::unordered_map<uint64, uint64> notified_friend_lobbies{};

    // the lobby the game passed to the last OpenOverlayInvite() call
    // (ActivateGameOverlayInviteDialog()). invite_friend() must send the invitation
    // to this specific lobby, not the last joined one
    std::atomic<uint64> invite_lobby_id{};

    // Rate-limiting queue for achievement notifications
    struct ScheduledAchievement {
        Overlay_Achievement ach;
        bool for_progress;
        std::chrono::milliseconds trigger_time; // when the achievement was triggered
        std::chrono::milliseconds scheduled_show_time; // when the notification should be shown
    };
    std::deque<ScheduledAchievement> achievement_queue{};
    std::chrono::milliseconds last_scheduled_show_time{}; // tracks the last scheduled show time for spacing

    bool overlay_state_changed = false;

    std::atomic<bool> i_have_lobby = false;
    std::atomic<bool> i_have_game_server = false;

    // Lobby chat state
    std::string lobby_chat_history{};
    size_t lobby_chat_last_entry_count{0};  // how many chat_entries we've already processed
    char lobby_chat_input[768]{};

    // notifications queued before overlay is ready
    std::vector<std::string> pending_lobby_notifications{};

    // some stuff has to be initialized once the renderer hook is ready
    std::atomic<bool> late_init_imgui = false;

    // changed each time a notification is posted or overlay is shown/hidden
    std::atomic_uint32_t renderer_frame_processing_requests = 0;
    // changed only when overlay is shown/hidden, true means overlay is shown
    std::atomic_uint32_t obscure_cursor_requests = 0;
    
    InGameOverlay::RendererHook_t *_renderer{};

    common_helpers::KillableWorker renderer_detector_delay_thread{};
    common_helpers::KillableWorker renderer_hook_init_thread{};
    int renderer_hook_timeout_ctr{};

    std::vector<InGameOverlay::ToggleKey> toggle_keys{};
    std::vector<InGameOverlay::ToggleKey> screenshot_keys{};

    struct ScreenshotItem {
        std::string filename;
        std::string full_path;
        InGameOverlay::RendererResource_t* texture = nullptr;
        // Keep pixel data alive until the GPU has consumed it (AttachResource does NOT own the data)
        std::vector<uint8_t> thumbnail_pixels{};
        bool selected = false;
        bool failed_to_load = false;
        time_t mtime = 0;  // file modification time, 0 = unknown
        uint64_t size = 0; // file size in bytes, 0 = unknown
    };

    struct CapturedScreenshot {
        uint32_t width;
        uint32_t height;
        std::vector<uint8_t> pixels_rgb;
    };

    std::vector<ScreenshotItem> screenshot_items{};
    bool screenshots_loaded = false;
    bool show_screenshots_window = false;
    std::string preview_screenshot_path{};
    InGameOverlay::RendererResource_t* preview_texture = nullptr;
    // Persistent storage for preview/pin pixel data (AttachResource does NOT own the data)
    std::vector<uint8_t> preview_pixels{};
    uint32_t preview_pixels_w = 0;
    uint32_t preview_pixels_h = 0;
    int preview_index = -1;
    bool preview_open_active = false;       // true between OpenPopup and explicit close
    bool preview_delete_pending = false;    // true while inline delete confirmation is shown in preview
    bool preview_crop_mode = false;         // true while the crop editor is open inside the preview
    ImVec4 preview_crop_rect = { 0, 0, 0, 0 };        // current selection in source pixels (0,0,0,0 = full image)
    ImVec4 preview_crop_rect_prev = { 0, 0, 0, 0 };   // saved on entering crop mode (for Cancel)
    bool delete_confirm_open_active = false;

    bool show_delete_confirmation = false;
    bool delete_all_selected = false;
    std::string single_delete_path;

    // Drag state for the crop-rectangle editor. Used by both the pin and the
    // preview crop flows. `handle` follows the same convention as in the
    // editor implementation: 0..3 = corners (TL, TR, BR, BL), 4..7 = edge
    // midpoints (T, R, B, L), 8 = body move, -1 = none.
    struct CropDragState {
        bool dragging = false;
        ImVec2 start = { 0, 0 };
        int handle = -1;
        ImVec2 anchor = { 0, 0 };        // opposite corner / body offset
        int handle_hover = -1;            // -1 = none
    };

    struct PinnedScreenshot {
        uint64_t id;                              // unique per-pin ID for ImGui window identity
        std::string path;
        InGameOverlay::RendererResource_t* texture = nullptr;
        std::vector<uint8_t> pixels;
        uint32_t pixels_w = 0, pixels_h = 0;
        float opacity = 1.0f;
        bool pos_set = false;
        ImVec2 pos = { 100, 100 };
        ImVec2 size = { 320, 180 };
        ImVec2 image_disp = { 0, 0 };             // actual image display size (capped by aspect ratio)
        ImVec2 last_outer = { 0, 0 };             // actual window outer size from last frame (drift guard)
        bool focus_requested = false;              // true to bring window to front on next frame
        bool open = true;                          // tracks window close-button (X) state
        ImVec4 crop_rect = { 0, 0, 0, 0 };         // crop region (x0,y0,x1,y1) in source pixels; (0,0,0,0) = no crop (show full image)
        ImVec4 crop_rect_prev = { 0, 0, 0, 0 };    // saved crop_rect when entering crop mode (for Cancel)
        bool crop_mode = false;                    // true while the crop editor is open for this pin
        CropDragState crop_drag{};                 // per-pin drag state for the crop editor
    };

    std::vector<PinnedScreenshot> pinned_screenshots{};
    uint64_t next_pin_id = 1;

    // Maximum dimension (px) for a context-menu-initiated pin. Generous — modern monitors are large.
    static constexpr float kContextPinMaxDim = 800.0f;

    std::vector<CapturedScreenshot> captured_screenshots_queue{};
    std::mutex captured_screenshots_mutex{};

    // font stuff - now supporting independent font sizes
    ImFontAtlas fonts_atlas{};
    ImFont *font_default{};
    ImFont *font_notif{};
    ImFont *font_fps{}; // separate font for FPS display
    ImFont *font_ach_title{}; // separate font for achievement title
    ImFont *font_ach_desc{}; // separate font for achievement description
    ImFontConfig font_cfg{};
    ImFontGlyphRangesBuilder font_builder{};
    ImVector<ImWchar> ranges{};

    std::recursive_mutex overlay_mutex{};
    std::atomic<bool> setup_overlay_called = false;
    std::atomic<int64_t> bridge_last_heartbeat_ms{0}; // steady_clock ms, 0 = never connected

    std::map<std::string, std::vector<char>> wav_files{
        { "overlay_achievement_notification.wav",  {} }, // achievement unlocked
        { "overlay_achievement_progress.wav",      {} }, // achievement progress (not yet unlocked)
        { "overlay_friend_notification.wav",       {} }, // generic friend/lobby fallback
        { "overlay_invite_notification.wav",       {} }, // game invite from friend
        { "overlay_chat_notification.wav",         {} }, // chat message
        { "overlay_auto_accept_notification.wav",  {} }, // auto-accepted invite
        { "overlay_lobby_join_request.wav",        {} }, // someone requests to join your lobby
        { "overlay_lobby_join_response.wav",       {} }, // generic fallback for accepted/denied
        { "overlay_lobby_join_accepted.wav",       {} }, // your lobby join request was accepted
        { "overlay_lobby_join_denied.wav",         {} }, // your lobby join request was denied
        { "overlay_lobby_kicked.wav",              {} }, // you were kicked from a lobby
        { "overlay_friend_lobby.wav",              {} }, // a friend entered a lobby
        { "overlay_lobby_status.wav",              {} }, // lobby/server status change (created, closed, etc.)
    };

    Steam_Overlay(Steam_Overlay const&) = delete;
    Steam_Overlay(Steam_Overlay&&) = delete;
    Steam_Overlay& operator=(Steam_Overlay const&) = delete;
    Steam_Overlay& operator=(Steam_Overlay&&) = delete;

    void parse_key_combo();
    void parse_screenshot_key_combo();
    bool submit_notification(
        notification_type type,
        const std::string &msg,
        std::pair<const Friend, friend_window_state> *frd = nullptr,
        Overlay_Achievement *ach = nullptr
    );

    void play_overlay_sound(const char* sound_key);
    
    void refresh_screenshots_list();
    void render_gallery_window();
    void render_pinned_screenshot();
    void process_captured_screenshots();
    // Clears all preview-popup state (path, index, texture, pixels). Does NOT call CloseCurrentPopup.
    void clear_preview_state();
    // Removes a single pinned screenshot by ID (cleans up GPU resources).
    void unpin_screenshot(uint64_t id);
    // Removes all pinned screenshots (cleans up GPU resources).
    void unpin_all_screenshots();

    // Drag state for the crop-rectangle editor. The pin version lives inside
    // PinnedScreenshot (per-pin); the preview version is shared since only
    // one preview exists at a time.
    CropDragState preview_crop_drag{};

    // Renders the crop-rectangle editor over a displayed image. `rect` is
    // (x0, y0, x1, y1) in source pixels and is mutated in place. `tex_id` is
    // the texture resource; it's used to redraw the image inside the
    // selection area so the dim overlay outside doesn't cover it. Returns
    // Active while the user keeps editing, Confirm when the Confirm button
    // is clicked (caller applies the crop), Cancel when Cancel is clicked
    // (caller discards).
    enum class CropAction { Active, Confirm, Cancel };
    CropAction render_crop_editor(ImVec4& rect, CropDragState& st,
                                  ImTextureID tex_id,
                                  uint32_t src_w, uint32_t src_h,
                                  ImVec2 img_min, ImVec2 img_size);

    static void on_screenshot_captured(const InGameOverlay::ScreenshotCallbackParameter_t* screenshot, void* userParameter);

    void notify_sound_user_invite(friend_window_state& friend_state);
    void notify_sound_chat_message(friend_window_state& friend_state);
    void notify_sound_user_achievement();
    void notify_sound_auto_accept_friend_invite();
    void notify_sound_lobby_join();
    void notify_sound_friend_lobby();
    void notify_sound_lobby_kicked();
    void notify_sound_lobby_join_response(bool accepted);
    void notify_sound_achievement_progress();
    void notify_sound_lobby_status();

    // Right click on friend
    void build_friend_context_menu(Friend const& frd, friend_window_state &state);
    // Double click on friend
    void build_chat_window();
    std::chrono::milliseconds get_notification_duration(notification_type type);
    // Notifications like achievements, chat and invitations
    void set_next_notification_pos(std::pair<float, float> scrn_size, std::chrono::milliseconds elapsed, std::chrono::milliseconds duration, const Notification &noti, struct NotificationsCoords &coords);
    // factor controlling the amount of sliding during the animation, 0 means disabled
    float animate_factor(std::chrono::milliseconds elapsed, std::chrono::milliseconds duration);
    void add_ach_progressbar(const Overlay_Achievement &ach);
    ImVec4 get_notification_bg_rgba_safe();
    void build_notifications(float width, float height);
    
    void request_renderer_detector();
    void set_renderer_hook_timeout();
    void cleanup_renderer_hook();
    bool renderer_hook_proc();
    
    // note: make sure to load all relevant strings before creating the font(s), otherwise some glyphs ranges will be missing
    void create_fonts();
    void load_audio();
    void load_achievements_data();

    void overlay_state_hook(bool ready);
    void allow_renderer_frame_processing(bool state, bool cleaning_up_overlay = false);
    void obscure_game_input(bool state);

    void add_auto_accept_invite_notification();
    void add_invite_notification(std::pair<const Friend, friend_window_state> &wnd_state);
    void post_achievement_notification(Overlay_Achievement &ach, bool for_progress);
    void add_chat_message_notification(std::string const& message, std::pair<const Friend, friend_window_state> *frd = nullptr);
    void poll_lobby_join_requests();
    void show_test_achievement();

    bool open_overlay_hook(bool toggle);

    bool try_load_ach_icon(Overlay_Achievement &ach, bool achieved, bool upload_new_icon_to_gpu);

    void overlay_render_proc();
    void load_next_ach_icon();
    uint32 apply_global_style_color();
    void render_main_window();


    void steam_run_callback_update_my_lobby();
    bool is_friend_joinable(std::pair<const Friend, friend_window_state> &f);
    // invite a single friend
    void invite_friend(uint64 friend_id, class Steam_Friends* steamFriends, class Steam_Matchmaking* steamMatchmaking);
    void steam_run_callback_friends_actions();
    void steam_run_callback();

    void networking_msg_received(Common_Message* msg);


    static void overlay_run_callback(void* object);
    static void overlay_networking_callback(void* object, Common_Message* msg);
    
public:
    Steam_Overlay(Settings* settings, Local_Storage *local_storage, SteamCallResults* callback_results, SteamCallBacks* callbacks, RunEveryRunCB* run_every_runcb, Networking *network, PlaytimeCounter* playtime_counter);

    ~Steam_Overlay();

    bool Ready() const;

    bool NeedPresent() const;

    void SetNotificationPosition(ENotificationPosition eNotificationPosition);

    void SetNotificationInset(int nHorizontalInset, int nVerticalInset);
    void SetupOverlay();
    void UnSetupOverlay();

    void OpenOverlayInvite(CSteamID lobbyId);
    void OpenOverlay(const char* pchDialog);
    void OpenOverlayWebpage(const char* pchURL);

    bool ShowOverlay() const;
    void ShowOverlay(bool state);

    void SetLobbyInvite(Friend friendId, uint64 lobbyId);
    void SetRichInvite(Friend friendId, const char* connect_str);

    void FriendConnect(Friend _friend);
    void FriendDisconnect(Friend _friend);
    void FriendUpdate(Friend _friend);

    void AddAchievementNotification(const std::string &ach_name, nlohmann::json const& ach, bool for_progress);

    void add_lobby_join_request_response_notification(uint64 lobby_id, const std::string &owner_name, bool accepted, uint64 source_id = 0);

    void add_lobby_kicked_notification(uint64 lobby_id, const std::string &kicker_name, uint64 source_id = 0);

    void SortAchievementsByGlobalPercent(const std::map<std::string, float> &percentages);

    // Called by Steam_User_Stats after SteamHunters data arrives asynchronously
    void UpdateSteamHuntersData();

    // Called by Steam_User_Stats when SCE asset downloads finish
    void NotifySceAssetsReady(uint32_t downloaded, uint32_t skipped, uint32_t total);

    // ── Bridge accessor methods (called by overlay_bridge.cpp exports) ──
    struct BridgeStatsSnapshot {
        bool  show_fps{};
        bool  show_frametime{};
        bool  show_playtime{};
        bool  show_fps_graph{};
        bool  show_frametime_graph{};
        bool  show_min_max_avg{};
        bool  show_percentile_1{};
        bool  show_percentile_5{};
        bool  show_percentile_01{};
        int   graph_timeframe_sec{};
        float fps{};
        float frametime_ms{};
        float playtime_hr{};
        float playtime_min{};
        float playtime_sec{};
    };

    bool Bridge_GetWarnLocalSave() const;
    bool Bridge_GetWarnBadAppId() const;
    int  Bridge_GetNotifPosition() const;
    // Sets the global notification position from a GSE_NotifPosition value.
    // Only the four corners are representable (Steam's ENotificationPosition has
    // no center variants); returns true if the value was applied.
    bool Bridge_SetNotifPosition(int gse_pos);
    BridgeStatsSnapshot Bridge_GetStatsState() const;
    int  Bridge_GetAchievementCount() const;
    int  Bridge_GetAchievements(struct GSE_Achievement *out, int max_count);
    int  Bridge_HasAchievementGroups();
    int  Bridge_GetAchievementGroupCount();
    int  Bridge_GetAchievementGroups(struct GSE_AchievementGroup *out, int max_count);
    int  Bridge_GetNotifications(struct GSE_Notification *out, int max_count);
    void Bridge_ExpireNotification(int id);
    int  Bridge_GetDisplayInfo(struct GSE_DisplayInfo *out, int max_count) const;
    float Bridge_GetSDRWhiteScale() const;
    int  Bridge_GetFriendCount() const;
    int  Bridge_GetFriends(struct GSE_Friend *out, int max_count) const;
    int  Bridge_HasLobby() const;
    int  Bridge_GetLocalLobbyInfo(struct GSE_LocalLobbyInfo *out) const;
    int  Bridge_GetConnectString(char *out, int out_size) const;
    int  Bridge_GetExeName(char *out, int out_size) const;
    int  Bridge_GetGameServerInfo(struct GSE_GameServerInfo *out) const;
    int  Bridge_GetLanguage() const;
    void Bridge_RequestSaveSettings();
    void Bridge_MarkConnected();
    bool Bridge_IsConnected() const;
    void Bridge_TestAchievement();
    void Bridge_ResetAchievements();
    void Bridge_SimulateAchievements();
    void Bridge_InviteAllFriends();
    void Bridge_FriendAction(uint64_t steam_id, int action);
    void Bridge_KickAllLobbyMembers();
    void Bridge_LeaveLobby();
    void Bridge_AcceptLobbyJoinRequest(int notification_id);
    void Bridge_DeclineLobbyJoinRequest(int notification_id);
    void Bridge_RequestJoinFriendLobby(int notification_id);
    void Bridge_SetShowFps(bool v);
    void Bridge_SetShowFrametime(bool v);
    void Bridge_SetShowPlaytime(bool v);
    void Bridge_SetShowFpsGraph(bool v);
    void Bridge_SetShowFrametimeGraph(bool v);
    void Bridge_SetShowMinMaxAvg(bool v);
    void Bridge_SetShowPercentile1(bool v);
    void Bridge_SetShowPercentile5(bool v);
    void Bridge_SetShowPercentile01(bool v);
    void Bridge_SetGraphTimeframe(int sec);
    
    // Chat support for ReShade addon
    int  Bridge_GetChatState(uint64_t steam_id, struct GSE_ChatState *out);
    void Bridge_SendChatMessage(uint64_t steam_id, const char *msg);
    void Bridge_OpenChat(uint64_t steam_id);
    void Bridge_CloseChat(uint64_t steam_id);
    
    // Avatar support for ReShade addon
    int  Bridge_GetAvatar(uint64_t steam_id, struct GSE_AvatarData *out);
    int  Bridge_GetLocalAvatar(struct GSE_AvatarData *out);

    // Lobby chat support
    int  Bridge_GetLobbyChatState(struct GSE_LobbyChatState *out);
    void Bridge_SendLobbyChatMsg(const char *msg);

    // IP address support for ReShade addon
    int  Bridge_GetLocalIP(char *out, int max_len) const;

    // Network topology for overlay
    int  Bridge_GetNetworkInfo(struct GSE_NetAdapter *out, int max_adapters) const;

    // Screenshots (ABI v16). Capture needs the emu's own renderer hook, which does
    // not exist in bridge-only mode — Bridge_IsScreenshotSupported() reports that.
    int  Bridge_IsScreenshotSupported() const;
    void Bridge_TakeScreenshot();
    int  Bridge_GetScreenshotCount();
    int  Bridge_GetScreenshots(struct GSE_ScreenshotInfo *out, int max_count);
    int  Bridge_DeleteScreenshot(uint64_t id);
    int  Bridge_GetScreenshotsFolder(char *out, int out_size);

    // Notification history (ABI v16)
    int  Bridge_GetNotificationHistoryCount();
    int  Bridge_GetNotificationHistory(struct GSE_NotificationHistoryEntry *out, int max_count);
    void Bridge_ClearNotificationHistory();

    // Toggle hotkey (ABI v17)
    int  Bridge_GetToggleKeys(struct GSE_ToggleKeyInfo *out) const;

    // Identity / localisation (ABI v17). The setters only stage the value in the
    // same fields the native settings window edits; Bridge_RequestSaveSettings()
    // persists them.
    int  Bridge_GetLanguageCount() const;
    int  Bridge_GetLanguageName(int index, char *out, int out_size) const;
    int  Bridge_SetLanguageIndex(int index);
    int  Bridge_SetUsername(const char *name);

    // Rate-limiting queue functions
    void process_achievement_queue();
};

#else // EMU_OVERLAY

class PlaytimeCounter;

class Steam_Overlay
{
public:
    Steam_Overlay(Settings* settings, Local_Storage *local_storage, SteamCallResults* callback_results, SteamCallBacks* callbacks, RunEveryRunCB* run_every_runcb, Networking* network, PlaytimeCounter* playtime_counter) {}
    ~Steam_Overlay() {}

    bool Ready() const { return false; }

    bool NeedPresent() const { return false; }

    void SetNotificationPosition(ENotificationPosition eNotificationPosition) {}

    void SetNotificationInset(int nHorizontalInset, int nVerticalInset) {}
    void SetupOverlay() {}
    void UnSetupOverlay() {}

    void OpenOverlayInvite(CSteamID lobbyId) {}
    void SortAchievementsByGlobalPercent(const std::map<std::string, float> &) {}
    void UpdateSteamHuntersData() {}
    void NotifySceAssetsReady(uint32_t downloaded, uint32_t skipped, uint32_t total) {}
    void OpenOverlay(const char* pchDialog) {}
    void OpenOverlayWebpage(const char* pchURL) {}

    bool ShowOverlay() const { return false; }
    void ShowOverlay(bool state) {}

    void SetLobbyInvite(Friend friendId, uint64 lobbyId) {}
    void SetRichInvite(Friend friendId, const char* connect_str) {}

    void FriendConnect(Friend _friend) {}
    void FriendDisconnect(Friend _friend) {}
    void FriendUpdate(Friend _friend) {}

    void AddAchievementNotification(const std::string &ach_name, nlohmann::json const& ach, bool for_progress) {}

    void add_lobby_join_request_response_notification(uint64 lobby_id, const std::string &owner_name, bool accepted, uint64 source_id = 0) {}

    void add_lobby_kicked_notification(uint64 lobby_id, const std::string &kicker_name, uint64 source_id = 0) {}
    
    void process_achievement_queue() {}
};

#endif // EMU_OVERLAY

#endif //__INCLUDED_STEAM_OVERLAY_H__
