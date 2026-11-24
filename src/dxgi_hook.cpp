#include "dxgi_hook.h"
#include "dual_output.h"
#include "log.h"

#include <MinHook.h>
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>

using gotham::DualOutput;
using gotham::Log;
using Microsoft::WRL::ComPtr;

namespace {

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*,
    IUnknown*,
    HWND,
    const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
    IDXGIOutput*,
    IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

CreateSwapChainFn g_orig_create_swapchain = nullptr;
CreateSwapChainForHwndFn g_orig_create_swapchain_hwnd = nullptr;
PresentFn g_orig_present = nullptr;
Present1Fn g_orig_present1 = nullptr;
ResizeBuffersFn g_orig_resize_buffers = nullptr;

std::mutex g_state_mtx;
ComPtr<IDXGISwapChain3> g_game_swapchain;
ComPtr<ID3D12CommandQueue> g_game_queue;
DualOutput g_dual_output;

thread_local bool g_in_internal_create = false;
std::atomic<uint64_t> g_frame_count{0};
std::atomic<bool> g_logged_missing_queue{false};

void TryCaptureGameSwapChain(IUnknown* device_or_queue, IDXGISwapChain* sc, const char* source) {
    if (!device_or_queue || !sc) {
        return;
    }

    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device_or_queue->QueryInterface(IID_PPV_ARGS(&queue)))) {
        return;
    }

    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        return;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(sc3->GetDesc1(&desc)) || desc.Width < 320 || desc.Height < 240) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_state_mtx);
    g_game_swapchain = sc3;
    g_game_queue = queue;
    Log("Captured game D3D12 SwapChain=%p Queue=%p via %s (%ux%u fmt=%u)",
        static_cast<void*>(sc3.Get()),
        static_cast<void*>(queue.Get()),
        source,
        desc.Width,
        desc.Height,
        static_cast<unsigned>(desc.Format));
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* self,
                                              IUnknown* pDevice,
                                              DXGI_SWAP_CHAIN_DESC* pDesc,
                                              IDXGISwapChain** ppSwapChain) {
    if (g_in_internal_create) {
        return g_orig_create_swapchain(self, pDevice, pDesc, ppSwapChain);
    }

    const HRESULT hr = g_orig_create_swapchain(self, pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        TryCaptureGameSwapChain(pDevice, *ppSwapChain, "CreateSwapChain");
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(
    IDXGIFactory2* self,
    IUnknown* pDevice,
    HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) {
    if (g_in_internal_create) {
        return g_orig_create_swapchain_hwnd(
            self, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    }

    const HRESULT hr = g_orig_create_swapchain_hwnd(
        self, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        TryCaptureGameSwapChain(pDevice, *ppSwapChain, "CreateSwapChainForHwnd");
    }
    return hr;
}

void HandleGamePresent(IDXGISwapChain* self) {
    ComPtr<IDXGISwapChain3> sc3;
    ComPtr<ID3D12CommandQueue> queue;
    {
        std::lock_guard<std::mutex> lock(g_state_mtx);
        if (g_game_swapchain.Get() == self) {
            sc3 = g_game_swapchain;
            queue = g_game_queue;
        }
    }

    if (!sc3 || !queue) {
        if (!g_logged_missing_queue.exchange(true)) {
            Log("Present hit on %p before game D3D12 queue was captured (game_sc=%p).",
                static_cast<void*>(self),
                static_cast<void*>(g_game_swapchain.Get()));
        }
        return;
    }

    g_in_internal_create = true;
    g_dual_output.PresentFrame(sc3.Get(), queue.Get(), g_orig_present);
    g_in_internal_create = false;

    const uint64_t frame = ++g_frame_count;
    if (frame % 300 == 0) {
        Log("DualOutput heartbeat: frame %llu", static_cast<unsigned long long>(frame));
    }
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    HandleGamePresent(self);
    return g_orig_present(self, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* self,
                                       UINT sync_interval,
                                       UINT present_flags,
                                       const DXGI_PRESENT_PARAMETERS* params) {
    HandleGamePresent(self);
    return g_orig_present1(self, sync_interval, present_flags, params);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self,
                                            UINT buffer_count,
                                            UINT width,
                                            UINT height,
                                            DXGI_FORMAT new_format,
                                            UINT swapchain_flags) {
    bool is_game = false;
    {
        std::lock_guard<std::mutex> lock(g_state_mtx);
        is_game = (g_game_swapchain.Get() == self);
    }
    if (is_game) {
        Log("Game ResizeBuffers: %ux%u buffers=%u format=%u",
            width,
            height,
            buffer_count,
            static_cast<unsigned>(new_format));
        g_dual_output.ResetSwapChains();
    }
    return g_orig_resize_buffers(self, buffer_count, width, height, new_format, swapchain_flags);
}

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct DummyDxgiVtables {
    void* create_swapchain = nullptr;
    void* create_swapchain_hwnd = nullptr;
    void* present = nullptr;
    void* resize_buffers = nullptr;
    void* present1 = nullptr;
};

bool ResolveDxgiVtables(DummyDxgiVtables& out) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GothamDuoDummyWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    bool ok = false;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain1> swapchain;

    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        void** factory_vt = *reinterpret_cast<void***>(factory.Get());
        out.create_swapchain = factory_vt[10];
        out.create_swapchain_hwnd = factory_vt[15];

        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)))) {
            DXGI_SWAP_CHAIN_DESC1 sc_desc{};
            sc_desc.Width = 64;
            sc_desc.Height = 64;
            sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sc_desc.SampleDesc.Count = 1;
            sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sc_desc.BufferCount = 2;
            sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

            if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sc_desc, nullptr, nullptr, &swapchain))) {
                void** sc_vt = *reinterpret_cast<void***>(swapchain.Get());
                out.present = sc_vt[8];
                out.resize_buffers = sc_vt[13];
                out.present1 = sc_vt[22];
                ok = true;
            }
        }
    }

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

}  // namespace

