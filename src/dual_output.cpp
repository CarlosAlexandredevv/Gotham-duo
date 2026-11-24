#include "dual_output.h"
#include "log.h"

#include <dxgi1_5.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gotham {

namespace {

constexpr UINT kFrameSlots = 3;
constexpr UINT kOutputBuffers = 3;
constexpr UINT kMsgSetVisible = WM_APP + 1;

struct MonitorDesc {
    RECT rect{};
    bool primary = false;
    wchar_t device[CCHDEVICENAME]{};

    LONG width() const { return rect.right - rect.left; }
    LONG height() const { return rect.bottom - rect.top; }
};

BOOL CALLBACK EnumMonitorProc(HMONITOR hmon, HDC, LPRECT, LPARAM lparam) {
    auto* list = reinterpret_cast<std::vector<MonitorDesc>*>(lparam);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmon, &mi)) {
        MonitorDesc m{};
        m.rect = mi.rcMonitor;
        m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        wcscpy_s(m.device, mi.szDevice);
        list->push_back(m);
    }
    return TRUE;
}

std::vector<MonitorDesc> QueryMonitors() {
    std::vector<MonitorDesc> monitors;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorProc, reinterpret_cast<LPARAM>(&monitors));
    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto& m = monitors[i];
        Log("Monitor[%zu]: %ls (%ld,%ld %ldx%ld) primary=%d",
            i,
            m.device,
            m.rect.left,
            m.rect.top,
            m.width(),
            m.height(),
            m.primary ? 1 : 0);
    }
    return monitors;
}

bool SelectTargetMonitors(HMODULE self_module,
                          const std::vector<MonitorDesc>& monitors,
                          MonitorDesc& out_p1,
                          MonitorDesc& out_p2) {
    if (monitors.size() < 2) {
        Log("Need at least 2 monitors; found %zu.", monitors.size());
        return false;
    }

    wchar_t ini_path[MAX_PATH] = {};
    GetModuleFileNameW(self_module, ini_path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(ini_path, L'\\')) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - ini_path), L"gothamduo.ini");
    }

    const int cfg_p1 = GetPrivateProfileIntW(L"Monitors", L"Player1", -1, ini_path);
    const int cfg_p2 = GetPrivateProfileIntW(L"Monitors", L"Player2", -1, ini_path);
    if (cfg_p1 >= 0 && cfg_p2 >= 0 && cfg_p1 != cfg_p2 &&
        static_cast<size_t>(cfg_p1) < monitors.size() &&
        static_cast<size_t>(cfg_p2) < monitors.size()) {
        out_p1 = monitors[cfg_p1];
        out_p2 = monitors[cfg_p2];
        Log("Using gothamduo.ini monitors: Player1=%d (%ls), Player2=%d (%ls)",
            cfg_p1, out_p1.device, cfg_p2, out_p2.device);
        return true;
    }

    // Auto-select landscape monitors ordered left-to-right so left half sits on the left monitor.
    std::vector<size_t> candidates;
    for (size_t i = 0; i < monitors.size(); ++i) {
        if (monitors[i].width() >= monitors[i].height()) {
            candidates.push_back(i);
        }
    }
    if (candidates.size() < 2) {
        candidates.clear();
        for (size_t i = 0; i < monitors.size(); ++i) {
            candidates.push_back(i);
        }
    }

    std::sort(candidates.begin(), candidates.end(), [&](size_t a, size_t b) {
        if (monitors[a].rect.left != monitors[b].rect.left) {
            return monitors[a].rect.left < monitors[b].rect.left;
        }
        return monitors[a].rect.top < monitors[b].rect.top;
    });

    out_p1 = monitors[candidates[0]];
    out_p2 = monitors[candidates[1]];
    Log("Auto-selected monitors: Player1=%zu (%ls), Player2=%zu (%ls)",
        candidates[0], out_p1.device, candidates[1], out_p2.device);
    return true;
}

