#include "d3d12_probe.h"
#include "log.h"

#include <MinHook.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gotham {

namespace {

using ResourceReleaseFn = ULONG(STDMETHODCALLTYPE*)(ID3D12Resource*);
using ResourceMapFn =
    HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
using ResourceUnmapFn =
    void(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*);

using ExecuteCommandListsFn =
    void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using CmdCloseFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using CmdResetFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using DrawInstancedFn =
    void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIndexedInstancedFn =
    void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using CopyBufferRegionFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                                    ID3D12Resource*,
                                                    UINT64,
                                                    ID3D12Resource*,
                                                    UINT64,
                                                    UINT64);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                                ID3D12Resource*,
                                                ID3D12Resource*);
using RSSetViewportsFn =
    void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using RSSetScissorRectsFn =
    void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
using SetPipelineStateFn =
    void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using IASetIndexBufferFn =
    void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
using IASetVertexBuffersFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                                      UINT,
                                                      UINT,
                                                      const D3D12_VERTEX_BUFFER_VIEW*);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                                      UINT,
                                                      const D3D12_CPU_DESCRIPTOR_HANDLE*,
                                                      BOOL,
                                                      const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ExecuteIndirectFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,
                                                   ID3D12CommandSignature*,
                                                   UINT,
                                                   ID3D12Resource*,
                                                   UINT64,
                                                   ID3D12Resource*,
                                                   UINT64);

ResourceReleaseFn g_orig_res_release = nullptr;
ResourceMapFn g_orig_res_map = nullptr;
ResourceUnmapFn g_orig_res_unmap = nullptr;
ResourceReleaseFn g_orig_placed_release = nullptr;
ResourceMapFn g_orig_placed_map = nullptr;
ResourceUnmapFn g_orig_placed_unmap = nullptr;

ExecuteCommandListsFn g_orig_exec_lists = nullptr;
CmdCloseFn g_orig_cmd_close = nullptr;
CmdResetFn g_orig_cmd_reset = nullptr;
DrawInstancedFn g_orig_draw = nullptr;
DrawIndexedInstancedFn g_orig_draw_indexed = nullptr;
CopyBufferRegionFn g_orig_copy_buf = nullptr;
CopyBufferRegionFn g_orig_copy_buf_copyq = nullptr;
CopyResourceFn g_orig_copy_res = nullptr;
CopyResourceFn g_orig_copy_res_copyq = nullptr;
RSSetViewportsFn g_orig_rs_viewports = nullptr;
RSSetScissorRectsFn g_orig_rs_scissors = nullptr;
SetPipelineStateFn g_orig_set_pso = nullptr;
IASetIndexBufferFn g_orig_ia_ib = nullptr;
IASetVertexBuffersFn g_orig_ia_vb = nullptr;
OMSetRenderTargetsFn g_orig_om_set_rt = nullptr;
ExecuteIndirectFn g_orig_exec_indirect = nullptr;

// --- Upload buffer Map/Unmap/Release tracking ---
struct MappedUpload {
    ID3D12Resource* res = nullptr;
    UINT64 gpu_va = 0;
    UINT64 size = 0;
    const uint8_t* cpu_ptr = nullptr;
    uint32_t map_count = 1;
};

struct KnownBufferRange {
    UINT64 gpu_va = 0;
    UINT64 size = 0;
};

struct BufferSnapshot {
    UINT64 gpu_va = 0;
    uint64_t seq = 0;
    std::vector<uint8_t> data;
};

constexpr size_t kMaxSnapshots = 8192;
constexpr size_t kMaxSnapshotBytes = 64 * 1024 * 1024;

std::recursive_mutex g_map_mtx;
std::unordered_map<ID3D12Resource*, MappedUpload> g_mapped_by_res;
std::unordered_map<ID3D12Resource*, KnownBufferRange> g_known_buffers;
std::vector<MappedUpload> g_mapped_list;
std::map<UINT64, BufferSnapshot> g_snap_map;
uint64_t g_snapshot_seq = 0;
std::atomic<uint32_t> g_map_hits{0};
std::atomic<uint32_t> g_unmap_hits{0};
std::atomic<uint32_t> g_unmap_skip_big{0};
std::atomic<uint32_t> g_copy_hits{0};
std::atomic<uint32_t> g_copy_src_miss{0};
std::atomic<uint32_t> g_copyq_hits{0};

std::atomic<bool> g_sub_split_active{false};
std::atomic<uint32_t> g_sub_frame_w{0};
std::atomic<uint32_t> g_sub_frame_h{0};

void RebuildMappedListLocked() {
    g_mapped_list.clear();
    g_mapped_list.reserve(g_mapped_by_res.size());
    for (const auto& kv : g_mapped_by_res) {
        g_mapped_list.push_back(kv.second);
    }
}

std::map<UINT64, BufferSnapshot>::iterator FindSnapshotLocked(UINT64 gpu_va, size_t byte_count) {
    auto it = g_snap_map.upper_bound(gpu_va);
    if (it == g_snap_map.begin()) {
        return g_snap_map.end();
    }
    --it;
    if (gpu_va >= it->first && (gpu_va - it->first) + byte_count <= it->second.data.size()) {
        return it;
    }
    return g_snap_map.end();
}

void EraseSnapshotRangeLocked(UINT64 va, size_t len) {
    if (va == 0 || len == 0 || len > (UINT64_MAX - va)) {
        return;
    }
    const UINT64 del_end = va + len;
    auto it = g_snap_map.upper_bound(va);
    if (it != g_snap_map.begin()) {
        auto prev = std::prev(it);
        const UINT64 prev_end = prev->first + prev->second.data.size();
        if (prev_end > va) {
            if (prev_end > del_end) {
                const size_t tail_off = static_cast<size_t>(del_end - prev->first);
                std::vector<uint8_t> tail(prev->second.data.begin() + tail_off,
                                          prev->second.data.end());
                const uint64_t seq = prev->second.seq;
                if (va == prev->first) {
                    g_snap_map.erase(prev);
                } else {
                    prev->second.data.resize(static_cast<size_t>(va - prev->first));
                }
                BufferSnapshot& rhs = g_snap_map[del_end];
                rhs.gpu_va = del_end;
                rhs.seq = seq;
                rhs.data = std::move(tail);
                return;
            }
            if (va == prev->first) {
                g_snap_map.erase(prev);
            } else {
                prev->second.data.resize(static_cast<size_t>(va - prev->first));
            }
        }
    }
    while (it != g_snap_map.end() && it->first < del_end) {
        const UINT64 it_end = it->first + it->second.data.size();
        if (it_end <= del_end) {
            it = g_snap_map.erase(it);
            continue;
        }
        const size_t tail_off = static_cast<size_t>(del_end - it->first);
        std::vector<uint8_t> tail(it->second.data.begin() + tail_off, it->second.data.end());
        const uint64_t seq = it->second.seq;
        g_snap_map.erase(it);
        BufferSnapshot& rhs = g_snap_map[del_end];
        rhs.gpu_va = del_end;
        rhs.seq = seq;
        rhs.data = std::move(tail);
        break;
    }
}

