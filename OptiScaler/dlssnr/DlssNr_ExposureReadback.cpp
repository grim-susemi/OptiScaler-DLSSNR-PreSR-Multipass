#include "pch.h"

#include "DlssNr_ExposureScan_Internal.h"
#include "DlssNr_Readback.h"

#include <DirectXPackedVector.h>
#include <Config.h>
#include <Util.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace DlssNr::ExposureScan
{
namespace Detail
{
ScanState g_scan;
std::mutex g_scanMutex;
std::mutex g_tickMutex;

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (res == nullptr || from == to)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = res;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    cmdList->ResourceBarrier(1, &barrier);
}

bool EnsureReadback(ID3D12Device* device)
{
    for (unsigned int i = 0; i < kSlots; ++i)
    {
        if (g_scan.readback[i] != nullptr)
            continue;

        if (!DlssNr::CreateReadbackBuffer(device, kStride * kMaxCandidates, &g_scan.readback[i]))
        {
            std::lock_guard<std::mutex> lock(g_scanMutex);
            g_scan.status = "could not allocate the readback buffers";
            return false;
        }
    }

    return true;
}

}
using namespace Detail;
void Tick(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, uint64_t submissionEpoch)
{
    if (!Wanted())
        return;

    if (device == nullptr || cmdList == nullptr)
        return;

    std::lock_guard<std::mutex> tickLock(g_tickMutex);
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        if (g_scan.device != nullptr && g_scan.device != device)
            return;
        if (submissionEpoch != UINT64_MAX && submissionEpoch == g_scan.lastEpoch)
            return;
        if (g_scan.device == nullptr)
        {
            g_scan.device = device;
            device->AddRef();
            // Discovery starts before a rendering device is known. Never record a copy of a
            // resource discovered on another device into this device's command list.
            std::erase_if(g_scan.tracked, [device](const Tracked& candidate)
            {
                if (candidate.device == device)
                    return false;
                candidate.resource->Release();
                return true;
            });
        }
    }

    // Outside g_scanMutex, deliberately. EnsureReadback creates a committed resource, and that call is
    // detoured to hkCreateCommittedResource -> NoteResource, which takes g_scanMutex. Holding it
    // here would be a self-deadlock. g_tickMutex serializes different NR owners and shutdown.
    if (!EnsureReadback(device))
        return;

    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (g_scan.tracked.empty())
    {
        g_scan.status = "no buffer in this game is shaped like an exposure";
        return;
    }
    g_scan.lastEpoch = submissionEpoch;

    // Read the slot written four frames ago before overwriting it. Retired by now, so this reads
    // mapped memory rather than waiting on the GPU.
    if (g_scan.frames >= kSlots)
    {
        ID3D12Resource* old = g_scan.readback[g_scan.frames % kSlots];
        void* mapped = nullptr;
        D3D12_RANGE range { 0, kStride * kMaxCandidates };

        if (old != nullptr && SUCCEEDED(old->Map(0, &range, &mapped)) && mapped != nullptr)
        {
            const unsigned char* base = (const unsigned char*) mapped;

            const auto readableCount = std::min(g_scan.tracked.size(), g_scan.readbackCounts[g_scan.frames % kSlots]);
            for (size_t i = 0; i < readableCount; ++i)
            {
                Tracked& t = g_scan.tracked[i];
                const unsigned char* at = base + i * kStride;

                float value = 0.0f;

                if (t.bytes == 2)
                {
                    uint16_t half = 0;
                    std::memcpy(&half, at, sizeof(half));
                    value = DirectX::PackedVector::XMConvertHalfToFloat(half);
                }
                else
                {
                    std::memcpy(&value, at, sizeof(value));
                }

                if (!std::isfinite(value))
                    continue;

                // Only plausible positive exposure values contribute to the observed range.
                if (value <= kFloor || value >= kCeiling)
                {
                    t.latest = value;
                    t.reads++;
                    continue;
                }

                if (t.inRange == 0)
                {
                    t.lowest = value;
                    t.highest = value;
                }
                else
                {
                    t.lowest = std::min(t.lowest, value);
                    t.highest = std::max(t.highest, value);
                }

                t.inRange++;

                // Require a 25% ratio change across plausible samples to reject near-zero numerical noise.
                if (t.inRange > 1 && t.highest > t.lowest * 1.25f)
                    t.moves = true;

                t.latest = value;
                t.reads++;
            }

            D3D12_RANGE nothingWritten { 0, 0 };
            old->Unmap(0, &nothingWritten);
        }
    }

    // Log moving candidates and their observed ranges every 300 scan frames.
    if (g_scan.frames > 0 && g_scan.frames % 300 == 0)
    {
        unsigned int movers = 0;

        for (size_t i = 0; i < g_scan.tracked.size(); ++i)
        {
            const Tracked& t = g_scan.tracked[i];

            if (!t.moves)
                continue;

            movers++;
            LOG_INFO("DLSS-NR scan mover: candidate {} ({}) range {:.5f}..{:.5f} (x{:.1f}), latest {:.5f}",
                     (unsigned int) (i + 1), t.shape, t.lowest, t.highest,
                     t.lowest > kFloor ? t.highest / t.lowest : 0.0f, t.latest);
        }

        if (movers == 0)
            LOG_INFO("DLSS-NR scan: {} candidates tracked, none moving yet -- go between bright and dark",
                     (unsigned int) g_scan.tracked.size());
    }

    ID3D12Resource* dst = g_scan.readback[g_scan.frames % kSlots];

    if (dst == nullptr)
        return;

    // Discovered UAVs have no exposure-state contract. Scanning assumes UNORDERED_ACCESS.
    for (size_t i = 0; i < g_scan.tracked.size(); ++i)
    {
        Tracked& t = g_scan.tracked[i];

        if (t.resource == nullptr)
            continue;

        Barrier(cmdList, t.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);

        if (t.isBuffer)
        {
            cmdList->CopyBufferRegion(dst, i * kStride, t.resource, 0, t.bytes);
        }
        else
        {
            D3D12_TEXTURE_COPY_LOCATION src {};
            src.pResource = t.resource;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION to {};
            to.pResource = dst;
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint.Offset = i * kStride;
            to.PlacedFootprint.Footprint.Format = t.texFormat;
            to.PlacedFootprint.Footprint.Width = 1;
            to.PlacedFootprint.Footprint.Height = 1;
            to.PlacedFootprint.Footprint.Depth = 1;
            to.PlacedFootprint.Footprint.RowPitch = 256;

            D3D12_BOX one { 0, 0, 0, 1, 1, 1 };
            cmdList->CopyTextureRegion(&to, 0, 0, 0, &src, &one);
        }

        Barrier(cmdList, t.resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    g_scan.readbackCounts[g_scan.frames % kSlots] = g_scan.tracked.size();
    g_scan.frames++;
    g_scan.status = "";
}

// Release foreign resource references before their placed heaps are destroyed.
// Keep our readback ring alive: recorded copies may still be in flight.
void ReleaseTrackedResources()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    for (Tracked& t : g_scan.tracked)
    {
        if (t.resource != nullptr)
            t.resource->Release();
    }

    g_scan.tracked.clear();
    g_scan.complained = false;
    // The ring may still contain copies of the old candidates. Do not interpret those values
    // as newly adopted resources that happen to occupy the same list positions.
    std::fill(std::begin(g_scan.readbackCounts), std::end(g_scan.readbackCounts), 0);
}

void Shutdown()
{
    std::lock_guard<std::mutex> tickLock(g_tickMutex);
    std::lock_guard<std::mutex> lock(g_scanMutex);

    for (Tracked& t : g_scan.tracked)
    {
        if (t.resource != nullptr)
            t.resource->Release();
    }

    g_scan.tracked.clear();

    for (unsigned int i = 0; i < kSlots; ++i)
    {
        if (g_scan.readback[i] != nullptr)
        {
            g_scan.readback[i]->Release();
            g_scan.readback[i] = nullptr;
        }
    }

    g_scan.frames = 0;
    std::fill(std::begin(g_scan.readbackCounts), std::end(g_scan.readbackCounts), 0);
    g_scan.lastEpoch = UINT64_MAX;
    if (g_scan.device != nullptr)
    {
        g_scan.device->Release();
        g_scan.device = nullptr;
    }
    g_scan.status = "not started";
}

}