LRESULT CALLBACK OutputWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    if (msg == WM_MOUSEACTIVATE) {
        const auto* game_hwnd_ptr =
            reinterpret_cast<const std::atomic<HWND>*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (game_hwnd_ptr) {
            if (HWND gh = game_hwnd_ptr->load(); gh && IsWindow(gh)) {
                SetForegroundWindow(gh);
            }
        }
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ResizeGameWindowToDualSpan(HWND gh, const MonitorDesc& p1, const MonitorDesc& p2) {
    if (!gh || !IsWindow(gh)) {
        return;
    }

    const int target_w = static_cast<int>(p1.width() + p2.width());
    const int target_h = static_cast<int>((std::min)(p1.height(), p2.height()));
    const int target_x = static_cast<int>((std::min)(p1.rect.left, p2.rect.left));
    const int target_y = static_cast<int>((std::min)(p1.rect.top, p2.rect.top));

    LONG_PTR style = GetWindowLongPtrW(gh, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtrW(gh, GWL_STYLE, style);

    SetWindowPos(gh,
                 nullptr,
                 target_x,
                 target_y,
                 target_w,
                 target_h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_NOSENDCHANGING |
                     SWP_ASYNCWINDOWPOS);

    Log("Requested game window span resize: hwnd=%p (%d,%d %dx%d)",
        static_cast<void*>(gh),
        target_x,
        target_y,
        target_w,
        target_h);
}

}  // namespace

struct DualOutput::Impl {
    HMODULE self_module = nullptr;
    MonitorDesc mon_p1{};
    MonitorDesc mon_p2{};
    bool has_monitors = false;

    HANDLE win_thread = nullptr;
    HANDLE windows_ready_event = nullptr;
    DWORD win_thread_id = 0;
    HWND hwnd[2]{};
    std::atomic<bool> stop_requested{false};

    bool pipeline_ready = false;
    bool init_failed = false;
    bool tearing_supported = false;
    bool logged_runtime_err = false;
    bool tried_wide_resize = false;
    std::atomic<bool> split_enabled{true};
    std::atomic<HWND> game_hwnd{nullptr};
    std::atomic<ULONGLONG> last_present_ms{0};
    UINT src_width = 0;
    UINT src_height = 0;
    DXGI_FORMAT src_format = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocators[kFrameSlots];
    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 next_fence_value = 1;
    UINT64 slot_fence_values[kFrameSlots]{};
    UINT frame_index = 0;

    ComPtr<IDXGISwapChain3> out_swapchain[2];

    static DWORD WINAPI WindowThreadProc(LPVOID param) {
        auto* self = static_cast<Impl*>(param);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OutputWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GothamDuoOutputWindow";
        RegisterClassExW(&wc);

        const MonitorDesc* targets[2] = {&self->mon_p1, &self->mon_p2};
        const wchar_t* titles[2] = {L"Gotham Duo - Player 1", L"Gotham Duo - Player 2"};

        for (int i = 0; i < 2; ++i) {
            const DWORD ex_style = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
            self->hwnd[i] = CreateWindowExW(ex_style,
                                            wc.lpszClassName,
                                            titles[i],
                                            WS_POPUP,
                                            targets[i]->rect.left,
                                            targets[i]->rect.top,
                                            targets[i]->width(),
                                            targets[i]->height(),
                                            nullptr,
                                            nullptr,
                                            wc.hInstance,
                                            &self->game_hwnd);
            if (self->hwnd[i]) {
                ShowWindow(self->hwnd[i], SW_SHOWNOACTIVATE);
                UpdateWindow(self->hwnd[i]);
            }
            Log("Created output window %d: hwnd=%p (%ld,%ld %ldx%ld)",
                i + 1,
                static_cast<void*>(self->hwnd[i]),
                targets[i]->rect.left,
                targets[i]->rect.top,
                targets[i]->width(),
                targets[i]->height());
        }

        SetEvent(self->windows_ready_event);
        if (self->hwnd[0]) {
            SetTimer(self->hwnd[0], 1, 200, nullptr);
        }

        bool currently_visible = true;
        auto apply_visibility = [&](bool show) {
            if (show == currently_visible) {
                return;
            }
            currently_visible = show;
            const int cmd = show ? SW_SHOWNOACTIVATE : SW_HIDE;
            for (int i = 0; i < 2; ++i) {
                if (self->hwnd[i]) {
                    ShowWindow(self->hwnd[i], cmd);
                }
            }
        };

        MSG msg{};
        while (!self->stop_requested.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == kMsgSetVisible) {
                apply_visibility(msg.wParam != 0);
                continue;
            }
            if (msg.message == WM_TIMER && msg.hwnd == self->hwnd[0]) {
                const HWND gh = self->game_hwnd.load();
                const ULONGLONG last_ms = self->last_present_ms.load();
                const bool game_active =
                    (!gh || (IsWindow(gh) && IsWindowVisible(gh) && !IsIconic(gh))) &&
                    (last_ms == 0 || (GetTickCount64() - last_ms) < 1500);
                apply_visibility(self->split_enabled.load() && game_active);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (self->hwnd[0]) {
            KillTimer(self->hwnd[0], 1);
        }

        for (int i = 0; i < 2; ++i) {
            if (self->hwnd[i]) {
                DestroyWindow(self->hwnd[i]);
                self->hwnd[i] = nullptr;
            }
        }
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
    }

    void WaitForGpuIdle() {
        if (!fence || !fence_event) {
            return;
        }
        const UINT64 wait_val = next_fence_value - 1;
        if (fence->GetCompletedValue() < wait_val) {
            if (SUCCEEDED(fence->SetEventOnCompletion(wait_val, fence_event))) {
                WaitForSingleObject(fence_event, 2000);
            }
        }
    }

    void ReleaseD3D12() {
        WaitForGpuIdle();
        for (auto& sc : out_swapchain) {
            sc.Reset();
        }
        cmd_list.Reset();
        for (auto& alloc : allocators) {
            alloc.Reset();
        }
        fence.Reset();
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
        device.Reset();
        pipeline_ready = false;
        src_width = 0;
        src_height = 0;
        src_format = DXGI_FORMAT_UNKNOWN;
    }

    bool InitD3D12(IDXGISwapChain3* game_swapchain, ID3D12CommandQueue* game_queue) {
        if (!has_monitors || !hwnd[0] || !hwnd[1]) {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(game_swapchain->GetDesc1(&desc)) || desc.Width < 2 || desc.Height < 1) {
            Log("DualOutput: invalid game swapchain desc.");
            return false;
        }

        if (FAILED(game_swapchain->GetDevice(IID_PPV_ARGS(&device)))) {
            Log("DualOutput: failed to get ID3D12Device from game swapchain.");
            return false;
        }

        for (UINT i = 0; i < kFrameSlots; ++i) {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&allocators[i])))) {
                Log("DualOutput: CreateCommandAllocator failed.");
                return false;
            }
            slot_fence_values[i] = 0;
        }

        if (FAILED(device->CreateCommandList(0,
                                             D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             allocators[0].Get(),
                                             nullptr,
                                             IID_PPV_ARGS(&cmd_list)))) {
            Log("DualOutput: CreateCommandList failed.");
            return false;
        }
        cmd_list->Close();

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            Log("DualOutput: CreateFence failed.");
            return false;
        }
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        next_fence_value = 1;

        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
            Log("DualOutput: CreateDXGIFactory2 failed.");
            return false;
        }

        BOOL allow_tearing = FALSE;
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing)))) {
            tearing_supported = (allow_tearing == TRUE);
        }

        DXGI_SWAP_CHAIN_DESC1 out_desc{};
        out_desc.Width = desc.Width / 2;
        out_desc.Height = desc.Height;
        out_desc.Format = desc.Format;
        out_desc.SampleDesc.Count = 1;
        out_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        out_desc.BufferCount = kOutputBuffers;
        out_desc.Scaling = DXGI_SCALING_STRETCH;
        out_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        out_desc.Flags = tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        for (int i = 0; i < 2; ++i) {
            ComPtr<IDXGISwapChain1> sc1;
            HRESULT hr = factory->CreateSwapChainForHwnd(
                game_queue, hwnd[i], &out_desc, nullptr, nullptr, &sc1);
            if (FAILED(hr) || FAILED(sc1.As(&out_swapchain[i]))) {
                Log("DualOutput: CreateSwapChainForHwnd[%d] failed (hr=0x%08X, fmt=%u, %ux%u).",
                    i,
                    static_cast<unsigned>(hr),
                    static_cast<unsigned>(out_desc.Format),
                    out_desc.Width,
                    out_desc.Height);
                ReleaseD3D12();
                return false;
            }
            factory->MakeWindowAssociation(hwnd[i], DXGI_MWA_NO_ALT_ENTER);
        }

        src_width = desc.Width;
        src_height = desc.Height;
        src_format = desc.Format;
        pipeline_ready = true;
        Log("DualOutput D3D12 pipeline ready: game=%ux%u fmt=%u -> each output=%ux%u tearing=%d",
            src_width,
            src_height,
            static_cast<unsigned>(src_format),
            out_desc.Width,
            out_desc.Height,
            tearing_supported ? 1 : 0);
        return true;
    }
};

