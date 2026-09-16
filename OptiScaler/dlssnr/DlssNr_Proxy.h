#pragma once

// Neural Rendering uses the existing NVIDIA NGX driver dispatcher.

#include <d3d12.h>
#include "DlssNr_ModelParameters.h"
#include <memory>
#include <cstdint>

namespace DlssNr
{
namespace Proxy
{
using Frame = ModelFrame<ID3D12Resource>;

class Context
{
    struct Impl;
    std::unique_ptr<Impl> _impl;

  public:
    Context();
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // Creation records GPU work. A feature becomes ready only in a later submission epoch.
    // The owning pipeline releases the chain before changing its device, size or placement.
    unsigned int Prepare(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, unsigned int width,
                         unsigned int height, const ModelSettings& settings, uint64_t submissionEpoch, bool* ready);
    void Collect();

    // Prepare must report ready first; the caller supplies validated, shader-readable inputs.
    unsigned int Evaluate(ID3D12GraphicsCommandList* cmdList, const Frame& frame);

    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    void ResetRecording(ID3D12CommandList* commands);
    bool Idle();

    // Retires ownership and clears failures; destruction waits for recordings and GPU work.
    // Unresolved ownership is abandoned if this context is destroyed.
    void Release();
};
} // namespace Proxy
} // namespace DlssNr
