#include "pch.h"
#include "DlssNr_Dx12_State.h"

bool DlssNr_Dx12::State::PrepareRunModels(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device,
                                        const DlssNrFrameInfo& frame, const D3D12_RESOURCE_DESC& desc,
                                        DlssNr::ColorExtent native, DlssNr::ColorExtent work,
                                        float workScale, unsigned int requestedPasses)
{
    const auto& cfg = *Config::Instance();
    const auto width = native.width, height = native.height;
    const auto workWidth = work.width, workHeight = work.height;
    const bool cropColor = frame.BeforeUpscale && (width != desc.Width || height != desc.Height);
    const bool reduced = workWidth != width || workHeight != height;
    const bool formatChanged = nr.output && nr.output->GetDesc().Format != desc.Format;
    if (formatChanged)
        LOG_INFO("DLSS-NR rebuilding surfaces: format {} -> {} (inject point changed)",
                 (int) nr.output->GetDesc().Format, (int) desc.Format);

    const bool resolutionChanged =
        nr.width != width || nr.height != height || nr.workWidth != workWidth || nr.workHeight != workHeight;
    const bool placementChanged = nr.width != 0 && (nr.beforeUpscale != frame.BeforeUpscale ||
                                                    nr.rayReconstruction != frame.RayReconstruction);

    if (formatChanged || resolutionChanged || placementChanged)
    {
        // Parked rather than released: with frame generation the GPU can still be several frames
        // deep in work that references all of it.
        for (auto& model : nr.models)
            model.Release();
        nr.reset = true;
        modelRunning = false;

        for (auto** resource : { &nr.output, &nr.passScratch, &nr.passClamp, &nr.colorCopy, &nr.hdrCopy,
                                 &nr.colorSmall, &nr.outputNative, &nr.activeColor })
            ParkNrResource(*resource);
    }

    nr.workWidth = workWidth;
    nr.workHeight = workHeight;
    nr.width = width;
    nr.height = height;
    nr.beforeUpscale = frame.BeforeUpscale;
    nr.rayReconstruction = frame.RayReconstruction;

    // Prepare at most one missing layer per submission. Repeated CPU calls in the same epoch
    // cannot evaluate creation work or create another layer before the first one is submitted.
    for (unsigned int pass = requestedPasses; pass < DlssNr::MaxPassCount; ++pass)
        nr.models[pass].Release();
    for (unsigned int pass = 0; pass < requestedPasses; ++pass)
    {
        bool ready = false;
        const auto prepared = nr.models[pass].Prepare(cmdList, device, workWidth, workHeight, PassSettings(cfg, pass),
                                                      frame.SubmissionEpoch, &ready);
        if (prepared != NVSDK_NGX_Result_Success)
        {
            nr.failed = true;
            nr.reason = "the NVIDIA NGX driver could not create the requested Neural Rendering passes";
            LOG_ERROR("DLSS-NR driver creation for pass {} failed: 0x{:X}", pass + 1, prepared);
            return false;
        }
        if (!ready)
        {
            // Context::Prepare rebuilds only the changed pass. Reset the whole chain's history
            // when its replacement becomes ready, without recreating unchanged model weights.
            nr.reset = true;
            modelRunning = false;
            return false;
        }
    }

    const auto ensure = [&](ID3D12Resource*& resource, unsigned w, unsigned h)
    {
        if (!resource)
            resource = CreateScratch(device, desc.Format, w, h);
        return resource != nullptr;
    };
    if (requestedPasses == 1)
    {
        ParkNrResource(nr.passScratch);
        ParkNrResource(nr.passClamp);
    }
    if (!ensure(nr.output, workWidth, workHeight) || !ensure(nr.colorCopy, width, height) ||
        !ensure(nr.hdrCopy, width, height) || (cropColor && !ensure(nr.activeColor, width, height)) ||
        (requestedPasses > 1 && (!ensure(nr.passScratch, workWidth, workHeight) ||
                                 !ensure(nr.passClamp, workWidth, workHeight))) ||
        (reduced && !ensure(nr.colorSmall, workWidth, workHeight)) ||
        (workScale > 1.0f && !ensure(nr.outputNative, width, height)))
    {
        nr.failed = true;
        nr.reason = "the Neural Rendering staging textures could not be allocated";
        return false;
    }

    return true;
}