DualOutput::~DualOutput() {
    Shutdown();
}

void DualOutput::Start(HMODULE self_module) {
    if (impl_) {
        return;
    }
    impl_ = new Impl();
    impl_->self_module = self_module;

    const auto monitors = QueryMonitors();
    impl_->has_monitors = SelectTargetMonitors(self_module, monitors, impl_->mon_p1, impl_->mon_p2);
    if (!impl_->has_monitors) {
        return;
    }

    impl_->windows_ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->win_thread = CreateThread(
        nullptr, 0, &Impl::WindowThreadProc, impl_, 0, &impl_->win_thread_id);
}

void DualOutput::PresentFrame(IDXGISwapChain3* game_swapchain,
                              ID3D12CommandQueue* game_queue,
                              OrigPresentFn orig_present) {
    if (!impl_ || !impl_->has_monitors || impl_->init_failed || !game_swapchain || !game_queue) {
        return;
    }

    // Wait until the window thread has created both HWNDs (non-blocking once signaled).
    if (WaitForSingleObject(impl_->windows_ready_event, 0) != WAIT_OBJECT_0) {
        return;
    }

    impl_->last_present_ms.store(GetTickCount64());
    if (!impl_->game_hwnd.load()) {
        HWND gh = nullptr;
        if (SUCCEEDED(game_swapchain->GetHwnd(&gh)) && gh) {
            impl_->game_hwnd.store(gh);
        }
    }

    if (GetAsyncKeyState(VK_F9) & 1) {
        const bool next = !impl_->split_enabled.load();
        impl_->split_enabled.store(next);
        if (impl_->win_thread_id != 0) {
            PostThreadMessageW(impl_->win_thread_id, kMsgSetVisible, next ? 1 : 0, 0);
        }
        Log("DualOutput toggled via F9: %s", next ? "ON" : "OFF");
    }

    if (!impl_->split_enabled.load()) {
        return;
    }

    DXGI_SWAP_CHAIN_DESC1 cur_desc{};
    if (FAILED(game_swapchain->GetDesc1(&cur_desc))) {
        return;
    }

    const UINT desired_w = static_cast<UINT>(impl_->mon_p1.width() + impl_->mon_p2.width());
    const bool f10_pressed = (GetAsyncKeyState(VK_F10) & 1) != 0;
    if (f10_pressed || (!impl_->tried_wide_resize && impl_->frame_index == 360 && cur_desc.Width != desired_w)) {
        impl_->tried_wide_resize = true;
        ResizeGameWindowToDualSpan(impl_->game_hwnd.load(), impl_->mon_p1, impl_->mon_p2);
    }

    if (impl_->pipeline_ready &&
        (cur_desc.Width != impl_->src_width || cur_desc.Height != impl_->src_height ||
         cur_desc.Format != impl_->src_format)) {
        impl_->ReleaseD3D12();
    }

    if (!impl_->pipeline_ready) {
        if (!impl_->InitD3D12(game_swapchain, game_queue)) {
            impl_->init_failed = true;
            return;
        }
    }

    const UINT slot = impl_->frame_index++ % kFrameSlots;
    const UINT64 wait_val = impl_->slot_fence_values[slot];
    if (wait_val != 0 && impl_->fence->GetCompletedValue() < wait_val) {
        if (SUCCEEDED(impl_->fence->SetEventOnCompletion(wait_val, impl_->fence_event))) {
            WaitForSingleObject(impl_->fence_event, 1000);
        }
    }

    ComPtr<ID3D12Resource> src_buf;
    ComPtr<ID3D12Resource> dst_buf[2];
    if (FAILED(game_swapchain->GetBuffer(game_swapchain->GetCurrentBackBufferIndex(),
                                         IID_PPV_ARGS(&src_buf)))) {
        return;
    }
    for (int i = 0; i < 2; ++i) {
        if (FAILED(impl_->out_swapchain[i]->GetBuffer(
                impl_->out_swapchain[i]->GetCurrentBackBufferIndex(),
                IID_PPV_ARGS(&dst_buf[i])))) {
            return;
        }
    }

    if (FAILED(impl_->allocators[slot]->Reset()) ||
        FAILED(impl_->cmd_list->Reset(impl_->allocators[slot].Get(), nullptr))) {
        return;
    }

    auto make_barrier = [](ID3D12Resource* res,
                           D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        return b;
    };

    const D3D12_RESOURCE_BARRIER pre_barriers[3] = {
        make_barrier(src_buf.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
        make_barrier(dst_buf[0].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
        make_barrier(dst_buf[1].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    impl_->cmd_list->ResourceBarrier(3, pre_barriers);

    const UINT half_w = impl_->src_width / 2;
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = src_buf.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    const D3D12_BOX boxes[2] = {
        {0, 0, 0, half_w, impl_->src_height, 1},
        {impl_->src_width - half_w, 0, 0, impl_->src_width, impl_->src_height, 1},
    };

    for (int i = 0; i < 2; ++i) {
        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = dst_buf[i].Get();
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        impl_->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &boxes[i]);
    }

    const D3D12_RESOURCE_BARRIER post_barriers[3] = {
        make_barrier(src_buf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
        make_barrier(dst_buf[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
        make_barrier(dst_buf[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
    };
    impl_->cmd_list->ResourceBarrier(3, post_barriers);

    if (FAILED(impl_->cmd_list->Close())) {
        return;
    }

    ID3D12CommandList* lists[] = {impl_->cmd_list.Get()};
    game_queue->ExecuteCommandLists(1, lists);

    const UINT64 fence_val = impl_->next_fence_value++;
    if (SUCCEEDED(game_queue->Signal(impl_->fence.Get(), fence_val))) {
        impl_->slot_fence_values[slot] = fence_val;
    }

    // Call original Present directly to avoid re-entering our MinHook detour.
    const UINT present_flags = impl_->tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0;
    for (int i = 0; i < 2; ++i) {
        const HRESULT hr = orig_present(impl_->out_swapchain[i].Get(), 0, present_flags);
        if (FAILED(hr) && !impl_->logged_runtime_err) {
            impl_->logged_runtime_err = true;
            Log("DualOutput out_swapchain[%d]->Present failed: hr=0x%08X (DeviceRemoved=0x%08X)",
                i,
                static_cast<unsigned>(hr),
                static_cast<unsigned>(impl_->device->GetDeviceRemovedReason()));
        }
    }
}

void DualOutput::ResetSwapChains() {
    if (impl_) {
        impl_->ReleaseD3D12();
    }
}

void DualOutput::Shutdown() {
    if (!impl_) {
        return;
    }
    impl_->stop_requested.store(true);
    if (impl_->win_thread_id != 0) {
        PostThreadMessageW(impl_->win_thread_id, WM_QUIT, 0, 0);
    }
    if (impl_->win_thread) {
        WaitForSingleObject(impl_->win_thread, 2000);
        CloseHandle(impl_->win_thread);
        impl_->win_thread = nullptr;
    }
    impl_->ReleaseD3D12();
    if (impl_->windows_ready_event) {
        CloseHandle(impl_->windows_ready_event);
        impl_->windows_ready_event = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
}

}  // namespace gotham