void StoreSnapshotLocked(UINT64 gpu_va, const uint8_t* src_ptr, size_t len) {
    if (gpu_va == 0 || !src_ptr || len < 16 || len > kMaxSnapshotBytes ||
        len > (UINT64_MAX - gpu_va)) {
        return;
    }
    const UINT64 new_end = gpu_va + len;
    auto it = g_snap_map.upper_bound(gpu_va);
    if (it != g_snap_map.begin()) {
        auto prev = std::prev(it);
        const UINT64 prev_end = prev->first + prev->second.data.size();
        if (prev_end >= new_end) {
            std::memcpy(prev->second.data.data() + (gpu_va - prev->first), src_ptr, len);
            prev->second.seq = ++g_snapshot_seq;
            return;
        }
        if (prev_end >= gpu_va && (new_end - prev->first) <= kMaxSnapshotBytes) {
            UINT64 final_end = new_end;
            bool can_coalesce = true;
            auto scan = it;
            auto last_tail_it = g_snap_map.end();
            while (scan != g_snap_map.end() && scan->first <= final_end) {
                const UINT64 scan_end = scan->first + scan->second.data.size();
                if ((scan_end - prev->first) > kMaxSnapshotBytes) {
                    if (scan->first < final_end) {
                        can_coalesce = false;
                    }
                    break;
                }
                if (scan_end > final_end) {
                    final_end = scan_end;
                    last_tail_it = scan;
                }
                ++scan;
            }
            if (can_coalesce) {
                std::vector<uint8_t> tail;
                if (last_tail_it != g_snap_map.end() &&
                    (last_tail_it->first + last_tail_it->second.data.size()) > new_end) {
                    const size_t tail_off = static_cast<size_t>(new_end - last_tail_it->first);
                    tail.assign(last_tail_it->second.data.begin() + tail_off,
                                last_tail_it->second.data.end());
                }
                g_snap_map.erase(it, scan);
                prev->second.data.resize(static_cast<size_t>(final_end - prev->first));
                std::memcpy(prev->second.data.data() + (gpu_va - prev->first), src_ptr, len);
                if (!tail.empty()) {
                    std::memcpy(prev->second.data.data() + (new_end - prev->first),
                                tail.data(),
                                tail.size());
                }
                prev->second.seq = ++g_snapshot_seq;
                return;
            }
        }
    }

    EraseSnapshotRangeLocked(gpu_va, len);

    std::vector<uint8_t> tail;
    auto right = g_snap_map.find(new_end);
    if (right != g_snap_map.end() &&
        (right->first + right->second.data.size() - gpu_va) <= kMaxSnapshotBytes) {
        tail = std::move(right->second.data);
        g_snap_map.erase(right);
    }

    std::vector<uint8_t> recycled;
    if (g_snap_map.size() >= kMaxSnapshots) {
        auto oldest = g_snap_map.begin();
        for (auto scan = std::next(oldest); scan != g_snap_map.end(); ++scan) {
            if (scan->second.seq < oldest->second.seq) {
                oldest = scan;
            }
        }
        if (recycled.capacity() < oldest->second.data.capacity()) {
            recycled = std::move(oldest->second.data);
        }
        g_snap_map.erase(oldest);
    }

    BufferSnapshot& snap = g_snap_map[gpu_va];
    snap.gpu_va = gpu_va;
    snap.seq = ++g_snapshot_seq;
    snap.data = std::move(recycled);
    snap.data.resize(len + tail.size());
    std::memcpy(snap.data.data(), src_ptr, len);
    if (!tail.empty()) {
        std::memcpy(snap.data.data() + len, tail.data(), tail.size());
    }
}

bool ReadBytesLocked(UINT64 gpu_va, size_t byte_count, void* out_buf) {
    for (const auto& m : g_mapped_list) {
        if (gpu_va >= m.gpu_va && (gpu_va - m.gpu_va) + byte_count <= m.size) {
            std::memcpy(out_buf, m.cpu_ptr + (gpu_va - m.gpu_va), byte_count);
            return true;
        }
    }
    auto it = FindSnapshotLocked(gpu_va, byte_count);
    if (it != g_snap_map.end()) {
        std::memcpy(out_buf, it->second.data.data() + (gpu_va - it->first), byte_count);
        return true;
    }
    return false;
}

bool TryComputeSlateDrawBounds(UINT64 vb_addr,
                               UINT64 ib_addr,
                               DXGI_FORMAT ib_fmt,
                               UINT start_idx,
                               INT base_vtx,
                               UINT idx_count,
                               float frame_w,
                               float frame_h,
                               SlateRect& out_rect) {
    if (vb_addr == 0 || ib_addr == 0 || idx_count < 3 || idx_count > 12000) {
        return false;
    }
    const size_t ib_stride = (ib_fmt == DXGI_FORMAT_R32_UINT) ? 4 : 2;
    const size_t ib_bytes = static_cast<size_t>(idx_count) * ib_stride;
    thread_local std::vector<uint8_t> tl_ib;
    thread_local std::vector<uint8_t> tl_vb;
    tl_ib.resize(ib_bytes);

    INT64 min_v = 0;
    INT64 max_v = -1;
    size_t vb_bytes = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_map_mtx);
        if (!ReadBytesLocked(ib_addr + static_cast<UINT64>(start_idx) * ib_stride,
                             ib_bytes,
                             tl_ib.data())) {
            return false;
        }
        for (UINT k = 0; k < idx_count; ++k) {
            UINT raw_idx = 0;
            if (ib_stride == 4) {
                std::memcpy(&raw_idx, tl_ib.data() + k * 4, 4);
            } else {
                uint16_t idx16 = 0;
                std::memcpy(&idx16, tl_ib.data() + k * 2, 2);
                raw_idx = idx16;
            }
            const INT64 vi = static_cast<INT64>(base_vtx) + static_cast<INT64>(raw_idx);
            if (vi < 0) {
                return false;
            }
            if (k == 0 || vi < min_v) {
                min_v = vi;
            }
            if (vi > max_v) {
                max_v = vi;
            }
        }
        if (max_v < min_v || (max_v - min_v) > 8192) {
            return false;
        }
        vb_bytes = static_cast<size_t>(max_v - min_v + 1) * 44ULL;
        tl_vb.resize(vb_bytes);
        if (!ReadBytesLocked(vb_addr + static_cast<UINT64>(min_v) * 44ULL, vb_bytes, tl_vb.data())) {
            return false;
        }
    }

    const float half_w = 0.5f * frame_w;
    float min_x = 0.0f;
    float max_x = 0.0f;
    float min_y = 0.0f;
    float max_y = 0.0f;
    float tri_min_x = 0.0f;
    float tri_max_x = 0.0f;
    bool has_center_prim = false;
    bool first = true;
    for (UINT k = 0; k < idx_count; ++k) {
        UINT raw_idx = 0;
        if (ib_stride == 4) {
            std::memcpy(&raw_idx, tl_ib.data() + k * 4, 4);
        } else {
            uint16_t idx16 = 0;
            std::memcpy(&idx16, tl_ib.data() + k * 2, 2);
            raw_idx = idx16;
        }
        const size_t rel = static_cast<size_t>(static_cast<INT64>(base_vtx) + raw_idx - min_v);
        const size_t v_off = rel * 44ULL;
        if (v_off + 32 > vb_bytes) {
            return false;
        }
        float x = 0.0f;
        float y = 0.0f;
        std::memcpy(&x, tl_vb.data() + v_off + 24, sizeof(float));
        std::memcpy(&y, tl_vb.data() + v_off + 28, sizeof(float));
        if (!std::isfinite(x) || !std::isfinite(y) ||
            x < -64.0f || x > frame_w + 64.0f || y < -64.0f || y > frame_h + 64.0f) {
            return false;
        }
        if (first) {
            min_x = max_x = x;
            min_y = max_y = y;
            first = false;
        } else {
            min_x = (std::min)(min_x, x);
            max_x = (std::max)(max_x, x);
            min_y = (std::min)(min_y, y);
            max_y = (std::max)(max_y, y);
        }
        const UINT tri_lane = k % 3;
        if (tri_lane == 0) {
            tri_min_x = tri_max_x = x;
        } else {
            tri_min_x = (std::min)(tri_min_x, x);
            tri_max_x = (std::max)(tri_max_x, x);
            if (tri_lane == 2 && tri_max_x >= half_w - 48.0f && tri_min_x <= half_w + 48.0f) {
                has_center_prim = true;
            }
        }
    }
    if (first) {
        return false;
    }
    out_rect = SlateRect{min_x, max_x, min_y, max_y, has_center_prim, kTargetBoth};
    return true;
}

bool ReadMappedBytes(UINT64 gpu_va, size_t byte_count, void* out_buf) {
    if (gpu_va == 0 || byte_count == 0 || !out_buf) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(g_map_mtx);
    return ReadBytesLocked(gpu_va, byte_count, out_buf);
}

