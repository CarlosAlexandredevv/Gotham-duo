#include "dual_output.h"
#include "d3d12_probe.h"
#include "duo_resize_guard.h"
#include "input_trigger.h"
#include "log.h"
#include "split_seam_detector.h"

#include <dxgi1_5.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gotham {

namespace {

constexpr UINT kFrameSlots = 3;
constexpr UINT kOutputBuffers = 3;
constexpr UINT kMsgSetVisible = WM_APP + 1;

enum class OutputMode { Solo, Duo };

struct MonitorDesc {
    RECT rect{};
    bool primary = false;
    wchar_t device[CCHDEVICENAME]{};

    LONG width() const { return rect.right - rect.left; }
    LONG height() const { return rect.bottom - rect.top; }
};

bool BuildIniPath(HMODULE self_module, wchar_t (&out_path)[MAX_PATH]) {
    out_path[0] = L'\0';
    const DWORD len = GetModuleFileNameW(self_module, out_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        out_path[0] = L'\0';
        return false;
    }
    wchar_t* slash = wcsrchr(out_path, L'\\');
    if (!slash ||
        (MAX_PATH - static_cast<size_t>(slash + 1 - out_path)) < _countof(L"gothamduo.ini")) {
        out_path[0] = L'\0';
        return false;
    }
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - out_path), L"gothamduo.ini");
    return true;
}

InputConfig LoadInputConfig(HMODULE self_module) {
    InputConfig cfg{};
    wchar_t ini_path[MAX_PATH] = {};
    if (!BuildIniPath(self_module, ini_path)) {
        return cfg;
    }

    cfg.allow_controller =
        GetPrivateProfileIntW(L"Input", L"AllowControllerToggle", 1, ini_path) != 0;
    cfg.p2_controller_index = static_cast<int>(
        GetPrivateProfileIntW(L"Input", L"P2ControllerIndex", -1, ini_path));
    const int hold_ms = static_cast<int>(
        GetPrivateProfileIntW(L"Input", L"HoldBackStartMs", 800, ini_path));
    cfg.hold_back_start_ms = static_cast<uint32_t>(hold_ms <= 0 ? 800 : hold_ms);
    return cfg;
}

