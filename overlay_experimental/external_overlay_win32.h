#pragma once

#ifdef _WIN32
#ifdef USE_EXTERNAL_OVERLAY

#include <windows.h>
#include <d3d11.h>
#include <functional>
#include <thread>
#include <atomic>
#include "InGameOverlay/ImGui/imgui.h"

class ExternalOverlayWindow
{
public:
    ExternalOverlayWindow() = default;
    ~ExternalOverlayWindow() = default;

    ExternalOverlayWindow(const ExternalOverlayWindow&) = delete;
    ExternalOverlayWindow& operator=(const ExternalOverlayWindow&) = delete;

    // Creates the overlay HWND, D3D11 device/swapchain/RTV, inits ImGui backends, starts render thread.
    bool Init(HWND game_hwnd, std::function<void()> render_callback, ImFontAtlas* fonts);

    // Shows or hides the overlay. When show=true steals focus from game (exits exclusive fullscreen).
    // When show=false returns focus to the game HWND.
    void SetVisible(bool show);

    // Stops render thread, shuts down ImGui backends, destroys HWND and D3D11 resources.
    void Shutdown();

    // Resizes overlay HWND and swapchain to current monitor resolution.
    void UpdateDisplaySize();

private:
    void RenderLoop();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd_{};
    HWND game_hwnd_{};
    std::function<void()> render_callback_{};

    ID3D11Device*           d3d_device_{};
    ID3D11DeviceContext*    d3d_context_{};
    IDXGISwapChain*         swapchain_{};
    ID3D11RenderTargetView* rtv_{};

    std::thread render_thread_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> visible_{false};
};

#endif // USE_EXTERNAL_OVERLAY
#endif // _WIN32
