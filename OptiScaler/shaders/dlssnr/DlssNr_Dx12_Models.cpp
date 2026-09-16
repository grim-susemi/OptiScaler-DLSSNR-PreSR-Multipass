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

    // Tuning changes require rebuilding the feature, but not its scratch textures.
    bool tuningChanged = false;
    for (unsigned pass = 0; pass < requestedPasses; ++pass)
        tuningChanged |= nr.models[pass].HasFeature() && nr.builtSettings[pass] != PassSettings(cfg, pass);

    if (formatChanged || resolutionChanged || placementChanged || (nr.models[0].HasFeature() && tuningChanged))
    {
        // Parked rather than released: with frame generation the GPU can still be several frames
        // deep in work that references all of it.
        for (auto& model : nr.models)
            model.RetryAfterFailure();
        std::fill(std::begin(nr.passCreateFailed), std::end(nr.passCreateFailed), false);
        nr.reset = true;
        modelRunning = false;

        // Resolution and seam changes invalidate the scratch state. Tuning does not, and throwing
        // resources away for it would mean a reallocation every time a slider moves.
        if (formatChanged || resolutionChanged || placementChanged)
        {
            for (auto** resource : { &nr.output, &nr.passScratch, &nr.passClamp, &nr.colorCopy, &nr.hdrCopy,
                                     &nr.colorSmall, &nr.outputNative, &nr.activeColor })
                ParkNrResource(*resource);
            nr.passScratchFailed = false;
        }
    }

    if (nr.output == nullptr)
    {
        nr.output = CreateScratch(device, desc.Format, workWidth, workHeight);
        nr.colorCopy = CreateScratch(device, desc.Format, width, height);
        nr.hdrCopy = CreateScratch(device, desc.Format, width, height);
        nr.workWidth = workWidth;
        nr.workHeight = workHeight;
        nr.width = width;
        nr.height = height;
        nr.beforeUpscale = frame.BeforeUpscale;
        nr.rayReconstruction = frame.RayReconstruction;
        nr.reset = true;
    }

    if (cropColor && nr.activeColor == nullptr)
        nr.activeColor = CreateScratch(device, desc.Format, width, height);
    if (cropColor && nr.activeColor == nullptr)
    {
        nr.failed = true;
        nr.reason = "the pre-SR active colour staging texture could not be allocated";
        LOG_ERROR("DLSS-NR unavailable: {}", nr.reason);
        return false;
    }

    if (requestedPasses == 1)
    {
        // Reclaim the extra raster and clear its failure latch. Raising the count later gets one fresh
        // allocation attempt; holding a failing allocation at two must not retry it every frame.
        ParkNrResource(nr.passScratch);
        ParkNrResource(nr.passClamp);
        nr.passScratchFailed = false;
    }
    else if (nr.passScratch == nullptr && !nr.passScratchFailed)
    {
        nr.passScratch = CreateScratch(device, desc.Format, workWidth, workHeight);
        nr.passClamp = CreateScratch(device, desc.Format, workWidth, workHeight);
        nr.passScratchFailed = nr.passScratch == nullptr || nr.passClamp == nullptr;
        if (nr.passScratchFailed)
        {
            ParkNrResource(nr.passScratch);
            ParkNrResource(nr.passClamp);
            LOG_ERROR("DLSS-NR: could not allocate the multipass textures; extra passes are disabled");
        }
    }

    if (reduced && nr.colorSmall == nullptr)
        nr.colorSmall = CreateScratch(device, desc.Format, workWidth, workHeight);

    // The down-leg target is native (the answer is brought back to frame size before the resolve).
    if (workScale > 1.0f && nr.outputNative == nullptr)
        nr.outputNative = CreateScratch(device, desc.Format, width, height);

    if (!nr.output || !nr.colorCopy || !nr.hdrCopy)
    {
        nr.failed = true;
        nr.reason = "the Neural Rendering staging textures could not be allocated";
        return false;
    }

    // Prepare at most one missing layer per submission. Repeated CPU calls in the same epoch
    // cannot evaluate creation work or create another layer before the first one is submitted.
    for (unsigned int pass = requestedPasses; pass < DlssNr::MaxPassCount; ++pass)
    {
        nr.models[pass].RetryAfterFailure();
        nr.passCreateFailed[pass] = false;
    }
    const unsigned int buildPasses = nr.passScratch ? requestedPasses : 1;
    for (unsigned int pass = 0; pass < buildPasses; ++pass)
    {
        if (nr.passCreateFailed[pass])
            break;
        bool ready = false;
        const auto prepared = nr.models[pass].Prepare(cmdList, device, workWidth, workHeight, PassSettings(cfg, pass),
                                                      frame.SubmissionEpoch, &ready);
        if (prepared != NVSDK_NGX_Result_Success)
        {
            nr.passCreateFailed[pass] = true;
            if (pass == 0)
            {
                nr.failed = true;
                nr.reason = "the NVIDIA NGX driver could not create Neural Rendering";
            }
            LOG_ERROR("DLSS-NR driver creation for pass {} failed: 0x{:X} ({})", pass + 1, prepared,
                      NgxResultName(prepared));
            return false;
        }
        nr.builtSettings[pass] = PassSettings(cfg, pass);
        if (!ready)
            return false;
    }

    return true;
}

auto DlssNr_Dx12::State::NgxResultName(unsigned int r) -> const char*
{
    if (r == NVSDK_NGX_Result_Success)
        return "Success";
    static constexpr const char* failures[] = {
        "FAIL_FeatureNotSupported",
        "FAIL_PlatformError",
        "FAIL_FeatureAlreadyExists",
        "FAIL_FeatureNotFound",
        "FAIL_InvalidParameter",
        "FAIL_ScratchBufferTooSmall",
        "FAIL_NotInitialized",
        "FAIL_UnsupportedInputFormat",
        "FAIL_RWFlagMissing",
        "FAIL_MissingInput",
        "FAIL_UnableToInitializeFeature",
        "FAIL_OutOfDate",
        "FAIL_OutOfGPUMemory",
        "FAIL_UnsupportedFormat",
        "FAIL_UnableToWriteToAppDataPath",
        "FAIL_UnsupportedParameter",
        "FAIL_Denied",
        "FAIL_NotImplemented",
    };
    const unsigned index = r - 0xBAD00001u;
    return index < std::size(failures) ? failures[index] : "unknown";
}
