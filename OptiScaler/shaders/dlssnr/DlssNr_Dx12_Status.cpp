#include "pch.h"
#include "DlssNr_Dx12_State.h"

auto DlssNr_Dx12::State::ReportSkipOnce(const char* reason) -> void
{

    if (seen.insert(reason).second)
        LOG_INFO("DLSS-NR did not run: {}", reason);
}

auto DlssNr_Dx12::State::DeferredDlssStatus() -> std::string
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return deferredSr.status;
}

auto DlssNr_Dx12::State::RetryAfterFailure() -> void
{
    ReleaseEnlarger();
    enlargementStatus.clear();
    nr.failed = false;
    nr.reason = "";
    nr.reset = true;
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
            model.RetryAfterFailure();
        std::fill(std::begin(nr.passCreateFailed), std::end(nr.passCreateFailed), false);
        modelRunning = false;
        RetryAfterFailure();
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
          frames,
          { nr.exposureFrames, nr.exposureOfferedNow, nr.exposureEverOffered, nr.gameExposure, nr.gamePreExposure } });
}

void DlssNr_Dx12::State::EndGpuTiming(ID3D12GraphicsCommandList* cmdList)
{
    gpuTime->End(cmdList);
    if (auto ms = gpuTime->ReadGpuTime())
        lastGpuTime = ms;
    if (auto ngx = ngxTime->ReadGpuTime())
        lastNgxTime = ngx;

    if (lastGpuTime && lastNgxTime && frames - lastSplitLog > 600)
    {
        lastSplitLog = frames;
        const double total = *lastGpuTime, ngx = *lastNgxTime;
        LOG_INFO("DLSS-NR elapsed: {:.2f} ms total, {:.2f} ms model, {:.2f} ms surrounding work ({:.0f}%; "
                 "intervals may include other GPU work)",
                 total, ngx, total - ngx, total > 0.0 ? 100.0 * (total - ngx) / total : 0.0);
    }
}
