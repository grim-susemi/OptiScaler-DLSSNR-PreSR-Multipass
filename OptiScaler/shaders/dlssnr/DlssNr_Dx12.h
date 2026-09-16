#pragma once

// DX12 NR owner: model evaluation, colour conversion and presentation composition.

#include "DlssNr_Common.h"
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <memory>

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

// Twelve-frame descriptor budget. Each multipass chain reuses two immutable clamp slots.
#define DLSSNR_NUM_OF_HEAPS 96

class DlssNr_Dx12 : public Shader_Dx12, public DlssNr_Common
{
  private:
    struct State;
    std::unique_ptr<State> _state;
    FrameDescriptorHeap _frameHeaps[DLSSNR_NUM_OF_HEAPS];

    // Each recorded dispatch needs immutable constants until the GPU consumes them.
    ID3D12Resource* _constantBuffers[DLSSNR_NUM_OF_HEAPS] = {};

    uint32_t _heapIndex = 0;

    static constexpr uint32_t kSrvCount = 5;
    static constexpr uint32_t kUavCount = 2;

    ID3D12PipelineState* _residualPipelineState = nullptr;
    ID3D12PipelineState* _finishedColorPipelineState = nullptr;

    // Caller holds both locks and supplies valid commands/source/target; NR shaders share the descriptor layout.
    bool DispatchCompute(ID3D12GraphicsCommandList* cmd, const DlssNrConstants& constants,
                         ID3D12PipelineState* pipeline, ID3D12Resource* source, ID3D12Resource* model,
                         ID3D12Resource* original, ID3D12Resource* motion,
                         ID3D12Resource* target, ID3D12Resource* keep, uint32_t* immutableSlot);

  public:
    DlssNr_Dx12(std::string InName, ID3D12Device* InDevice);
    ~DlssNr_Dx12();
    static void Retire(std::unique_ptr<DlssNr_Dx12> owner);
    bool ReadyToDestroy();

    // Colour/output may alias. The adapter supplies metadata and preserves the game's resource states.
    bool Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour, ID3D12Resource* depth,
                  ID3D12Resource* motion, ID3D12Resource* output, const DlssNrFrameInfo& frame,
                  ID3D12CommandQueue* timingQueue = nullptr);

    bool CreateBufferResource(ID3D12Device* device, ID3D12Resource* source, D3D12_RESOURCE_STATES state);
    void SetBufferState(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES state);
    ID3D12Resource* Buffer();
    void DiagnosePipeline(unsigned stage, ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params,
                          ID3D12Resource* color, uint32_t flags, bool rr, bool success = true);
    void BeginInputHold(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                        const D3D12_RESOURCE_STATES* inputStates);
    void EndInputHold(NVSDK_NGX_Parameter* params);
    bool ProcessSeam(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, bool beforeUpscale,
                     ID3D12CommandQueue* queue, bool rayReconstruction, unsigned long long submissionEpoch,
                     bool interop = false, uint32_t featureFlags = 0);
    void ResetFinishedCommands(ID3D12CommandList* cmd);
    void SubmitFinishedCommands(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    bool WaitFinished();
    void ApplyFinished(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue);
    void ApplyStreamlineFinished(IDXGISwapChain* swapchain, ID3D12Resource* picture, ID3D12CommandQueue* queue);
    void ApplyFinishedDx11(IDXGISwapChain* swapchain);
    std::string FinishedStatus();
    std::string DeferredStatus();

    // Unused inputs receive stand-in descriptors. Reuse immutableSlot only for identical bindings/constants;
    // initialize it to UINT32_MAX before the chain.
    bool DispatchPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                      ID3D12Resource* InSource, ID3D12Resource* InModel, ID3D12Resource* InOriginal,
                      ID3D12Resource* InMotion,
                      ID3D12Resource* OutTarget, ID3D12Resource* OutKeep,
                      uint32_t* immutableSlot = nullptr);

    // Uses the residual or finished-colour shader with the same descriptor layout.
    bool DispatchResidualPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                              ID3D12Resource* InSource, ID3D12Resource* InModel, ID3D12Resource* InOriginal,
                              ID3D12Resource* InMotion, ID3D12Resource* OutTarget, bool finishedColor = false);
};
