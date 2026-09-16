#include "pch.h"
#include "DlssNr_Proxy.h"
#include "DlssNr_GpuLifetime.h"
#include "DlssNr_CompatibilityRuntime.h"

#include <Logger.h>
#include <proxies/NVNGX_Proxy.h>
#include <vector>

namespace
{
struct ProxyState
{
    NVSDK_NGX_Handle* feature = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    std::shared_ptr<DlssNr::CompatibilityRuntime> compatibility;

    DlssNr::ModelSettings settings {};
    uint64_t creationEpoch = 0;
    bool failed = false;
    bool reset = true;
};

void DestroyState(ProxyState& state)
{
    if (state.feature != nullptr)
    {
        if (state.compatibility) state.compatibility->Release(state.feature);
        else if (NVNGXProxy::D3D12_ReleaseFeature() != nullptr)
            NVNGXProxy::D3D12_ReleaseFeature()(state.feature);
    }

    if (state.params != nullptr && NVNGXProxy::D3D12_DestroyParameters() != nullptr)
        NVNGXProxy::D3D12_DestroyParameters()(state.params);

    state = {};
}

// Everything the model reads when the feature is built.
//
// These have to be set before create, not at evaluate. The model reads its tuning once, while
// building the feature; values written only at evaluate are ignored, which is why several of these
// controls appeared to do nothing for a long time.
void SetCreationParameters(NVSDK_NGX_Parameter* params, const DlssNr::ModelSettings& settings, unsigned int width,
                           unsigned int height)
{
    DlssNr::SetModelCreation(params, settings, width, height);
    params->Set("DLSSNR.ControlMask", static_cast<ID3D12Resource*>(nullptr));
    params->Set("DLSSNR.UI", static_cast<ID3D12Resource*>(nullptr));
    params->Set("DLSSNR.UIAlpha", static_cast<ID3D12Resource*>(nullptr));
    params->Set("DLSSNR.Backbuffer", static_cast<ID3D12Resource*>(nullptr));
    for (const char* key :
         { "DLSSNR.UISubrectBaseX", "DLSSNR.UISubrectBaseY", "DLSSNR.UISubrectWidth", "DLSSNR.UISubrectHeight",
           "DLSSNR.UIAlphaSubrectBaseX", "DLSSNR.UIAlphaSubrectBaseY", "DLSSNR.UIAlphaSubrectWidth",
           "DLSSNR.UIAlphaSubrectHeight", "DLSSNR.BackbufferSubrectBaseX", "DLSSNR.BackbufferSubrectBaseY",
           "DLSSNR.BackbufferSubrectWidth", "DLSSNR.BackbufferSubrectHeight" })
        params->Set(key, 0u);
}
} // namespace

namespace DlssNr
{
namespace Proxy
{
struct Context::Impl
{
    ProxyState state;
    DlssNr::GpuLifetime lifetime;
    void RetireState();
};

void Context::Impl::RetireState()
{
    if (state.feature != nullptr || state.params != nullptr)
        lifetime.Retire([retired = state]() mutable { DestroyState(retired); });
    state = {};
    lifetime.BeginGeneration();
}

unsigned int Context::Prepare(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, unsigned int width,
                                    unsigned int height, const ModelSettings& settings, uint64_t submissionEpoch,
                                    bool* ready)
{
    auto& state = _impl->state;
    auto& lifetime = _impl->lifetime;
    *ready = false;
    lifetime.Collect();
    if (state.failed || !cmdList || !device || !width || !height)
        return 0;
    if (!NVNGXProxy::InitDx12(device) || !NVNGXProxy::D3D12_GetCapabilityParameters() ||
        !NVNGXProxy::D3D12_DestroyParameters() || !NVNGXProxy::D3D12_ReleaseFeature() ||
        !NVNGXProxy::D3D12_CreateFeature() || !NVNGXProxy::D3D12_EvaluateFeature())
        return 0;
    if (state.feature && state.settings != settings)
        _impl->RetireState();
    if (state.params == nullptr)
    {
        // A dedicated parameter map populated with NGX capabilities. Unlike the deprecated
        // GetParameters API, GetCapabilityParameters transfers ownership to the caller.
        const auto allocated = NVNGXProxy::D3D12_GetCapabilityParameters()(&state.params);
        if (allocated != NVSDK_NGX_Result_Success || state.params == nullptr)
        {
            DestroyState(state);
            state.failed = true;
            LOG_ERROR("DLSS-NR (driver): the NGX core refused its capability parameters");
            return (unsigned int) (allocated == NVSDK_NGX_Result_Success ? NVSDK_NGX_Result_Fail : allocated);
        }
    }

    if (state.feature == nullptr)
    {
        SetCreationParameters(state.params, settings, width, height);

        lifetime.Record(cmdList);
        auto created =
            NVNGXProxy::D3D12_CreateFeature()(cmdList, (NVSDK_NGX_Feature) 18, state.params, &state.feature);
        if (NVSDK_NGX_FAILED(created) && !state.feature)
        {
            state.compatibility = CompatibilityRuntime::TryOpen(device);
            if (state.compatibility)
            {
                SetCreationParameters(state.params, settings, width, height);
                created = state.compatibility->Create(cmdList, state.params, &state.feature);
                LOG_INFO("NR compatibility: CreateFeature(18) result=0x{:08X} handle={}",
                         (unsigned)created, (void*)state.feature);
            }
        }

        if (created != NVSDK_NGX_Result_Success || state.feature == nullptr)
        {
            _impl->RetireState();
            state.failed = true;
            LOG_ERROR("DLSS-NR: CreateFeature(18) failed 0x{:X}", (unsigned int) created);
            return (unsigned int) (created == NVSDK_NGX_Result_Success ? NVSDK_NGX_Result_Fail : created);
        }

        state.settings = settings;
        state.creationEpoch = submissionEpoch;
        LOG_INFO("DLSS-NR: feature created at {}x{} through {}", width, height,
                 state.compatibility ? "direct compatibility runtime" : "NVIDIA NGX driver");

        // Creation must reach the GPU before any evaluation is recorded.
        return (unsigned int) NVSDK_NGX_Result_Success;
    }

    *ready = submissionEpoch != state.creationEpoch;
    return (unsigned int) NVSDK_NGX_Result_Success;
}

unsigned int Context::Evaluate(ID3D12GraphicsCommandList* cmdList, const Frame& frame)
{
    auto& state = _impl->state;
    auto& lifetime = _impl->lifetime;
    NVSDK_NGX_Parameter* params = state.params;

    SetModelEvaluation(params, frame, state.settings, state.reset);

    lifetime.Record(cmdList);
    const auto result = state.compatibility ? state.compatibility->Evaluate(cmdList, state.feature, params)
                                           : NVNGXProxy::D3D12_EvaluateFeature()(cmdList, state.feature, params, nullptr);

    state.failed = result != NVSDK_NGX_Result_Success;
    state.reset = state.failed;
    return (unsigned int) result;
}
Context::Context() : _impl(std::make_unique<Impl>()) {}
Context::~Context() { Release(); }
void Context::Release()
{
    _impl->RetireState();
    _impl->lifetime.Collect();
}
void Context::Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    _impl->lifetime.Submitted(queue, count, lists);
}
void Context::ResetRecording(ID3D12CommandList* commands) { _impl->lifetime.ResetRecording(commands); }
bool Context::Idle() { return _impl->lifetime.Idle(); }

void Context::Collect() { _impl->lifetime.Collect(); }

} // namespace Proxy
} // namespace DlssNr
