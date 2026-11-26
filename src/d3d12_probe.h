#pragma once

#include "split_seam_detector.h"

#include <d3d12.h>

namespace gotham {

/// Hooks ID3D12Resource::{Map, Unmap, Release} early to track CPU-mapped UPLOAD vertex buffers.
void InitD3D12ResourceHooks(ID3D12Device* device);

/// Hooks ID3D12CommandQueue::ExecuteCommandLists and ID3D12GraphicsCommandList methods.
void InitD3D12Probe(ID3D12CommandQueue* game_queue);

/// Marks internal DualOutput command recording/execution so it is excluded from game stats.
void SetD3D12InternalCall(bool internal_call);

/// Enables/disables centered Slate UI replication when active Duo Split mode is rendering.
void SetSubtitleSplitActive(bool active, uint32_t frame_w, uint32_t frame_h);

/// Consumes accumulated D3D12 viewport stats for the just-completed frame on game_queue.
FrameRenderStats ConsumeFrameRenderStats(ID3D12CommandQueue* game_queue,
                                         uint32_t frame_w,
                                         uint32_t frame_h);

/// Polls F2 and manages 2-frame detailed D3D12 diagnostic logging on Present.
void TickD3D12ProbeOnPresent(ID3D12CommandQueue* game_queue, bool internal_call);

}  // namespace gotham
