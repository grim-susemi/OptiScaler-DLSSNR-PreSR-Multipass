#pragma once

#include <upscalers/ShaderPipeline_Dx12.h>
#include <algorithm>

class DlssNr_Dx12;

namespace DlssNr
{
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

// Return each borrowed input to its arrival state, once even when guides alias.
struct ReadableInputs_Dx12
{
    ID3D12GraphicsCommandList* commands;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;

    void Read(ID3D12Resource* resource, D3D12_RESOURCE_STATES state)
    {
        if (!resource || std::any_of(barriers.begin(), barriers.end(),
                                    [resource](const auto& b) { return b.Transition.pResource == resource; }))
            return;
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        barriers.push_back(barrier);
        if (state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            commands->ResourceBarrier(1, &barrier);
    }
    ~ReadableInputs_Dx12()
    {
        for (auto it = barriers.rbegin(); it != barriers.rend(); ++it)
        {
            std::swap(it->Transition.StateBefore, it->Transition.StateAfter);
            if (it->Transition.StateBefore != it->Transition.StateAfter)
                commands->ResourceBarrier(1, &*it);
        }
    }
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