MirrorConfig LoadMirrorConfig(HMODULE self_module) {
    MirrorConfig cfg{};
    wchar_t ini_path[MAX_PATH] = {};
    if (!BuildIniPath(self_module, ini_path)) {
        return cfg;
    }

    cfg.auto_detect = GetPrivateProfileIntW(L"MenuMirror", L"AutoDetect", 1, ini_path) != 0;
    const int enter_ms = static_cast<int>(
        GetPrivateProfileIntW(L"MenuMirror", L"EnterMirrorMs", 200, ini_path));
    const int exit_ms = static_cast<int>(
        GetPrivateProfileIntW(L"MenuMirror", L"ExitMirrorMs", 100, ini_path));
    cfg.enter_mirror_ms = static_cast<uint32_t>(enter_ms < 0 ? 200 : enter_ms);
    cfg.exit_mirror_ms = static_cast<uint32_t>(exit_ms < 0 ? 100 : exit_ms);
    return cfg;
}

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
                          MonitorDesc& out_solo,
                          MonitorDesc& out_p1,
                          MonitorDesc& out_p2) {
    if (monitors.size() < 2) {
        Log("Need at least 2 monitors; found %zu.", monitors.size());
        return false;
    }

    wchar_t ini_path[MAX_PATH] = {};
    int cfg_p1 = -1;
    int cfg_p2 = -1;
    if (BuildIniPath(self_module, ini_path)) {
        cfg_p1 = static_cast<int>(GetPrivateProfileIntW(L"Monitors", L"Player1", -1, ini_path));
        cfg_p2 = static_cast<int>(GetPrivateProfileIntW(L"Monitors", L"Player2", -1, ini_path));
    }
    if (cfg_p1 >= 0 && cfg_p2 >= 0 && cfg_p1 != cfg_p2 &&
        static_cast<size_t>(cfg_p1) < monitors.size() &&
        static_cast<size_t>(cfg_p2) < monitors.size()) {
        out_p1 = monitors[cfg_p1];
        out_p2 = monitors[cfg_p2];
        Log("Using gothamduo.ini monitors: Player1=%d (%ls), Player2=%d (%ls)",
            cfg_p1, out_p1.device, cfg_p2, out_p2.device);
    } else {
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
    }

    out_solo = out_p2;
    for (const auto& m : monitors) {
        if (m.primary) {
            out_solo = m;
            break;
        }
    }
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

RECT ComputeDualSpanRect(const MonitorDesc& p1, const MonitorDesc& p2) {
    const LONG left = (std::min)(p1.rect.left, p2.rect.left);
    const LONG top = (std::min)(p1.rect.top, p2.rect.top);
    const LONG width = p1.width() + p2.width();
    const LONG height = (std::min)(p1.height(), p2.height());
    return RECT{left, top, left + width, top + height};
}

void ResizeGameWindowToRect(HWND gh, const RECT& rect, const char* reason) {
    if (!gh || !IsWindow(gh)) {
        return;
    }

    const int width = static_cast<int>(rect.right - rect.left);
    const int height = static_cast<int>(rect.bottom - rect.top);

    const LONG_PTR old_style = GetWindowLongPtrW(gh, GWL_STYLE);
    const LONG_PTR new_style =
        (old_style &
         ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU)) |
        WS_POPUP;
    UINT swp_flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_ASYNCWINDOWPOS;
    if (new_style != old_style) {
        SetWindowLongPtrW(gh, GWL_STYLE, new_style);
        swp_flags |= SWP_FRAMECHANGED;
    }

    SetWindowPos(gh, nullptr, rect.left, rect.top, width, height, swp_flags);

    Log("Requested game window resize (%s): hwnd=%p (%ld,%ld %dx%d)",
        reason ? reason : "",
        static_cast<void*>(gh),
        rect.left,
        rect.top,
        width,
        height);
}

}  // namespace

struct DualOutput::Impl {
    HMODULE self_module = nullptr;
    MonitorDesc mon_solo{};
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
    bool pending_duo_resize = false;
    int fullscreen_exit_attempts = 0;
    const char* duo_resize_reason = "duo";
    DuoResizeGuard duo_resize_guard{};
    std::atomic<OutputMode> mode{OutputMode::Solo};
    std::atomic<bool> has_presented_duo{false};
    InputTrigger input_trigger;
    std::atomic<HWND> game_hwnd{nullptr};
    std::atomic<ULONGLONG> last_present_ms{0};
    UINT src_width = 0;
    UINT src_height = 0;
    DXGI_FORMAT src_format = DXGI_FORMAT_UNKNOWN;

    std::mutex d3d_mtx;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocators[kFrameSlots];
    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 next_fence_value = 1;
    UINT64 slot_fence_values[kFrameSlots]{};
    UINT frame_index = 0;

    ComPtr<IDXGISwapChain3> out_swapchain[2];
    UINT out_width[2]{};
    UINT out_height[2]{};
    bool pipeline_mirror = false;
    MirrorStateMachine mirror_state;
    SplitJoinDetector split_join_detector;

