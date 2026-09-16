#include "pch.h"
#include "DlssNr_Dx12_State.h"

auto DlssNr_Dx12::State::ReportSkipOnce(const char* reason) -> void
{

    if (seen.insert(reason).second)
        LOG_INFO("DLSS-NR did not run: {}", reason);
}

auto DlssNr_Dx12::State::ConsumeControls() -> void
{
    const auto& cfg = *Config::Instance();
    if (!cfg.DlssNrEnabled.value_or_default() || cfg.DlssNrTransfer.value_or_default() != 2 ||
        cfg.DlssNrWorkingScale.value_or_default() >= 1.0f)
    {
        ReleaseEnlarger();
        enlargementStatus.clear();
    }
    const auto requested = DlssNr::ReadControlRequests();
    if (requested.retryGeneration != controls.retryGeneration)
    {
        for (auto& model : nr.models)
            model.Release();
        modelRunning = false;
        ReleaseEnlarger();
        enlargementStatus.clear();
        nr.failed = false;
        nr.reason = "";
        nr.reset = true;
    }
    if (requested.captureGeneration != controls.captureGeneration)
        captureFrames.request(requested.captureFrames);
    controls = requested;
}

auto DlssNr_Dx12::State::Publish() -> void
{
    DlssNr::PublishStatus(
        &shader, DlssNr::Backend::Dx12,
        { !nr.failed && modelRunning && enlargementStatus.empty(),
          nr.failed ? nr.reason : enlargementStatus,
          lastGpuTime,
          frames });
}

void DlssNr_Dx12::State::EndGpuTiming(ID3D12GraphicsCommandList* cmdList)
{
    gpuTime->End(cmdList);
    if (auto ms = gpuTime->ReadGpuTime())
        lastGpuTime = ms;
}
