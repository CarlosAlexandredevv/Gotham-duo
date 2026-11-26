#pragma once

#include "split_seam_detector.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <optional>

namespace gotham {

using OrigPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

/// Manages the two borderless monitor output windows and D3D12 split/mirror copy pipeline.
class DualOutput {
public:
    DualOutput() = default;
    ~DualOutput();

    DualOutput(const DualOutput&) = delete;
    DualOutput& operator=(const DualOutput&) = delete;

    /// Initializes configuration and spawns the Win32 window thread.
    void Start(HMODULE self_module);

    /// Copies left/right halves (or mirrored full frame) of game_swapchain and presents both monitors.
    void PresentFrame(IDXGISwapChain3* game_swapchain,
                      ID3D12CommandQueue* game_queue,
                      OrigPresentFn orig_present,
                      std::optional<StripClassification> frame_sample = std::nullopt,
                      const FrameRenderStats* frame_stats = nullptr);

    /// Flushes in-flight copy work on game ResizeBuffers while keeping output swapchains alive.
    void OnGameResizeBuffers(DXGI_FORMAT new_format);

    /// Releases swapchains/resources (called on format change, fallback, or shutdown).
    void ResetSwapChains();

    /// Stops the window thread and frees all resources.
    void Shutdown();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace gotham