    void NotifyVisibilityChange() const {
        if (hwnd[0]) {
            PostMessageW(hwnd[0], kMsgSetVisible, 0, 0);
        } else if (win_thread_id != 0) {
            PostThreadMessageW(win_thread_id, kMsgSetVisible, 0, 0);
        }
    }

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
            Log("Created output window %d (hidden): hwnd=%p (%ld,%ld %ldx%ld)",
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

        bool currently_visible = false;
        auto sync_visibility = [&]() {
            const HWND gh = self->game_hwnd.load();
            const ULONGLONG last_ms = self->last_present_ms.load();
            const HWND fg = GetForegroundWindow();
            const bool game_focused =
                (!gh || fg == gh || fg == self->hwnd[0] || fg == self->hwnd[1]);
            const bool game_active =
                (!gh || (IsWindow(gh) && IsWindowVisible(gh) && !IsIconic(gh))) &&
                (last_ms == 0 || (GetTickCount64() - last_ms) < 1500);
            const bool show = (self->mode.load() == OutputMode::Duo) &&
                              self->has_presented_duo.load() && game_active && game_focused;
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
            if (msg.message == kMsgSetVisible ||
                (msg.message == WM_TIMER && msg.hwnd == self->hwnd[0])) {
                sync_visibility();
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
        std::lock_guard<std::mutex> lock(d3d_mtx);
        ReleaseD3D12Locked();
    }

    void ReleaseD3D12Locked() {
        WaitForGpuIdle();
        for (auto& sc : out_swapchain) {
            sc.Reset();
        }
        for (int i = 0; i < 2; ++i) {
            out_width[i] = 0;
            out_height[i] = 0;
        }
        pipeline_mirror = false;
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
        has_presented_duo.store(false);
        tearing_supported = false;
        logged_runtime_err = false;
        src_width = 0;
        src_height = 0;
        src_format = DXGI_FORMAT_UNKNOWN;
    }

    bool InitD3D12Locked(IDXGISwapChain3* game_swapchain, ID3D12CommandQueue* game_queue) {
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
            ReleaseD3D12Locked();
            return false;
        }

        for (UINT i = 0; i < kFrameSlots; ++i) {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&allocators[i])))) {
                Log("DualOutput: CreateCommandAllocator failed.");
                ReleaseD3D12Locked();
                return false;
            }
            slot_fence_values[i] = 0;
        }

        if (FAILED(device->CreateCommandList(0,
                                             D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             allocators[0].Get(),
                                             nullptr,
                                             IID_PPV_ARGS(&cmd_list))) ||
            FAILED(cmd_list->Close())) {
            Log("DualOutput: CreateCommandList/Close failed.");
            ReleaseD3D12Locked();
            return false;
        }

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            Log("DualOutput: CreateFence failed.");
            ReleaseD3D12Locked();
            return false;
        }
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event) {
            Log("DualOutput: CreateEventW for fence failed.");
            ReleaseD3D12Locked();
            return false;
        }
        next_fence_value = 1;

        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
            Log("DualOutput: CreateDXGIFactory2 failed.");
            ReleaseD3D12Locked();
            return false;
        }

        BOOL allow_tearing = FALSE;
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing)))) {
            tearing_supported = (allow_tearing == TRUE);
        }

        const MonitorDesc* targets[2] = {&mon_p1, &mon_p2};
        const bool init_mirror = mirror_state.is_mirroring();

        for (int i = 0; i < 2; ++i) {
            const UINT desired_out_w = init_mirror ? desc.Width : (desc.Width / 2);
            const UINT max_out_w = static_cast<UINT>(targets[i]->width());
            const UINT max_out_h = static_cast<UINT>(targets[i]->height());
            const UINT target_out_w = (std::max)(1U, (std::min)(desired_out_w, max_out_w));
            const UINT target_out_h = (std::max)(1U, (std::min)(desc.Height, max_out_h));

            DXGI_SWAP_CHAIN_DESC1 out_desc{};
            out_desc.Width = target_out_w;
            out_desc.Height = target_out_h;
            out_desc.Format = desc.Format;
            out_desc.SampleDesc.Count = 1;
            out_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            out_desc.BufferCount = kOutputBuffers;
            out_desc.Scaling = DXGI_SCALING_STRETCH;
            out_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            out_desc.Flags = tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

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
                ReleaseD3D12Locked();
                return false;
            }
            out_width[i] = target_out_w;
            out_height[i] = target_out_h;
            factory->MakeWindowAssociation(hwnd[i], DXGI_MWA_NO_ALT_ENTER);
        }

        pipeline_mirror = init_mirror;
        src_width = desc.Width;
        src_height = desc.Height;
        src_format = desc.Format;
        pipeline_ready = true;
        Log("DualOutput D3D12 pipeline ready: game=%ux%u fmt=%u mirror=%d -> out0=%ux%u out1=%ux%u tearing=%d",
            src_width,
            src_height,
            static_cast<unsigned>(src_format),
            init_mirror ? 1 : 0,
            out_width[0],
            out_height[0],
            out_width[1],
            out_height[1],
            tearing_supported ? 1 : 0);
        return true;
    }

    void CopyAndPresent(IDXGISwapChain3* game_swapchain,
                        ID3D12CommandQueue* game_queue,
                        OrigPresentFn orig_present,
                        bool is_mirroring) {
        ComPtr<IDXGISwapChain3> sc_out[2];
        ComPtr<ID3D12Device> dev_ref;
        bool tearing = false;
        bool already_logged_err = false;

        {
            std::lock_guard<std::mutex> lock(d3d_mtx);
            if (init_failed) {
                return;
            }

            DXGI_SWAP_CHAIN_DESC1 cur_desc{};
            if (FAILED(game_swapchain->GetDesc1(&cur_desc))) {
                return;
            }

            if (pipeline_ready) {
                if (cur_desc.Format != src_format) {
                    ReleaseD3D12Locked();
                } else {
                    const MonitorDesc* targets[2] = {&mon_p1, &mon_p2};
                    const UINT desired_w = is_mirroring ? cur_desc.Width : (cur_desc.Width / 2);
                    bool need_sc_resize = false;
                    UINT target_w[2]{};
                    UINT target_h[2]{};
                    for (int i = 0; i < 2; ++i) {
                        target_w[i] = (std::max)(
                            1U, (std::min)(desired_w, static_cast<UINT>(targets[i]->width())));
                        target_h[i] = (std::max)(
                            1U,
                            (std::min)(cur_desc.Height, static_cast<UINT>(targets[i]->height())));
                        if (target_w[i] != out_width[i] || target_h[i] != out_height[i]) {
                            need_sc_resize = true;
                        }
                    }
                    if (need_sc_resize) {
                        WaitForGpuIdle();
                        for (UINT s = 0; s < kFrameSlots; ++s) {
                            if (allocators[s]) {
                                allocators[s]->Reset();
                            }
                            slot_fence_values[s] = 0;
                        }
                        const UINT sc_flags =
                            tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
                        bool resize_ok = true;
                        for (int i = 0; i < 2; ++i) {
                            if (target_w[i] != out_width[i] || target_h[i] != out_height[i]) {
                                if (FAILED(out_swapchain[i]->ResizeBuffers(kOutputBuffers,
                                                                           target_w[i],
                                                                           target_h[i],
                                                                           cur_desc.Format,
                                                                           sc_flags))) {
                                    resize_ok = false;
                                    break;
                                }
                                out_width[i] = target_w[i];
                                out_height[i] = target_h[i];
                            }
                        }
                        if (!resize_ok) {
                            ReleaseD3D12Locked();
                        } else {
                            src_width = cur_desc.Width;
                            src_height = cur_desc.Height;
                            pipeline_mirror = is_mirroring;
                        }
                    } else {
                        src_width = cur_desc.Width;
                        src_height = cur_desc.Height;
                        pipeline_mirror = is_mirroring;
                    }
                }
            }

            if (!pipeline_ready) {
                if (!InitD3D12Locked(game_swapchain, game_queue)) {
                    init_failed = true;
                    return;
                }
            }

            const UINT slot = frame_index++ % kFrameSlots;
            const UINT64 wait_val = slot_fence_values[slot];
            if (wait_val != 0 && fence->GetCompletedValue() < wait_val) {
                if (SUCCEEDED(fence->SetEventOnCompletion(wait_val, fence_event))) {
                    WaitForSingleObject(fence_event, 1000);
                }
            }

            ComPtr<ID3D12Resource> src_buf;
            ComPtr<ID3D12Resource> dst_buf[2];
            if (FAILED(game_swapchain->GetBuffer(game_swapchain->GetCurrentBackBufferIndex(),
                                                 IID_PPV_ARGS(&src_buf)))) {
                return;
            }
            for (int i = 0; i < 2; ++i) {
                if (FAILED(out_swapchain[i]->GetBuffer(
                        out_swapchain[i]->GetCurrentBackBufferIndex(),
                        IID_PPV_ARGS(&dst_buf[i])))) {
                    return;
                }
            }

            if (FAILED(allocators[slot]->Reset()) ||
                FAILED(cmd_list->Reset(allocators[slot].Get(), nullptr))) {
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
                make_barrier(
                    src_buf.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
                make_barrier(
                    dst_buf[0].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
                make_barrier(
                    dst_buf[1].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
            };
            cmd_list->ResourceBarrier(3, pre_barriers);

            const UINT half_w = src_width / 2;
            D3D12_TEXTURE_COPY_LOCATION src_loc{};
            src_loc.pResource = src_buf.Get();
            src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

            for (int i = 0; i < 2; ++i) {
                const UINT copy_h = (std::min)(src_height, out_height[i]);
                D3D12_BOX box{};
                if (is_mirroring) {
                    const UINT copy_w = (std::min)(src_width, out_width[i]);
                    const UINT src_x = (src_width > copy_w) ? ((src_width - copy_w) / 2) : 0;
                    box = D3D12_BOX{src_x, 0, 0, src_x + copy_w, copy_h, 1};
                } else {
                    const UINT copy_w = (std::min)(half_w, out_width[i]);
                    const UINT src_x = (i == 0) ? 0 : (src_width - copy_w);
                    box = D3D12_BOX{src_x, 0, 0, src_x + copy_w, copy_h, 1};
                }
                D3D12_TEXTURE_COPY_LOCATION dst_loc{};
                dst_loc.pResource = dst_buf[i].Get();
                dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);
            }

            const D3D12_RESOURCE_BARRIER post_barriers[3] = {
                make_barrier(
                    src_buf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
                make_barrier(
                    dst_buf[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
                make_barrier(
                    dst_buf[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
            };
            cmd_list->ResourceBarrier(3, post_barriers);

            if (FAILED(cmd_list->Close())) {
                ReleaseD3D12Locked();
                return;
            }

            ID3D12CommandList* lists[] = {cmd_list.Get()};
            game_queue->ExecuteCommandLists(1, lists);

            const UINT64 fence_val = next_fence_value++;
            if (SUCCEEDED(game_queue->Signal(fence.Get(), fence_val))) {
                slot_fence_values[slot] = fence_val;
            }

            sc_out[0] = out_swapchain[0];
            sc_out[1] = out_swapchain[1];
            dev_ref = device;
            tearing = tearing_supported;
            already_logged_err = logged_runtime_err;
        }

        // Call original Present outside d3d_mtx to avoid re-entering our MinHook detour under lock.
        const UINT present_flags = tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
        for (int i = 0; i < 2; ++i) {
            const HRESULT hr = orig_present(sc_out[i].Get(), 0, present_flags);
            if (FAILED(hr) && !already_logged_err) {
                already_logged_err = true;
                std::lock_guard<std::mutex> lock(d3d_mtx);
                if (!logged_runtime_err) {
                    logged_runtime_err = true;
                    Log("DualOutput out_swapchain[%d]->Present failed: hr=0x%08X (DeviceRemoved=0x%08X)",
                        i,
                        static_cast<unsigned>(hr),
                        dev_ref ? static_cast<unsigned>(dev_ref->GetDeviceRemovedReason()) : 0U);
                }
            }
        }
        if (!has_presented_duo.exchange(true)) {
            NotifyVisibilityChange();
        }
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
    impl_->input_trigger.SetConfig(LoadInputConfig(self_module));
    impl_->mirror_state.SetConfig(LoadMirrorConfig(self_module));

    const auto monitors = QueryMonitors();
    impl_->has_monitors =
        SelectTargetMonitors(self_module, monitors, impl_->mon_solo, impl_->mon_p1, impl_->mon_p2);
    if (!impl_->has_monitors) {
        return;
    }

    Log("Gotham Duo starting in SOLO mode (mon_solo=%ls)", impl_->mon_solo.device);

    impl_->windows_ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->win_thread = CreateThread(
        nullptr, 0, &Impl::WindowThreadProc, impl_, 0, &impl_->win_thread_id);
}

void DualOutput::PresentFrame(IDXGISwapChain3* game_swapchain,
                              ID3D12CommandQueue* game_queue,
                              OrigPresentFn orig_present,
                              std::optional<StripClassification> frame_sample,
                              const FrameRenderStats* frame_stats) {
    if (!impl_ || !impl_->has_monitors || !game_swapchain || !game_queue) {
        return;
    }

    // Wait until the window thread has created both HWNDs (non-blocking once signaled).
    if (WaitForSingleObject(impl_->windows_ready_event, 0) != WAIT_OBJECT_0) {
        return;
    }

    impl_->last_present_ms.store(GetTickCount64());
    HWND gh = impl_->game_hwnd.load();
    if (!gh || !IsWindow(gh)) {
        HWND queried = nullptr;
        if (SUCCEEDED(game_swapchain->GetHwnd(&queried)) && queried) {
            impl_->game_hwnd.store(queried);
        }
    }

    bool f9_down = false;
    std::array<ControllerButtons, 4> slots{};
    PollSystemInput(f9_down, slots);

    const ULONGLONG now_ms = GetTickCount64();
    const bool f1_down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    const bool is_duo = impl_->mode.load() == OutputMode::Duo;
    TriggerAction action =
        impl_->input_trigger.Update(is_duo, f9_down, slots, now_ms);
    bool auto_joined = false;
    if (!is_duo && action == TriggerAction::None) {
        std::lock_guard<std::mutex> lock(impl_->d3d_mtx);
        if (impl_->split_join_detector.Update(
                impl_->mirror_state.config(), frame_sample, static_cast<uint64_t>(now_ms))) {
            action = TriggerAction::ToggleToDuo;
            auto_joined = true;
        }
    }
    if (action == TriggerAction::ToggleToDuo) {
        bool start_mirrored = false;
        {
            std::lock_guard<std::mutex> lock(impl_->d3d_mtx);
            start_mirrored = impl_->mirror_state.config().auto_detect &&
                             frame_sample == StripClassification::Shared;
            impl_->mirror_state.Reset(f1_down, start_mirrored);
            impl_->split_join_detector.Reset(false);
        }
        impl_->mode.store(OutputMode::Duo);
        impl_->pending_duo_resize = true;
        impl_->fullscreen_exit_attempts = 0;
        impl_->duo_resize_reason = start_mirrored ? "duo-mirror" : "duo";
        impl_->duo_resize_guard.Reset();
        Log("Mode switched to DUO%s",
            auto_joined ? " (auto-split)" : (start_mirrored ? " (mirror)" : ""));
    } else if (action == TriggerAction::ToggleToSolo) {
        impl_->mode.store(OutputMode::Solo);
        impl_->pending_duo_resize = false;
        impl_->fullscreen_exit_attempts = 0;
        impl_->duo_resize_reason = "duo";
        impl_->duo_resize_guard.Reset();
        impl_->NotifyVisibilityChange();
        {
            std::lock_guard<std::mutex> lock(impl_->d3d_mtx);
            impl_->ReleaseD3D12Locked();
            impl_->init_failed = false;
            impl_->mirror_state.Reset(false);
            impl_->split_join_detector.Reset(true);
        }
        ResizeGameWindowToRect(impl_->game_hwnd.load(), impl_->mon_solo.rect, "solo");
        Log("Mode switched to SOLO");
    }

    if (impl_->mode.load() != OutputMode::Duo) {
        SetSubtitleSplitActive(false, 0, 0);
        return;
    }

    bool want_mirror = false;
    bool mirror_transitioned = false;
    {
        std::lock_guard<std::mutex> lock(impl_->d3d_mtx);
        const bool has_menu_blur = frame_stats && frame_stats->has_menu_blur_pass;
        const MirrorUpdateResult mirror_res = impl_->mirror_state.Update(
            f1_down, frame_sample, static_cast<uint64_t>(now_ms), has_menu_blur);
        want_mirror = mirror_res.is_mirroring;
        mirror_transitioned = mirror_res.transitioned;
        if (mirror_res.transitioned) {
            if (frame_stats) {
                std::vector<ViewportDrawSummary> sorted = frame_stats->viewports;
                std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
                    return a.draws > b.draws;
                });
                std::string vp_str;
                const size_t limit = (std::min)(sorted.size(), static_cast<size_t>(8));
                for (size_t k = 0; k < limit; ++k) {
                    char buf[64]{};
                    std::snprintf(buf,
                                  sizeof(buf),
                                  "%s(%d,%d %dx%d d=%u dsv=%d)",
                                  k == 0 ? "" : " ",
                                  sorted[k].x,
                                  sorted[k].y,
                                  sorted[k].w,
                                  sorted[k].h,
                                  sorted[k].draws,
                                  sorted[k].has_dsv ? 1 : 0);
                    vp_str += buf;
                }
                Log("DUO layout: %s (%s) frame=%ux%u blur=%d vps=[%s]",
                    want_mirror ? "MIRROR" : "SPLIT",
                    mirror_res.reason,
                    frame_stats->frame_width,
                    frame_stats->frame_height,
                    frame_stats->has_menu_blur_pass ? 1 : 0,
                    vp_str.c_str());
            } else {
                Log("DUO layout: %s (%s)", want_mirror ? "MIRROR" : "SPLIT", mirror_res.reason);
            }
        }
    }
    if (mirror_transitioned) {
        impl_->pending_duo_resize = true;
        impl_->fullscreen_exit_attempts = 0;
        impl_->duo_resize_reason = want_mirror ? "duo-mirror" : "duo-split";
        impl_->duo_resize_guard.Reset();
    }

    const RECT target_rect =
        want_mirror ? impl_->mon_solo.rect : ComputeDualSpanRect(impl_->mon_p1, impl_->mon_p2);
    const UINT expected_w = static_cast<UINT>(target_rect.right - target_rect.left);
    const UINT expected_h = static_cast<UINT>(target_rect.bottom - target_rect.top);

    if (!impl_->pending_duo_resize) {
        const HWND active_gh = impl_->game_hwnd.load();
        const bool window_active =
            active_gh && IsWindow(active_gh) && IsWindowVisible(active_gh) && !IsIconic(active_gh);
        DXGI_SWAP_CHAIN_DESC1 cur_desc{};
        if (window_active && SUCCEEDED(game_swapchain->GetDesc1(&cur_desc))) {
            bool hit_limit = false;
            if (impl_->duo_resize_guard.ShouldReapply(
                    cur_desc.Width, cur_desc.Height, expected_w, expected_h, now_ms, hit_limit)) {
                Log("Duo backbuffer mismatch (%ux%u != expected %ux%u); re-applying %s resize (attempt %d/%d)",
                    cur_desc.Width,
                    cur_desc.Height,
                    expected_w,
                    expected_h,
                    want_mirror ? "mirror" : "duo",
                    impl_->duo_resize_guard.retry_count,
                    DuoResizeGuard::kMaxRetries);
                impl_->pending_duo_resize = true;
                impl_->fullscreen_exit_attempts = 0;
                impl_->duo_resize_reason = want_mirror ? "duo-mirror-reapply" : "duo-reapply";
            } else if (hit_limit) {
                Log("Duo backbuffer stayed %ux%u (expected %ux%u) after %d attempts; stopping auto-resize (press F10 to retry).",
                    cur_desc.Width,
                    cur_desc.Height,
                    expected_w,
                    expected_h,
                    DuoResizeGuard::kMaxRetries);
            }
        }
    }

    if (impl_->pending_duo_resize) {
        BOOL is_fullscreen = FALSE;
        if (SUCCEEDED(game_swapchain->GetFullscreenState(&is_fullscreen, nullptr)) &&
            is_fullscreen) {
            const HRESULT hr = game_swapchain->SetFullscreenState(FALSE, nullptr);
            if (SUCCEEDED(hr) && ++impl_->fullscreen_exit_attempts < 5) {
                return;
            }
            Log("Warning: unable to exit fullscreen before duo resize (hr=0x%08X, attempts=%d); proceeding.",
                static_cast<unsigned>(hr),
                impl_->fullscreen_exit_attempts);
        }
        impl_->pending_duo_resize = false;
        impl_->fullscreen_exit_attempts = 0;
        ResizeGameWindowToRect(impl_->game_hwnd.load(), target_rect, impl_->duo_resize_reason);
        impl_->duo_resize_guard.NoteResizeRequested(GetTickCount64());
        impl_->NotifyVisibilityChange();
    }

    if ((GetAsyncKeyState(VK_F10) & 1) != 0) {
        impl_->duo_resize_guard.Reset();
        impl_->duo_resize_guard.NoteResizeRequested(now_ms);
        ResizeGameWindowToRect(
            impl_->game_hwnd.load(), target_rect, want_mirror ? "duo-mirror-manual" : "duo-manual");
    }

    DXGI_SWAP_CHAIN_DESC1 active_desc{};
    const bool have_active_desc = SUCCEEDED(game_swapchain->GetDesc1(&active_desc));
    const UINT actual_w = have_active_desc ? active_desc.Width : 0;
    const UINT actual_h = have_active_desc ? active_desc.Height : 0;
    const uint64_t check_ms = GetTickCount64();
    if (have_active_desc && (actual_w != expected_w || actual_h != expected_h) &&
        impl_->duo_resize_guard.last_resize_ms.has_value() &&
        check_ms >= *impl_->duo_resize_guard.last_resize_ms &&
        (check_ms - *impl_->duo_resize_guard.last_resize_ms) < 300) {
        SetSubtitleSplitActive(false, actual_w, actual_h);
        return;
    }

    SetSubtitleSplitActive(!want_mirror && actual_w >= 3000, actual_w, actual_h);
    impl_->CopyAndPresent(game_swapchain, game_queue, orig_present, want_mirror);
}

void DualOutput::OnGameResizeBuffers(DXGI_FORMAT new_format) {
    SetSubtitleSplitActive(false, 0, 0);
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->d3d_mtx);
    if (!impl_->pipeline_ready) {
        return;
    }
    if (new_format != DXGI_FORMAT_UNKNOWN && new_format != impl_->src_format) {
        impl_->ReleaseD3D12Locked();
        impl_->init_failed = false;
        return;
    }
    impl_->WaitForGpuIdle();
    for (UINT i = 0; i < kFrameSlots; ++i) {
        if (impl_->allocators[i]) {
            impl_->allocators[i]->Reset();
        }
        impl_->slot_fence_values[i] = 0;
    }
    impl_->src_width = 0;
    impl_->src_height = 0;
}

void DualOutput::ResetSwapChains() {
    SetSubtitleSplitActive(false, 0, 0);
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->d3d_mtx);
        impl_->ReleaseD3D12Locked();
        impl_->init_failed = false;
    }
}

void DualOutput::Shutdown() {
    if (!impl_) {
        return;
    }
    impl_->stop_requested.store(true);
    if (impl_->win_thread) {
        if (impl_->windows_ready_event) {
            WaitForSingleObject(impl_->windows_ready_event, 2000);
        }
        if (impl_->hwnd[0]) {
            PostMessageW(impl_->hwnd[0], WM_QUIT, 0, 0);
        } else if (impl_->win_thread_id != 0) {
            PostThreadMessageW(impl_->win_thread_id, WM_QUIT, 0, 0);
        }
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