void CopySnapshotRange(ID3D12Resource* dst_res, UINT64 dst_va, UINT64 src_va, size_t num_bytes) {
    if (dst_va == 0 || src_va == 0 || num_bytes < 16 || num_bytes > kMaxSnapshotBytes) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_map_mtx);
    if (dst_res && g_known_buffers.find(dst_res) == g_known_buffers.end()) {
        const D3D12_RESOURCE_DESC desc = dst_res->GetDesc();
        const UINT64 base_va = static_cast<UINT64>(dst_res->GetGPUVirtualAddress());
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && base_va != 0) {
            g_known_buffers[dst_res] = KnownBufferRange{base_va, desc.Width};
        }
    }
    for (const auto& m : g_mapped_list) {
        if (src_va >= m.gpu_va && (src_va - m.gpu_va) + num_bytes <= m.size) {
            StoreSnapshotLocked(dst_va, m.cpu_ptr + (src_va - m.gpu_va), num_bytes);
            g_copy_hits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    auto it = FindSnapshotLocked(src_va, num_bytes);
    if (it != g_snap_map.end()) {
        static std::vector<uint8_t> s_copy_scratch;
        s_copy_scratch.resize(num_bytes);
        std::memcpy(
            s_copy_scratch.data(), it->second.data.data() + (src_va - it->first), num_bytes);
        StoreSnapshotLocked(dst_va, s_copy_scratch.data(), num_bytes);
        g_copy_hits.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    EraseSnapshotRangeLocked(dst_va, num_bytes);
    g_copy_src_miss.fetch_add(1, std::memory_order_relaxed);
}

HRESULT OnResourceMapImpl(ID3D12Resource* self,
                          UINT subresource,
                          const D3D12_RANGE* read_range,
                          void** pp_data,
                          ResourceMapFn orig_fn) {
    const HRESULT hr = orig_fn(self, subresource, read_range, pp_data);
    if (SUCCEEDED(hr) && subresource == 0) {
        std::lock_guard<std::recursive_mutex> lock(g_map_mtx);
        auto it = g_mapped_by_res.find(self);
        if (it != g_mapped_by_res.end()) {
            ++it->second.map_count;
            if (pp_data && *pp_data) {
                it->second.cpu_ptr = static_cast<const uint8_t*>(*pp_data);
                RebuildMappedListLocked();
            }
            return hr;
        }
        if (pp_data && *pp_data) {
            const D3D12_RESOURCE_DESC desc = self->GetDesc();
            if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && desc.Width >= 16) {
                D3D12_HEAP_PROPERTIES hp{};
                const HRESULT hhr = self->GetHeapProperties(&hp, nullptr);
                if (FAILED(hhr) || hp.Type != D3D12_HEAP_TYPE_READBACK) {
                    const UINT64 gpu_va = static_cast<UINT64>(self->GetGPUVirtualAddress());
                    if (gpu_va != 0) {
                        const size_t erase_sz = static_cast<size_t>(
                            (std::min)(desc.Width, static_cast<UINT64>(SIZE_MAX)));
                        EraseSnapshotRangeLocked(gpu_va, erase_sz);
                        g_mapped_by_res[self] = MappedUpload{
                            self, gpu_va, desc.Width, static_cast<const uint8_t*>(*pp_data), 1};
                        g_known_buffers[self] = KnownBufferRange{gpu_va, desc.Width};
                        RebuildMappedListLocked();
                        g_map_hits.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }
    return hr;
}

void OnResourceUnmapImpl(ID3D12Resource* self,
                         UINT subresource,
                         const D3D12_RANGE* written_range,
                         ResourceUnmapFn orig_fn) {
    if (subresource == 0) {
        std::lock_guard<std::recursive_mutex> lock(g_map_mtx);
        auto it = g_mapped_by_res.find(self);
        if (it != g_mapped_by_res.end()) {
            if (it->second.map_count > 1) {
                --it->second.map_count;
            } else {
                const MappedUpload& m = it->second;
                size_t off = 0;
                size_t len = static_cast<size_t>(m.size);
                if (written_range && written_range->End > written_range->Begin &&
                    written_range->End <= m.size) {
                    off = written_range->Begin;
                    len = written_range->End - written_range->Begin;
                }
                if (len > kMaxSnapshotBytes) {
                    g_unmap_skip_big.fetch_add(1, std::memory_order_relaxed);
                }
                StoreSnapshotLocked(m.gpu_va + off, m.cpu_ptr + off, len);
                g_mapped_by_res.erase(it);
                RebuildMappedListLocked();
                g_unmap_hits.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    orig_fn(self, subresource, written_range);
}

ULONG OnResourceReleaseImpl(ID3D12Resource* self, ResourceReleaseFn orig_fn) {
    {
        std::lock_guard<std::recursive_mutex> lock(g_map_mtx);
        auto kit = g_known_buffers.find(self);
        if (kit != g_known_buffers.end() || g_mapped_by_res.find(self) != g_mapped_by_res.end()) {
            const ULONG rc = orig_fn(self);
            if (rc == 0) {
                if (kit != g_known_buffers.end()) {
                    const size_t erase_sz = static_cast<size_t>(
                        (std::min)(kit->second.size, static_cast<UINT64>(SIZE_MAX)));
                    EraseSnapshotRangeLocked(kit->second.gpu_va, erase_sz);
                    g_known_buffers.erase(kit);
                }
                if (g_mapped_by_res.erase(self) > 0) {
                    RebuildMappedListLocked();
                }
            }
            return rc;
        }
    }
    return orig_fn(self);
}

HRESULT STDMETHODCALLTYPE HookResourceMap(ID3D12Resource* self,
                                          UINT subresource,
                                          const D3D12_RANGE* read_range,
                                          void** pp_data) {
    return OnResourceMapImpl(self, subresource, read_range, pp_data, g_orig_res_map);
}

void STDMETHODCALLTYPE HookResourceUnmap(ID3D12Resource* self,
                                         UINT subresource,
                                         const D3D12_RANGE* written_range) {
    OnResourceUnmapImpl(self, subresource, written_range, g_orig_res_unmap);
}

ULONG STDMETHODCALLTYPE HookResourceRelease(ID3D12Resource* self) {
    return OnResourceReleaseImpl(self, g_orig_res_release);
}

HRESULT STDMETHODCALLTYPE HookPlacedResourceMap(ID3D12Resource* self,
                                                UINT subresource,
                                                const D3D12_RANGE* read_range,
                                                void** pp_data) {
    return OnResourceMapImpl(self, subresource, read_range, pp_data, g_orig_placed_map);
}

void STDMETHODCALLTYPE HookPlacedResourceUnmap(ID3D12Resource* self,
                                               UINT subresource,
                                               const D3D12_RANGE* written_range) {
    OnResourceUnmapImpl(self, subresource, written_range, g_orig_placed_unmap);
}

ULONG STDMETHODCALLTYPE HookPlacedResourceRelease(ID3D12Resource* self) {
    return OnResourceReleaseImpl(self, g_orig_placed_release);
}

// --- Lightweight per-command-list structural tracking + optional F2 probe dump ---
struct PassBucket {
    D3D12_VIEWPORT vp{};
    D3D12_RECT sc{};
    UINT64 rtv0 = 0;
    UINT num_rtv = 0;
    bool has_dsv = false;
    uint32_t draws = 0;
    uint32_t indirects = 0;
    UINT sample_count = 0;
    UINT start_idx = 0;
    INT base_vtx = 0;
    UINT vb_stride = 0;
    UINT64 vb_addr = 0;
    UINT64 ib_addr = 0;
    DXGI_FORMAT ib_fmt = DXGI_FORMAT_UNKNOWN;
    ID3D12PipelineState* last_pso = nullptr;
    bool has_vtx_sample = false;
    bool ib_hit = false;
    INT64 sample_i0 = -1;
    INT64 sample_i2 = -1;
    float vtx0[8]{};
    float vtx2[8]{};
    UINT64 vtx_miss_vb = 0;
};

struct ClSummary {
    std::vector<ViewportDrawSummary> viewports;
};

struct CmdListTrace {
    D3D12_VIEWPORT cur_vp{};
    UINT cur_vp_count = 0;
    D3D12_RECT cur_sc{};
    UINT cur_sc_count = 0;
    UINT64 cur_rtv0 = 0;
    UINT cur_num_rtv = 0;
    bool cur_has_dsv = false;
    UINT cur_vb_stride = 0;
    UINT64 cur_vb_addr = 0;
    UINT64 cur_ib_addr = 0;
    DXGI_FORMAT cur_ib_fmt = DXGI_FORMAT_UNKNOWN;
    ID3D12PipelineState* cur_pso = nullptr;
    std::vector<ViewportDrawSummary> vp_summary;
    std::vector<PassBucket> probe_buckets;
    std::vector<SlateRect> centered_boxes;
    uint32_t overflow_draws = 0;

    void ClearState(ID3D12PipelineState* pso) {
        cur_vp = {};
        cur_vp_count = 0;
        cur_sc = {};
        cur_sc_count = 0;
        cur_rtv0 = 0;
        cur_num_rtv = 0;
        cur_has_dsv = false;
        cur_vb_stride = 0;
        cur_vb_addr = 0;
        cur_ib_addr = 0;
        cur_ib_fmt = DXGI_FORMAT_UNKNOWN;
        cur_pso = pso;
        vp_summary.clear();
        probe_buckets.clear();
        centered_boxes.clear();
        overflow_draws = 0;
    }

    void RecordViewportSummary() {
        const int x = static_cast<int>(std::lround(cur_vp.TopLeftX));
        const int y = static_cast<int>(std::lround(cur_vp.TopLeftY));
        const int w = static_cast<int>(std::lround(cur_vp.Width));
        const int h = static_cast<int>(std::lround(cur_vp.Height));
        for (auto& e : vp_summary) {
            if (e.x == x && e.y == y && e.w == w && e.h == h && e.has_dsv == cur_has_dsv) {
                ++e.draws;
                return;
            }
        }
        if (vp_summary.size() < 64) {
            vp_summary.push_back(ViewportDrawSummary{x, y, w, h, 1, cur_has_dsv});
        }
    }

    void RecordDraw(bool is_indirect, UINT count, UINT start_idx, INT base_vtx, bool probe_on) {
        RecordViewportSummary();
        if (!probe_on) {
            return;
        }

        const bool is_slate_ui = (cur_vb_stride == 44 && cur_vp.Width >= 1900.0f);
        if (!is_slate_ui && !probe_buckets.empty()) {
            PassBucket& b = probe_buckets.back();
            if (b.vp.TopLeftX == cur_vp.TopLeftX && b.vp.TopLeftY == cur_vp.TopLeftY &&
                b.vp.Width == cur_vp.Width && b.vp.Height == cur_vp.Height &&
                b.sc.left == cur_sc.left && b.sc.top == cur_sc.top &&
                b.sc.right == cur_sc.right && b.sc.bottom == cur_sc.bottom &&
                b.rtv0 == cur_rtv0 && b.num_rtv == cur_num_rtv && b.has_dsv == cur_has_dsv) {
                if (is_indirect) {
                    ++b.indirects;
                } else {
                    ++b.draws;
                }
                b.last_pso = cur_pso;
                return;
            }
        }
        if (probe_buckets.size() >= 512) {
            ++overflow_draws;
            return;
        }
        PassBucket b{};
        b.vp = cur_vp;
        b.sc = cur_sc;
        b.rtv0 = cur_rtv0;
        b.num_rtv = cur_num_rtv;
        b.has_dsv = cur_has_dsv;
        b.draws = is_indirect ? 0 : 1;
        b.indirects = is_indirect ? 1 : 0;
        b.sample_count = count;
        b.start_idx = start_idx;
        b.base_vtx = base_vtx;
        b.vb_stride = cur_vb_stride;
        b.vb_addr = cur_vb_addr;
        b.ib_addr = cur_ib_addr;
        b.ib_fmt = cur_ib_fmt;
        b.last_pso = cur_pso;
        if (is_slate_ui && cur_vb_addr != 0) {
            INT64 i0 = static_cast<INT64>(base_vtx);
            INT64 i2 = static_cast<INT64>(base_vtx) + 2;
            if (cur_ib_addr != 0 && count >= 3) {
                const size_t ib_stride = (cur_ib_fmt == DXGI_FORMAT_R32_UINT) ? 4 : 2;
                const UINT64 ib_off = static_cast<UINT64>(start_idx) * ib_stride;
                if (ib_stride == 4) {
                    uint32_t idx_buf[3]{};
                    if (ReadMappedBytes(cur_ib_addr + ib_off, sizeof(idx_buf), idx_buf)) {
                        b.ib_hit = true;
                        i0 = static_cast<INT64>(base_vtx) + static_cast<INT64>(idx_buf[0]);
                        i2 = static_cast<INT64>(base_vtx) + static_cast<INT64>(idx_buf[2]);
                    }
                } else {
                    uint16_t idx_buf[3]{};
                    if (ReadMappedBytes(cur_ib_addr + ib_off, sizeof(idx_buf), idx_buf)) {
                        b.ib_hit = true;
                        i0 = static_cast<INT64>(base_vtx) + static_cast<INT64>(idx_buf[0]);
                        i2 = static_cast<INT64>(base_vtx) + static_cast<INT64>(idx_buf[2]);
                    }
                }
            }
            b.sample_i0 = i0;
            b.sample_i2 = i2;
            if (i0 >= 0 && i2 >= 0) {
                const UINT64 addr0 = cur_vb_addr + static_cast<UINT64>(i0) * 44ULL;
                const UINT64 addr2 = cur_vb_addr + static_cast<UINT64>(i2) * 44ULL;
                if (ReadMappedBytes(addr0, sizeof(b.vtx0), b.vtx0) &&
                    ReadMappedBytes(addr2, sizeof(b.vtx2), b.vtx2)) {
                    b.has_vtx_sample = true;
                }
            }
            if (!b.has_vtx_sample) {
                b.vtx_miss_vb = cur_vb_addr;
            }
        }
        probe_buckets.push_back(b);
    }
};

thread_local bool tl_internal_call = false;

struct TraceShard {
    std::mutex mtx;
    std::unordered_map<ID3D12GraphicsCommandList*, CmdListTrace> traces;
};

constexpr size_t kTraceShards = 16;
TraceShard g_trace_shards[kTraceShards];

TraceShard& GetTraceShard(ID3D12GraphicsCommandList* cl) {
    const size_t idx = (reinterpret_cast<uintptr_t>(cl) >> 4) & (kTraceShards - 1);
    return g_trace_shards[idx];
}

std::once_flag g_probe_init_once;
std::atomic<bool> g_probe_installed{false};
std::atomic<bool> g_probe_active{false};
std::atomic<ID3D12CommandQueue*> g_target_queue{nullptr};

std::mutex g_cl_mtx;
std::unordered_map<ID3D12GraphicsCommandList*, ClSummary> g_closed_summaries;
std::unordered_map<ID3D12GraphicsCommandList*, std::pair<std::vector<PassBucket>, uint32_t>>
    g_closed_probe_dumps;
std::vector<ClSummary> g_frame_cls;
int g_probe_frames_left = 0;
uint32_t g_probe_seq = 0;
uint32_t g_exec_seq = 0;
bool g_prev_f2_down = false;

HRESULT STDMETHODCALLTYPE HookCmdReset(ID3D12GraphicsCommandList* self,
                                       ID3D12CommandAllocator* alloc,
                                       ID3D12PipelineState* pso) {
    if (!tl_internal_call) {
        {
            auto& shard = GetTraceShard(self);
            std::lock_guard<std::mutex> slock(shard.mtx);
            shard.traces[self].ClearState(pso);
        }
        std::lock_guard<std::mutex> lock(g_cl_mtx);
        g_closed_summaries.erase(self);
        g_closed_probe_dumps.erase(self);
    }
    return g_orig_cmd_reset(self, alloc, pso);
}

HRESULT STDMETHODCALLTYPE HookCmdClose(ID3D12GraphicsCommandList* self) {
    if (!tl_internal_call) {
        bool found = false;
        ClSummary summary{};
        std::vector<PassBucket> buckets;
        uint32_t overflow = 0;
        {
            auto& shard = GetTraceShard(self);
            std::lock_guard<std::mutex> slock(shard.mtx);
            auto it = shard.traces.find(self);
            if (it != shard.traces.end()) {
                found = true;
                summary.viewports = std::move(it->second.vp_summary);
                buckets = std::move(it->second.probe_buckets);
                overflow = it->second.overflow_draws;
                shard.traces.erase(it);
            }
        }
        if (found) {
            std::lock_guard<std::mutex> lock(g_cl_mtx);
            g_closed_summaries[self] = std::move(summary);
            if (!buckets.empty() || overflow > 0) {
                g_closed_probe_dumps[self] = std::make_pair(std::move(buckets), overflow);
            } else {
                g_closed_probe_dumps.erase(self);
            }
        }
    }
    return g_orig_cmd_close(self);
}

void STDMETHODCALLTYPE HookRSSetViewports(ID3D12GraphicsCommandList* self,
                                          UINT num,
                                          const D3D12_VIEWPORT* vps) {
    if (!tl_internal_call && num > 0 && vps) {
        auto& shard = GetTraceShard(self);
        std::lock_guard<std::mutex> slock(shard.mtx);
        auto& t = shard.traces[self];
        t.cur_vp = vps[0];
        t.cur_vp_count = num;
    }
    g_orig_rs_viewports(self, num, vps);
}

void STDMETHODCALLTYPE HookRSSetScissorRects(ID3D12GraphicsCommandList* self,
                                             UINT num,
                                             const D3D12_RECT* rects) {
    if (!tl_internal_call && num > 0 && rects) {
        auto& shard = GetTraceShard(self);
        std::lock_guard<std::mutex> slock(shard.mtx);
        auto& t = shard.traces[self];
        t.cur_sc = rects[0];
        t.cur_sc_count = num;
    }
    g_orig_rs_scissors(self, num, rects);
}

void STDMETHODCALLTYPE HookSetPipelineState(ID3D12GraphicsCommandList* self,
                                            ID3D12PipelineState* pso) {
    if (!tl_internal_call) {
        auto& shard = GetTraceShard(self);
        std::lock_guard<std::mutex> slock(shard.mtx);
        shard.traces[self].cur_pso = pso;
    }
    g_orig_set_pso(self, pso);
}

void STDMETHODCALLTYPE HookIASetIndexBuffer(ID3D12GraphicsCommandList* self,
                                             const D3D12_INDEX_BUFFER_VIEW* view) {
    if (!tl_internal_call) {
        auto& shard = GetTraceShard(self);
        std::lock_guard<std::mutex> slock(shard.mtx);
        auto& t = shard.traces[self];
        if (view) {
            t.cur_ib_addr = static_cast<UINT64>(view->BufferLocation);
            t.cur_ib_fmt = view->Format;
        } else {
            t.cur_ib_addr = 0;
            t.cur_ib_fmt = DXGI_FORMAT_UNKNOWN;
        }
    }
    g_orig_ia_ib(self, view);
}

void STDMETHODCALLTYPE HookIASetVertexBuffers(ID3D12GraphicsCommandList* self,
                                              UINT start_slot,
                                              UINT num_views,
                                              const D3D12_VERTEX_BUFFER_VIEW* views) {
    if (!tl_internal_call && start_slot == 0 && num_views > 0 && views) {
        auto& shard = GetTraceShard(self);
        std::lock_guard<std::mutex> slock(shard.mtx);
        auto& t = shard.traces[self];
        t.cur_vb_stride = views[0].StrideInBytes;
        t.cur_vb_addr = static_cast<UINT64>(views[0].BufferLocation);
    }
    g_orig_ia_vb(self, start_slot, num_views, views);
}

void STDMETHODCALLTYPE HookOMSetRenderTargets(ID3D12GraphicsCommandList* self,
                                              UINT num_rtv,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                              BOOL single_handle,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    if (!tl_internal_call) {
        auto& shard = GetTraceShard(self);
        std::lock_guard<std::mutex> slock(shard.mtx);
        auto& t = shard.traces[self];
        t.cur_num_rtv = num_rtv;
        t.cur_rtv0 = (num_rtv > 0 && rtvs) ? static_cast<UINT64>(rtvs[0].ptr) : 0;
        t.cur_has_dsv = (dsv != nullptr && dsv->ptr != 0);
    }
    g_orig_om_set_rt(self, num_rtv, rtvs, single_handle, dsv);
}

void STDMETHODCALLTYPE HookDrawInstanced(ID3D12GraphicsCommandList* self,
                                         UINT vtx_count,
                                         UINT inst_count,
                                         UINT start_vtx,
                                         UINT start_inst) {
    if (!tl_internal_call) {
        const bool probe_on = g_probe_active.load(std::memory_order_acquire);
        auto& shard = GetTraceShard(self);
        std::lock_guard<std::mutex> slock(shard.mtx);
        shard.traces[self].RecordDraw(false, vtx_count, start_vtx, 0, probe_on);
    }
    g_orig_draw(self, vtx_count, inst_count, start_vtx, start_inst);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D12GraphicsCommandList* self,
                                                UINT idx_count,
                                                UINT inst_count,
                                                UINT start_idx,
                                                INT base_vtx,
                                                UINT start_inst) {
    if (!tl_internal_call) {
        const bool probe_on = g_probe_active.load(std::memory_order_acquire);
        D3D12_VIEWPORT cur_vp{};
        D3D12_RECT cur_sc{};
        UINT64 vb_addr = 0;
        UINT64 ib_addr = 0;
        DXGI_FORMAT ib_fmt = DXGI_FORMAT_UNKNOWN;
        bool candidate_slate = false;
        auto& shard = GetTraceShard(self);
        {
            std::lock_guard<std::mutex> slock(shard.mtx);
            auto& t = shard.traces[self];
            t.RecordDraw(false, idx_count, start_idx, base_vtx, probe_on);
            if (g_sub_split_active.load(std::memory_order_acquire) && t.cur_vb_stride == 44 &&
                !t.cur_has_dsv && t.cur_num_rtv > 0 && t.cur_vp_count == 1 && t.cur_sc_count == 1 &&
                inst_count == 1 && idx_count >= 3 && idx_count <= 12000 && t.cur_vb_addr != 0 &&
                t.cur_ib_addr != 0) {
                cur_vp = t.cur_vp;
                cur_sc = t.cur_sc;
                vb_addr = t.cur_vb_addr;
                ib_addr = t.cur_ib_addr;
                ib_fmt = t.cur_ib_fmt;
                candidate_slate = true;
            }
        }

        if (candidate_slate) {
            const uint32_t fw = g_sub_frame_w.load(std::memory_order_relaxed);
            const uint32_t fh = g_sub_frame_h.load(std::memory_order_relaxed);
            const float fw_f = static_cast<float>(fw);
            const float fh_f = static_cast<float>(fh);
            const LONG half_wi = static_cast<LONG>(fw / 2);
            const bool vp_ok = fw >= 3000 && fh >= 32 && std::fabs(cur_vp.TopLeftX) <= 2.0f &&
                               std::fabs(cur_vp.TopLeftY) <= 2.0f && cur_vp.Width >= fw_f - 4.0f &&
                               cur_vp.Height >= fh_f - 4.0f;
            const bool sc_valid = cur_sc.right > cur_sc.left && cur_sc.bottom > cur_sc.top &&
                                  cur_sc.right > 0 && cur_sc.bottom > 0 &&
                                  cur_sc.left < static_cast<LONG>(fw) &&
                                  cur_sc.top < static_cast<LONG>(fh);
            const bool sc_left = cur_sc.left < half_wi - 2;
            const bool sc_right = cur_sc.right > half_wi + 2;
            const uint8_t sc_mask = static_cast<uint8_t>((sc_left ? kTargetLeft : 0) |
                                                         (sc_right ? kTargetRight : 0));
            const bool vp_sc_ok = vp_ok && sc_valid && sc_mask != kTargetNone;

            SlateRect rect{};
            const bool bounds_ok =
                vp_sc_ok && TryComputeSlateDrawBounds(vb_addr,
                                                      ib_addr,
                                                      ib_fmt,
                                                      start_idx,
                                                      base_vtx,
                                                      idx_count,
                                                      fw_f,
                                                      fh_f,
                                                      rect);
            if (probe_on && vp_sc_ok && !bounds_ok) {
                static std::atomic<uint32_t> s_last_skip_seq{0};
                if (s_last_skip_seq.exchange(g_probe_seq, std::memory_order_relaxed) !=
                    g_probe_seq) {
                    Log("[PROBE-SUB-SKIP] fw=%u fh=%u vp=(%.0f,%.0f %.0fx%.0f) sc=(%ld,%ld,%ld,%ld) vb=0x%llX ib=0x%llX sidx=%u cnt=%u",
                        fw,
                        fh,
                        cur_vp.TopLeftX,
                        cur_vp.TopLeftY,
                        cur_vp.Width,
                        cur_vp.Height,
                        cur_sc.left,
                        cur_sc.top,
                        cur_sc.right,
                        cur_sc.bottom,
                        static_cast<unsigned long long>(vb_addr),
                        static_cast<unsigned long long>(ib_addr),
                        start_idx,
                        idx_count);
                }
            }
            if (bounds_ok) {
                rect.target_mask = sc_mask;
                CenteredUiMatch match = CenteredUiMatch::None;
                uint8_t target_mask = kTargetNone;
                {
                    std::lock_guard<std::mutex> slock(shard.mtx);
                    match = ClassifyCenteredSlateRect(
                        rect, fw, fh, shard.traces[self].centered_boxes, &target_mask);
                }
                if (match != CenteredUiMatch::None && target_mask != kTargetNone) {
                    const ReplicatedScissors rep_sc = ComputeReplicatedScissors(
                        ScissorBox{cur_sc.left, cur_sc.top, cur_sc.right, cur_sc.bottom},
                        fw,
                        fh,
                        target_mask);
                    if (rep_sc.draw_left || rep_sc.draw_right) {
                        if (probe_on) {
                            Log("[PROBE-REP] sidx=%u cnt=%u rect=(%.1f..%.1f,%.1f..%.1f) m=%d mask=%u",
                                start_idx,
                                idx_count,
                                rect.min_x,
                                rect.max_x,
                                rect.min_y,
                                rect.max_y,
                                static_cast<int>(match),
                                static_cast<unsigned>(target_mask));
                        }
                        const float quarter_w = 0.25f * fw_f;

                        if (rep_sc.draw_left) {
                            D3D12_VIEWPORT vp_l = cur_vp;
                            vp_l.TopLeftX = cur_vp.TopLeftX - quarter_w;
                            D3D12_RECT sc_l{rep_sc.left.left,
                                            rep_sc.left.top,
                                            rep_sc.left.right,
                                            rep_sc.left.bottom};
                            g_orig_rs_viewports(self, 1, &vp_l);
                            g_orig_rs_scissors(self, 1, &sc_l);
                            g_orig_draw_indexed(
                                self, idx_count, inst_count, start_idx, base_vtx, start_inst);
                        }

                        if (rep_sc.draw_right) {
                            D3D12_VIEWPORT vp_r = cur_vp;
                            vp_r.TopLeftX = cur_vp.TopLeftX + quarter_w;
                            D3D12_RECT sc_r{rep_sc.right.left,
                                            rep_sc.right.top,
                                            rep_sc.right.right,
                                            rep_sc.right.bottom};
                            g_orig_rs_viewports(self, 1, &vp_r);
                            g_orig_rs_scissors(self, 1, &sc_r);
                            g_orig_draw_indexed(
                                self, idx_count, inst_count, start_idx, base_vtx, start_inst);
                        }

                        g_orig_rs_viewports(self, 1, &cur_vp);
                        g_orig_rs_scissors(self, 1, &cur_sc);
                        return;
                    }
                }
            }
        }
    }
    g_orig_draw_indexed(self, idx_count, inst_count, start_idx, base_vtx, start_inst);
}

void STDMETHODCALLTYPE HookExecuteIndirect(ID3D12GraphicsCommandList* self,
                                           ID3D12CommandSignature* sig,
                                           UINT max_count,
                                           ID3D12Resource* arg_buf,
                                           UINT64 arg_off,
                                           ID3D12Resource* cnt_buf,
                                           UINT64 cnt_off) {
    if (!tl_internal_call) {
        const bool probe_on = g_probe_active.load(std::memory_order_acquire);
        auto& shard = GetTraceShard(self);
        std::lock_guard<std::mutex> slock(shard.mtx);
        shard.traces[self].RecordDraw(true, max_count, 0, 0, probe_on);
    }
    g_orig_exec_indirect(self, sig, max_count, arg_buf, arg_off, cnt_buf, cnt_off);
}

void CopyWholeBufferSnapshot(ID3D12Resource* dst_res, ID3D12Resource* src_res) {
    if (!dst_res || !src_res) {
        return;
    }
    const D3D12_RESOURCE_DESC dst_desc = dst_res->GetDesc();
    if (dst_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && dst_desc.Width >= 16 &&
        dst_desc.Width <= kMaxSnapshotBytes) {
        CopySnapshotRange(dst_res,
                          static_cast<UINT64>(dst_res->GetGPUVirtualAddress()),
                          static_cast<UINT64>(src_res->GetGPUVirtualAddress()),
                          static_cast<size_t>(dst_desc.Width));
    }
}

void STDMETHODCALLTYPE HookCopyResource(ID3D12GraphicsCommandList* self,
                                        ID3D12Resource* dst_res,
                                        ID3D12Resource* src_res) {
    if (!tl_internal_call) {
        CopyWholeBufferSnapshot(dst_res, src_res);
    }
    g_orig_copy_res(self, dst_res, src_res);
}

void STDMETHODCALLTYPE HookCopyResourceCopyQ(ID3D12GraphicsCommandList* self,
                                             ID3D12Resource* dst_res,
                                             ID3D12Resource* src_res) {
    if (!tl_internal_call) {
        g_copyq_hits.fetch_add(1, std::memory_order_relaxed);
        CopyWholeBufferSnapshot(dst_res, src_res);
    }
    g_orig_copy_res_copyq(self, dst_res, src_res);
}

void STDMETHODCALLTYPE HookCopyBufferRegion(ID3D12GraphicsCommandList* self,
                                            ID3D12Resource* dst_buf,
                                            UINT64 dst_off,
                                            ID3D12Resource* src_buf,
                                            UINT64 src_off,
                                            UINT64 num_bytes) {
    if (!tl_internal_call && dst_buf && src_buf && num_bytes >= 16 &&
        num_bytes <= kMaxSnapshotBytes) {
        const UINT64 src_va = static_cast<UINT64>(src_buf->GetGPUVirtualAddress()) + src_off;
        const UINT64 dst_va = static_cast<UINT64>(dst_buf->GetGPUVirtualAddress()) + dst_off;
        CopySnapshotRange(dst_buf, dst_va, src_va, static_cast<size_t>(num_bytes));
    }
    g_orig_copy_buf(self, dst_buf, dst_off, src_buf, src_off, num_bytes);
}

void STDMETHODCALLTYPE HookCopyBufferRegionCopyQ(ID3D12GraphicsCommandList* self,
                                                 ID3D12Resource* dst_buf,
                                                 UINT64 dst_off,
                                                 ID3D12Resource* src_buf,
                                                 UINT64 src_off,
                                                 UINT64 num_bytes) {
    if (!tl_internal_call && dst_buf && src_buf && num_bytes >= 16 &&
        num_bytes <= kMaxSnapshotBytes) {
        g_copyq_hits.fetch_add(1, std::memory_order_relaxed);
        const UINT64 src_va = static_cast<UINT64>(src_buf->GetGPUVirtualAddress()) + src_off;
        const UINT64 dst_va = static_cast<UINT64>(dst_buf->GetGPUVirtualAddress()) + dst_off;
        CopySnapshotRange(dst_buf, dst_va, src_va, static_cast<size_t>(num_bytes));
    }
    g_orig_copy_buf_copyq(self, dst_buf, dst_off, src_buf, src_off, num_bytes);
}

void STDMETHODCALLTYPE HookExecuteCommandLists(ID3D12CommandQueue* self,
                                               UINT num_lists,
                                               ID3D12CommandList* const* lists) {
    if (!tl_internal_call && self == g_target_queue.load() && lists && num_lists > 0) {
        const bool probe_on = g_probe_active.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> lock(g_cl_mtx);
        const uint32_t exec_id = probe_on ? ++g_exec_seq : 0;
        for (UINT i = 0; i < num_lists; ++i) {
            auto* gcl = static_cast<ID3D12GraphicsCommandList*>(lists[i]);
            auto sit = g_closed_summaries.find(gcl);
            if (sit != g_closed_summaries.end()) {
                if (!sit->second.viewports.empty()) {
                    g_frame_cls.push_back(std::move(sit->second));
                }
                g_closed_summaries.erase(sit);
            }
            if (probe_on) {
                auto pit = g_closed_probe_dumps.find(gcl);
                if (pit != g_closed_probe_dumps.end()) {
                    const auto& buckets = pit->second.first;
                    for (size_t k = 0; k < buckets.size(); ++k) {
                        const PassBucket& b = buckets[k];
                        Log("[PROBE] exec=%u cl=%u/%u(%p) pass=%zu vp=(%.0f,%.0f %.0fx%.0f) sc=(%ld,%ld,%ld,%ld) rtv0=0x%llX n=%u dsv=%d draws=%u ind=%u cnt=%u sidx=%u bvtx=%d vbs=%u pso=%p",
                            exec_id,
                            i,
                            num_lists,
                            static_cast<void*>(gcl),
                            k,
                            b.vp.TopLeftX,
                            b.vp.TopLeftY,
                            b.vp.Width,
                            b.vp.Height,
                            b.sc.left,
                            b.sc.top,
                            b.sc.right,
                            b.sc.bottom,
                            static_cast<unsigned long long>(b.rtv0),
                            b.num_rtv,
                            b.has_dsv ? 1 : 0,
                            b.draws,
                            b.indirects,
                            b.sample_count,
                            b.start_idx,
                            b.base_vtx,
                            b.vb_stride,
                            static_cast<void*>(b.last_pso));
                        if (b.has_vtx_sample) {
                            Log("[PROBE-VTX] pass=%zu vb=0x%llX ib=0x%llX ib_hit=%d i=(%lld,%lld) v0=(%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f) v2=(%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f)",
                                k,
                                static_cast<unsigned long long>(b.vb_addr),
                                static_cast<unsigned long long>(b.ib_addr),
                                b.ib_hit ? 1 : 0,
                                static_cast<long long>(b.sample_i0),
                                static_cast<long long>(b.sample_i2),
                                b.vtx0[0], b.vtx0[1], b.vtx0[2], b.vtx0[3],
                                b.vtx0[4], b.vtx0[5], b.vtx0[6], b.vtx0[7],
                                b.vtx2[0], b.vtx2[1], b.vtx2[2], b.vtx2[3],
                                b.vtx2[4], b.vtx2[5], b.vtx2[6], b.vtx2[7]);
                        } else if (b.vtx_miss_vb != 0) {
                            static uint32_t s_last_miss_seq = 0;
                            if (s_last_miss_seq != g_probe_seq) {
                                s_last_miss_seq = g_probe_seq;
                                size_t mapped_cnt = 0;
                                size_t snap_cnt = 0;
                                std::vector<MappedUpload> mapped_copy;
                                {
                                    std::lock_guard<std::recursive_mutex> mlock(g_map_mtx);
                                    mapped_cnt = g_mapped_list.size();
                                    snap_cnt = g_snap_map.size();
                                    mapped_copy = g_mapped_list;
                                }
                                Log("[PROBE-VTX-MISS] pass=%zu vb=0x%llX ib=0x%llX ib_hit=%d ib_fmt=%u sub_active=%d mapped=%zu snaps=%zu maps=%u unmaps=%u big=%u copies=%u cmiss=%u copyq=%u",
                                    k,
                                    static_cast<unsigned long long>(b.vtx_miss_vb),
                                    static_cast<unsigned long long>(b.ib_addr),
                                    b.ib_hit ? 1 : 0,
                                    static_cast<unsigned>(b.ib_fmt),
                                    g_sub_split_active.load(std::memory_order_relaxed) ? 1 : 0,
                                    mapped_cnt,
                                    snap_cnt,
                                    g_map_hits.load(std::memory_order_relaxed),
                                    g_unmap_hits.load(std::memory_order_relaxed),
                                    g_unmap_skip_big.load(std::memory_order_relaxed),
                                    g_copy_hits.load(std::memory_order_relaxed),
                                    g_copy_src_miss.load(std::memory_order_relaxed),
                                    g_copyq_hits.load(std::memory_order_relaxed));
                                for (size_t mi = 0; mi < mapped_copy.size() && mi < 12; ++mi) {
                                    Log("[PROBE-MAP] [%zu] va=0x%llX size=%llu",
                                        mi,
                                        static_cast<unsigned long long>(mapped_copy[mi].gpu_va),
                                        static_cast<unsigned long long>(mapped_copy[mi].size));
                                }
                            }
                        }
                    }
                    if (pit->second.second > 0) {
                        Log("[PROBE] exec=%u cl=%u(%p) overflow_buckets_skipped=%u",
                            exec_id,
                            i,
                            static_cast<void*>(gcl),
                            pit->second.second);
                    }
                    g_closed_probe_dumps.erase(pit);
                }
            }
        }
    }
    g_orig_exec_lists(self, num_lists, lists);
}

bool HookAndEnable(void* target, void* detour, void** orig) {
    return MH_CreateHook(target, detour, orig) == MH_OK && MH_EnableHook(target) == MH_OK;
}

}  // namespace

void InitD3D12ResourceHooks(ID3D12Device* device) {
    if (!device) {
        return;
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 64;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> dummy_buf;
    if (FAILED(device->CreateCommittedResource(&hp,
                                               D3D12_HEAP_FLAG_NONE,
                                               &rd,
                                               D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr,
                                               IID_PPV_ARGS(&dummy_buf)))) {
        Log("D3D12Probe: failed to create dummy upload buffer for resource hooks.");
        return;
    }
    void** res_vt = *reinterpret_cast<void***>(dummy_buf.Get());
    static std::mutex s_vt_mtx;
    static void* s_hooked_map = nullptr;
    std::lock_guard<std::mutex> lock(s_vt_mtx);
    if (res_vt[8] == s_hooked_map) {
        return;
    }
    s_hooked_map = res_vt[8];
    const bool ok =
        HookAndEnable(res_vt[2],
                      reinterpret_cast<void*>(&HookResourceRelease),
                      reinterpret_cast<void**>(&g_orig_res_release)) &&
        HookAndEnable(res_vt[8],
                      reinterpret_cast<void*>(&HookResourceMap),
                      reinterpret_cast<void**>(&g_orig_res_map)) &&
        HookAndEnable(res_vt[9],
                      reinterpret_cast<void*>(&HookResourceUnmap),
                      reinterpret_cast<void**>(&g_orig_res_unmap));
    Log("D3D12 resource Map/Unmap/Release hooks %s (vt_map=%p).",
        ok ? "ready" : "failed",
        res_vt[8]);

    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = 65536;
    hd.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> dummy_heap;
    ComPtr<ID3D12Resource> placed_buf;
    if (SUCCEEDED(device->CreateHeap(&hd, IID_PPV_ARGS(&dummy_heap))) &&
        SUCCEEDED(device->CreatePlacedResource(dummy_heap.Get(),
                                               0,
                                               &rd,
                                               D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr,
                                               IID_PPV_ARGS(&placed_buf)))) {
        void** pvt = *reinterpret_cast<void***>(placed_buf.Get());
        if (pvt[8] != res_vt[8]) {
            const bool pok =
                HookAndEnable(pvt[2],
                              reinterpret_cast<void*>(&HookPlacedResourceRelease),
                              reinterpret_cast<void**>(&g_orig_placed_release)) &&
                HookAndEnable(pvt[8],
                              reinterpret_cast<void*>(&HookPlacedResourceMap),
                              reinterpret_cast<void**>(&g_orig_placed_map)) &&
                HookAndEnable(pvt[9],
                              reinterpret_cast<void*>(&HookPlacedResourceUnmap),
                              reinterpret_cast<void**>(&g_orig_placed_unmap));
            Log("D3D12 placed resource hooks %s (placed_vt_map=%p).",
                pok ? "ready" : "failed",
                pvt[8]);
        }
    }
}

void InitD3D12Probe(ID3D12CommandQueue* game_queue) {
    if (!game_queue) {
        return;
    }
    g_target_queue.store(game_queue);

    std::call_once(g_probe_init_once, [game_queue]() {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> cmd_list;
        if (FAILED(game_queue->GetDevice(IID_PPV_ARGS(&device))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&alloc))) ||
            FAILED(device->CreateCommandList(0,
                                             D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             alloc.Get(),
                                             nullptr,
                                             IID_PPV_ARGS(&cmd_list)))) {
            Log("D3D12Probe: failed to create probe command list from game device.");
            return;
        }
        InitD3D12ResourceHooks(device.Get());

        void** queue_vt = *reinterpret_cast<void***>(game_queue);
        void** cl_vt = *reinterpret_cast<void***>(cmd_list.Get());

        const bool ok =
            HookAndEnable(queue_vt[10],
                          reinterpret_cast<void*>(&HookExecuteCommandLists),
                          reinterpret_cast<void**>(&g_orig_exec_lists)) &&
            HookAndEnable(cl_vt[9],
                          reinterpret_cast<void*>(&HookCmdClose),
                          reinterpret_cast<void**>(&g_orig_cmd_close)) &&
            HookAndEnable(cl_vt[10],
                          reinterpret_cast<void*>(&HookCmdReset),
                          reinterpret_cast<void**>(&g_orig_cmd_reset)) &&
            HookAndEnable(cl_vt[12],
                          reinterpret_cast<void*>(&HookDrawInstanced),
                          reinterpret_cast<void**>(&g_orig_draw)) &&
            HookAndEnable(cl_vt[13],
                          reinterpret_cast<void*>(&HookDrawIndexedInstanced),
                          reinterpret_cast<void**>(&g_orig_draw_indexed)) &&
            HookAndEnable(cl_vt[15],
                          reinterpret_cast<void*>(&HookCopyBufferRegion),
                          reinterpret_cast<void**>(&g_orig_copy_buf)) &&
            HookAndEnable(cl_vt[17],
                          reinterpret_cast<void*>(&HookCopyResource),
                          reinterpret_cast<void**>(&g_orig_copy_res)) &&
            HookAndEnable(cl_vt[21],
                          reinterpret_cast<void*>(&HookRSSetViewports),
                          reinterpret_cast<void**>(&g_orig_rs_viewports)) &&
            HookAndEnable(cl_vt[22],
                          reinterpret_cast<void*>(&HookRSSetScissorRects),
                          reinterpret_cast<void**>(&g_orig_rs_scissors)) &&
            HookAndEnable(cl_vt[25],
                          reinterpret_cast<void*>(&HookSetPipelineState),
                          reinterpret_cast<void**>(&g_orig_set_pso)) &&
            HookAndEnable(cl_vt[43],
                          reinterpret_cast<void*>(&HookIASetIndexBuffer),
                          reinterpret_cast<void**>(&g_orig_ia_ib)) &&
            HookAndEnable(cl_vt[44],
                          reinterpret_cast<void*>(&HookIASetVertexBuffers),
                          reinterpret_cast<void**>(&g_orig_ia_vb)) &&
            HookAndEnable(cl_vt[46],
                          reinterpret_cast<void*>(&HookOMSetRenderTargets),
                          reinterpret_cast<void**>(&g_orig_om_set_rt)) &&
            HookAndEnable(cl_vt[59],
                          reinterpret_cast<void*>(&HookExecuteIndirect),
                          reinterpret_cast<void**>(&g_orig_exec_indirect));

        ComPtr<ID3D12CommandAllocator> copy_alloc;
        ComPtr<ID3D12GraphicsCommandList> copy_cl;
        if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                     IID_PPV_ARGS(&copy_alloc))) &&
            SUCCEEDED(device->CreateCommandList(0,
                                                D3D12_COMMAND_LIST_TYPE_COPY,
                                                copy_alloc.Get(),
                                                nullptr,
                                                IID_PPV_ARGS(&copy_cl)))) {
            void** copy_vt = *reinterpret_cast<void***>(copy_cl.Get());
            if (copy_vt[15] != cl_vt[15]) {
                HookAndEnable(copy_vt[15],
                              reinterpret_cast<void*>(&HookCopyBufferRegionCopyQ),
                              reinterpret_cast<void**>(&g_orig_copy_buf_copyq));
                Log("D3D12Probe: hooked separate COPY CopyBufferRegion (%p != %p).",
                    copy_vt[15],
                    cl_vt[15]);
            }
            if (copy_vt[17] != cl_vt[17]) {
                HookAndEnable(copy_vt[17],
                              reinterpret_cast<void*>(&HookCopyResourceCopyQ),
                              reinterpret_cast<void**>(&g_orig_copy_res_copyq));
            }
        }

        g_probe_installed.store(ok);
        Log("D3D12Probe hooks %s (F2 to dump 2 frames).", ok ? "ready" : "failed");
    });
}

void SetD3D12InternalCall(bool internal_call) {
    tl_internal_call = internal_call;
}

void SetSubtitleSplitActive(bool active, uint32_t frame_w, uint32_t frame_h) {
    g_sub_frame_w.store(frame_w, std::memory_order_relaxed);
    g_sub_frame_h.store(frame_h, std::memory_order_relaxed);
    g_sub_split_active.store(active, std::memory_order_release);
}

FrameRenderStats ConsumeFrameRenderStats(ID3D12CommandQueue* game_queue,
                                         uint32_t frame_w,
                                         uint32_t frame_h) {
    FrameRenderStats stats{};
    stats.frame_width = frame_w;
    stats.frame_height = frame_h;
    if (!g_probe_installed.load()) {
        return stats;
    }
    g_target_queue.store(game_queue);

    std::vector<ClSummary> cls;
    {
        std::lock_guard<std::mutex> lock(g_cl_mtx);
        cls.swap(g_frame_cls);
    }

    const int full_w = static_cast<int>(frame_w);
    const int full_h = static_cast<int>(frame_h);
    const int half_w = full_w / 2;
    const int half_h = full_h / 2;
    const int quarter_w = full_w / 4;
    const int quarter_h = full_h / 4;
    auto near_i = [](int a, int b) { return std::abs(a - b) <= 2; };

    for (const auto& cl : cls) {
        bool cl_has_full = false;
        bool cl_has_half_blur = false;
        for (const auto& vp : cl.viewports) {
            if (!vp.has_dsv && vp.x == 0 && vp.y == 0 && near_i(vp.w, full_w) &&
                near_i(vp.h, full_h)) {
                cl_has_full = true;
            }
            if (!vp.has_dsv && vp.x == 0 && vp.y == 0 && vp.draws >= 1 && vp.draws <= 64 &&
                ((near_i(vp.w, half_w) && near_i(vp.h, half_h)) ||
                 (near_i(vp.w, quarter_w) && near_i(vp.h, quarter_h)))) {
                cl_has_half_blur = true;
            }
            bool merged = false;
            for (auto& dst : stats.viewports) {
                if (dst.x == vp.x && dst.y == vp.y && dst.w == vp.w && dst.h == vp.h &&
                    dst.has_dsv == vp.has_dsv) {
                    dst.draws += vp.draws;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                stats.viewports.push_back(vp);
            }
        }
        if (cl_has_full && cl_has_half_blur) {
            stats.has_menu_blur_pass = true;
        }
    }

    return stats;
}

void TickD3D12ProbeOnPresent(ID3D12CommandQueue* game_queue, bool internal_call) {
    if (internal_call || !g_probe_installed.load()) {
        return;
    }
    g_target_queue.store(game_queue);

    const bool f2_down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    const bool f2_rising = f2_down && !g_prev_f2_down;
    g_prev_f2_down = f2_down;

    std::lock_guard<std::mutex> lock(g_cl_mtx);
    if (g_probe_active.load()) {
        Log("=== D3D12 PROBE #%u FRAME END (remaining=%d) ===",
            g_probe_seq,
            g_probe_frames_left - 1);
        --g_probe_frames_left;
        if (g_probe_frames_left <= 0) {
            g_probe_active.store(false, std::memory_order_release);
            g_closed_probe_dumps.clear();
            Log("=== D3D12 PROBE #%u COMPLETE ===", g_probe_seq);
        }
        return;
    }

    if (f2_rising) {
        ++g_probe_seq;
        g_exec_seq = 0;
        g_closed_probe_dumps.clear();
        g_probe_frames_left = 2;
        g_probe_active.store(true, std::memory_order_release);
        Log("=== D3D12 PROBE #%u START (%d frames) ===", g_probe_seq, g_probe_frames_left);
    }
}

}  // namespace gotham