namespace gotham {

bool InitDxgiHook(HMODULE self_module) {
    g_dual_output.Start(self_module);

    DummyDxgiVtables vt{};
    if (!ResolveDxgiVtables(vt)) {
        Log("Failed to resolve DXGI vtables via D3D12 dummy device.");
        return false;
    }

    Log("Resolved DXGI vtables: CreateSwapChainForHwnd=%p Present=%p Present1=%p ResizeBuffers=%p",
        vt.create_swapchain_hwnd,
        vt.present,
        vt.present1,
        vt.resize_buffers);

    if (MH_Initialize() != MH_OK) {
        Log("MH_Initialize failed.");
        return false;
    }

    const bool created =
        MH_CreateHook(vt.create_swapchain,
                      reinterpret_cast<void*>(&HookCreateSwapChain),
                      reinterpret_cast<void**>(&g_orig_create_swapchain)) == MH_OK &&
        MH_CreateHook(vt.create_swapchain_hwnd,
                      reinterpret_cast<void*>(&HookCreateSwapChainForHwnd),
                      reinterpret_cast<void**>(&g_orig_create_swapchain_hwnd)) == MH_OK &&
        MH_CreateHook(vt.present,
                      reinterpret_cast<void*>(&HookPresent),
                      reinterpret_cast<void**>(&g_orig_present)) == MH_OK &&
        MH_CreateHook(vt.present1,
                      reinterpret_cast<void*>(&HookPresent1),
                      reinterpret_cast<void**>(&g_orig_present1)) == MH_OK &&
        MH_CreateHook(vt.resize_buffers,
                      reinterpret_cast<void*>(&HookResizeBuffers),
                      reinterpret_cast<void**>(&g_orig_resize_buffers)) == MH_OK;

    if (!created) {
        Log("MH_CreateHook failed.");
        MH_Uninitialize();
        return false;
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        Log("MH_EnableHook failed.");
        MH_Uninitialize();
        return false;
    }

    Log("DXGI hooks enabled successfully.");
    return true;
}

void ShutdownDxgiHook() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_dual_output.Shutdown();
    std::lock_guard<std::mutex> lock(g_state_mtx);
    g_game_swapchain.Reset();
    g_game_queue.Reset();
}

}  // namespace gotham
