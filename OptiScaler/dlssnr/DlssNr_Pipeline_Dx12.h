#pragma once

#include <upscalers/ShaderPipeline_Dx12.h>
#include <algorithm>

class DlssNr_Dx12;

namespace DlssNr
{
inline void Barrier(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    commands->ResourceBarrier(1, &barrier);
}

// Copy matching textures and restore both arrival states.
inline void CopyTexture(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target, D3D12_RESOURCE_STATES targetState,
                        ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState)
{
    Barrier(cmd, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, target, targetState, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(target, source);
    Barrier(cmd, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    Barrier(cmd, target, D3D12_RESOURCE_STATE_COPY_DEST, targetState);
}

inline bool FormatCanHoldLinearHdr(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return true;
    default:
        return false;
    }
}

struct InputStates_Dx12
{
    D3D12_RESOURCE_STATES color;
    D3D12_RESOURCE_STATES depth;
    D3D12_RESOURCE_STATES motion;
    D3D12_RESOURCE_STATES exposure;
};

// Track transitions within one pass and restore arrival states, including aliased inputs.
struct ResourceStates_Dx12
{
    ID3D12GraphicsCommandList* commands;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;

    void Set(ID3D12Resource* resource, D3D12_RESOURCE_STATES state,
             D3D12_RESOURCE_STATES arrival = D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        if (!resource)
            return;
        auto it = std::find_if(barriers.begin(), barriers.end(),
                               [resource](const auto& b) { return b.Transition.pResource == resource; });
        if (it == barriers.end())
        {
            D3D12_RESOURCE_BARRIER barrier {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, arrival, arrival };
            it = barriers.insert(it, barrier);
        }
        Barrier(commands, resource, it->Transition.StateAfter, state);
        it->Transition.StateAfter = state;
    }
    void Read(ID3D12Resource* resource, D3D12_RESOURCE_STATES arrival = D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        Set(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
    }
    void Restore()
    {
        for (auto it = barriers.rbegin(); it != barriers.rend(); ++it)
            Barrier(commands, it->Transition.pResource, it->Transition.StateAfter, it->Transition.StateBefore);
        barriers.clear();
    }
    ~ResourceStates_Dx12() { Restore(); }
};

// Shared arrival-state policy for NR input copies and private upscaler guides.
InputStates_Dx12 ResolveInputStates_Dx12(bool interop);
bool CanRunBeforeUpscale_Dx12(NVSDK_NGX_Parameter* parameters);
}

// These adapters only translate NGX inputs and resource states. The shader owns all NR resources/history.
ShaderPass_Dx12 MakeDlssNrPass(DlssNr_Dx12& shader, ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                               NVSDK_NGX_Parameter* parameters, bool beforeUpscale, unsigned int featureFlags,
                               ID3D12CommandQueue* timingQueue = nullptr, bool interop = false,
                               bool rayReconstruction = false, uint64_t submissionEpoch = 0);
ID3D12Resource* PrepareDlssNrInput(DlssNr_Dx12& shader, ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                                   NVSDK_NGX_Parameter* parameters, unsigned int featureFlags,
                                   ID3D12CommandQueue* timingQueue = nullptr, bool interop = false,
                                   bool rayReconstruction = false, uint64_t submissionEpoch = 0);
