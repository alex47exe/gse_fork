#ifdef _WIN32
#ifdef USE_EXTERNAL_OVERLAY

#include "external_overlay_win32.h"

#include <dxgi.h>

#include "InGameOverlay/ImGui/backends/imgui_impl_win32.h"
#include "InGameOverlay/ImGui/backends/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static const wchar_t* GSE_OVERLAY_CLASS_NAME = L"GSE_ExternalOverlay";

LRESULT CALLBACK ExternalOverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_DESTROY:
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ExternalOverlayWindow::Init(HWND game_hwnd, std::function<void()> render_callback, ImFontAtlas* fonts)
{
    game_hwnd_ = game_hwnd;
    render_callback_ = render_callback;

    // Register window class
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = GSE_OVERLAY_CLASS_NAME;
    RegisterClassExW(&wc);

    int cx = GetSystemMetrics(SM_CXSCREEN);
    int cy = GetSystemMetrics(SM_CYSCREEN);

    // Create transparent layered topmost window, initially click-through and inactive
    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        GSE_OVERLAY_CLASS_NAME,
        L"GSE Overlay",
        WS_POPUP,
        0, 0, cx, cy,
        nullptr, nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    if (!hwnd_) return false;

    // Use LWA_ALPHA with 255 — transparency comes from the D3D11 clear color {0,0,0,0}
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    // Create D3D11 device and swapchain
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount        = 2;
    scd.BufferDesc.Width   = (UINT)cx;
    scd.BufferDesc.Height  = (UINT)cy;
    scd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow       = hwnd_;
    scd.SampleDesc.Count   = 1;
    scd.Windowed           = TRUE;
    scd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;
    scd.Flags              = 0;

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        &feature_level, 1,
        D3D11_SDK_VERSION,
        &scd,
        &swapchain_,
        &d3d_device_,
        nullptr,
        &d3d_context_
    );
    if (FAILED(hr)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    // Create render target view
    ID3D11Texture2D* back_buffer{};
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer) {
        d3d_device_->CreateRenderTargetView(back_buffer, nullptr, &rtv_);
        back_buffer->Release();
    }

    // Init ImGui context and backends — create context only if none exists yet
    if (!ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(ImGui::CreateContext(fonts));
    }
    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(d3d_device_, d3d_context_);

    // Start render thread
    running_ = true;
    render_thread_ = std::thread([this]() { RenderLoop(); });

    return true;
}

void ExternalOverlayWindow::SetVisible(bool show)
{
    visible_ = show;

    if (show) {
        // Remove click-through flag so the overlay receives input
        LONG_PTR ex_style = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        ex_style &= ~WS_EX_TRANSPARENT;
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex_style);

        // Steal focus from the game to exit exclusive fullscreen
        SetForegroundWindow(hwnd_);
    } else {
        // Re-add click-through flag
        LONG_PTR ex_style = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        ex_style |= WS_EX_TRANSPARENT;
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex_style);

        // Return focus to the game so DXGI can re-enter exclusive fullscreen
        if (game_hwnd_) {
            SetForegroundWindow(game_hwnd_);
        }
    }
}

void ExternalOverlayWindow::UpdateDisplaySize()
{
    if (!hwnd_ || !swapchain_) return;

    int cx = GetSystemMetrics(SM_CXSCREEN);
    int cy = GetSystemMetrics(SM_CYSCREEN);

    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, cx, cy, SWP_NOACTIVATE);

    // Recreate RTV before resize
    if (rtv_) {
        rtv_->Release();
        rtv_ = nullptr;
    }
    d3d_context_->OMSetRenderTargets(0, nullptr, nullptr);

    swapchain_->ResizeBuffers(0, (UINT)cx, (UINT)cy, DXGI_FORMAT_UNKNOWN, 0);

    ID3D11Texture2D* back_buffer{};
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer) {
        d3d_device_->CreateRenderTargetView(back_buffer, nullptr, &rtv_);
        back_buffer->Release();
    }
}

void ExternalOverlayWindow::Shutdown()
{
    running_ = false;
    if (render_thread_.joinable()) render_thread_.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (rtv_)         { rtv_->Release();         rtv_ = nullptr; }
    if (swapchain_)   { swapchain_->Release();   swapchain_ = nullptr; }
    if (d3d_context_) { d3d_context_->Release(); d3d_context_ = nullptr; }
    if (d3d_device_)  { d3d_device_->Release();  d3d_device_ = nullptr; }

    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    UnregisterClassW(GSE_OVERLAY_CLASS_NAME, GetModuleHandleW(nullptr));
}

void ExternalOverlayWindow::RenderLoop()
{
    while (running_) {
        // Pump messages for our overlay window
        MSG msg{};
        while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (visible_) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            if (render_callback_) render_callback_();

            ImGui::Render();

            // Clear to transparent black
            const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            d3d_context_->OMSetRenderTargets(1, &rtv_, nullptr);
            d3d_context_->ClearRenderTargetView(rtv_, clear_color);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            swapchain_->Present(1, 0);
        } else {
            // Sleep when hidden to avoid wasting GPU
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
}

#endif // USE_EXTERNAL_OVERLAY
#endif // _WIN32
