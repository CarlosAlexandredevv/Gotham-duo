#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

namespace gotham {

using OrigPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

/// Manages the two borderless monitor output windows and D3D12 split copy pipeline.
class DualOutput {
public:
    DualOutput() = default;
    ~DualOutput();

    DualOutput(const DualOutput&) = delete;
    DualOutput& operator=(const DualOutput&) = delete;

    /// Initializes configuration and spawns the Win32 window thread.
    void Start(HMODULE self_module);

    /// Copies left/right halves of game_swapchain's backbuffer and presents both monitors.
    void PresentFrame(IDXGISwapChain3* game_swapchain,
                      ID3D12CommandQueue* game_queue,
                      OrigPresentFn orig_present);

    /// Releases swapchains/resources (called on ResizeBuffers or shutdown).
    void ResetSwapChains();

    /// Stops the window thread and frees all resources.
    void Shutdown();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace gotham
