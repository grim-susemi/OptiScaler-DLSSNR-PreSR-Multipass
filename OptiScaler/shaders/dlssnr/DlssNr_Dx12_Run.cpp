#include "pch.h"
#include "DlssNr_Dx12_State.h"

auto DlssNr_Dx12::State::Run(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* target, ID3D12Resource* depth, ID3D12Resource* motion,
              const DlssNrFrameInfo& frame, ID3D12CommandQueue* timingQueue) -> bool
{
    const Config& cfg = *Config::Instance();

    // Entry points hold the owner lock and validate command/resource pointers.
    if (nr.failed)
        return false;
    if (!shader.IsInit())
    {
        nr.failed = true;
        nr.reason = "the colour codec would not compile";
        return false;
    }

    // Guard creation and dispatch together: either can record GPU work and alter compute bindings.
    const bool restoreRequired =
        cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default();
    if (restoreRequired && !frame.IndependentCommands && !D3D12Hooks::CanRestoreRootSignature(cmdList))
    {
        ReportSkipOnce("the upscaler could not restore state this frame");
        return false;
    }
    lifetime.Record(cmdList);
    ScopedNrStateEnvelope stateEnvelope(cmdList);

    // A completed upscaler output normally arrives as a UAV. The pre-SR colour input instead arrives
    // readable. Track every transition so both paths return the resource exactly as their caller gave
    // it to us; a pre-SR resource without UAV support is written through a scratch-and-copy fallback.
    const auto outputArrival = static_cast<D3D12_RESOURCE_STATES>(frame.OutputArrivalState);
    D3D12_RESOURCE_STATES targetState = outputArrival;
    const auto TransitionTarget = [&](D3D12_RESOURCE_STATES to)
    {
        Barrier(cmdList, target, targetState, to);
        targetState = to;
    };

    auto* device = shader._device;
    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    const auto active =
        frame.BeforeUpscale
            ? DlssNr::PreSrColorExtent(desc, frame.RenderSubrectWidth, frame.RenderSubrectHeight)
            : std::optional<DlssNr::ColorExtent> { DlssNr::ColorExtent { (unsigned int) desc.Width, desc.Height } };
    if (!active)
    {
        ReportSkipOnce("the pre-SR active colour size is invalid");
        return false;
    }
    const auto width = active->width;
    const auto height = active->height;
    const bool cropColor = frame.BeforeUpscale && (width != desc.Width || height != desc.Height);
    const bool targetSupportsUav = cropColor || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;

    const auto guideDesc = depth->GetDesc();
    const auto motionDesc = motion->GetDesc();
    const auto guides = DlssNr::ResolveGuideRegions(
        { (unsigned int) guideDesc.Width, guideDesc.Height },
        { (unsigned int) motionDesc.Width, motionDesc.Height },
        { frame.RenderSubrectWidth, frame.RenderSubrectHeight }, { frame.OutputWidth, frame.OutputHeight },
        frame.MotionVectorsLowResolution, frame.DepthSubrectBaseX, frame.DepthSubrectBaseY,
        frame.MotionSubrectBaseX, frame.MotionSubrectBaseY);
    if (!guides.depth.valid() || !guides.motion.valid())
    {
        ReportSkipOnce("depth or motion-vector subrect is empty");
        return false;
    }
    nr.reset |= frame.Reset;

    const unsigned int requestedPasses =
        std::clamp(cfg.DlssNrPasses.value_or_default(), 1u,
                   cfg.DlssNrUnlockPasses.value_or_default() ? DlssNr::MaxPassCount : DlssNr::DefaultMaxPassCount);
    // Only the model runs at working resolution; source and composition remain at native size.
    float workScale = cfg.DlssNrWorkingScale.value_or_default();
    if (!std::isfinite(workScale))
        workScale = 1.0f;
    workScale = std::clamp(workScale, 0.25f, 2.0f);
    const auto workWidth = (unsigned int) (width * workScale + 0.5f);
    const auto workHeight = (unsigned int) (height * workScale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;
    if (!PrepareRunModels(cmdList, device, frame, desc, { width, height },
                           { workWidth, workHeight }, workScale, requestedPasses))
        return false;
    // The parameter adapter already combined the HDR flag with the active color format.
    const bool isHdrBuffer = frame.ColourIsLinearHdr;

    // Advance capture scheduling only once the codec and models are ready.
    ++frames;
    lifetime.Collect();
    CheckCaptureTrigger();

    if (captureFrames.isActive())
    {
        const auto captureDir = Util::DllPath().remove_filename() / "dlssnr-capture";
        const auto written = captureFrames.write(captureDir);

        if (!written.empty())
            LOG_INFO("DLSS-NR wrote matched before/after frames to {}", written);
    }

    ResTrack_Dx12::HookLateNrQueue(device);
    if (gpuTime == nullptr)
        gpuTime = std::make_unique<DlssNrGpuTime>(device);

    gpuTime->Start(cmdList);

    // Copy just the live image, not the stale right/bottom margins. Do this only after model
    // creation/pending-submission early returns, and inside the measured GPU interval. The compact
    // texture lets every existing codec/compare/hold/capture path use unmodified pixel coordinates.
    ID3D12Resource* const gameColor = target;
    if (cropColor)
    {
        TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmdList, nr.activeColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        DlssNr::CopyActiveColor(cmdList, nr.activeColor, gameColor, *active);
        TransitionTarget(outputArrival);
        Barrier(cmdList, nr.activeColor, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        target = nr.activeColor;
        targetState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    const auto FinishColor = [&](bool copyBack)
    {
        if (cropColor)
        {
            if (copyBack)
            {
                TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cmdList, gameColor, outputArrival, D3D12_RESOURCE_STATE_COPY_DEST);
                DlssNr::CopyActiveColor(cmdList, gameColor, target, *active);
                Barrier(cmdList, gameColor, D3D12_RESOURCE_STATE_COPY_DEST, outputArrival);
            }
            TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            TransitionTarget(outputArrival);
        }
    };

    const float whitePoint = HoldColor(cmdList, device, target, targetState,
        frame.WhitePointOverride > 0.0f ? frame.WhitePointOverride : cfg.DlssNrWhitePointScale.value_or_default());
    DlssNrConstants encodeParams {};
    encodeParams.Mode = DlssNrMode_Encode;
    // A frame that is already display-referred is handed over untouched: the encode becomes a copy and
    // the resolve adds the model's edit back at full scale.
    encodeParams.Passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.WhitePoint = whitePoint;
    encodeParams.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
    encodeParams.Width = width;
    encodeParams.Height = height;

    TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    shader.DispatchPass(cmdList, encodeParams, target, nullptr, nullptr, nullptr, nr.colorCopy,
                        nr.hdrCopy);

    if (targetSupportsUav)
        TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The transitions double as the wait for the encode's writes.
    Barrier(cmdList, nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Below full resolution the model is shown a filtered shrink of the proxy; the edit it returns is
    // enlarged during the resolve while the frame underneath stays full size and untouched.
    auto* modelInput = nr.colorCopy;

    if (reduced && nr.colorSmall != nullptr)
    {
        bool built = false;

        if (workScale > 1.0f)
        {
            // Both filters bake the NR downscaler selection. Retire them together when it changes.
            const Scaler nrScaler = cfg.DlssNrScalingDownscaler.value_or_default();
            if (nr.nrScaler != nrScaler)
            {
                ReleaseSupersamplers();
                nr.nrScaler = nrScaler;
            }
            if (nr.superUp == nullptr)
                nr.superUp = new OS_Dx12("DLSS-NR supersample up", device, true, nrScaler);
            if (nr.superDown == nullptr)
                nr.superDown = new OS_Dx12("DLSS-NR supersample down", device, false, nrScaler);

            if (nr.superUp != nullptr && nr.superUp->DispatchResources(cmdList, nr.colorCopy, nr.colorSmall))
            {
                Barrier(cmdList, nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                built = true;
            }
        }

        if (!built)
        {
            if (workScale > 1.0f && !warnedSuper)
            {
                warnedSuper = true;
                LOG_WARN("DLSS-NR supersample: upscaler unavailable, falling back to a blocky enlarge.");
            }

            // Sub-native (or the upsampler could not be built): box-resample the proxy to the work size.
            DlssNrConstants down {};
            down.Mode = DlssNrMode_Downsample;
            down.Width = workWidth;
            down.Height = workHeight;
            shader.DispatchPass(cmdList, down, modelInput, nullptr, nullptr, nullptr, nr.colorSmall,
                                nullptr);
            Barrier(cmdList, nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        modelInput = nr.colorSmall;
    }

    ID3D12Resource* depthIn = ReadableGuide(device, cmdList, depth, &nr.depthClone);
    ID3D12Resource* motionIn = ReadableGuide(device, cmdList, motion, &nr.motionClone);

    if (depthIn == nullptr || motionIn == nullptr)
    {
        nr.reset = true;
        ReportSkipOnce("the game's depth or motion vectors could not be made readable this frame");
        FinishColor(false);
        return false;
    }

    // The vectors were scaled to full-frame pixels; the image the model reprojects is the working size.
    const float mvToWorkX = (float) workWidth / width;
    const float mvToWorkY = (float) workHeight / height;

    if (loggedConfigured != requestedPasses)
    {
        loggedConfigured = requestedPasses;
        LOG_INFO("DLSS-NR model passes: {}", requestedPasses);
    }

    // Keep the encoded base immutable; ping-pong model outputs and compose the final delta once.
    ID3D12Resource* passInput = modelInput;
    ID3D12Resource* passOutput = nr.output;
    ID3D12Resource* finalAnswer = nullptr;
    bool outputReadable = false;
    bool scratchReadable = false;
    bool clampReadable = false;
    uint32_t clampSlots[2] = { UINT32_MAX, UINT32_MAX };

    const auto SetModelReadable = [&](ID3D12Resource* resource, bool next)
    {
        bool& readable = resource == nr.output      ? outputReadable
                         : resource == nr.passClamp ? clampReadable
                                                    : scratchReadable;
        if (readable == next)
            return;
        const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Barrier(cmdList, resource, readable ? read : write, next ? read : write);
        readable = next;
    };

    int result = NVSDK_NGX_Result_Success;
    const bool enlargementReset = nr.reset;
    bool compositionSucceeded = false;

    DlssNr::Proxy::Frame modelFrame {};
    modelFrame.depth = depthIn;
    modelFrame.motion = motionIn;
    modelFrame.size = { workWidth, workHeight };
    modelFrame.guides = guides;
    modelFrame.depthInverted = frame.DepthInverted;
    modelFrame.reset = nr.reset;
    modelFrame.mvScaleX = frame.MvScaleX * mvToWorkX;
    modelFrame.mvScaleY = frame.MvScaleY * mvToWorkY;

    for (unsigned int pass = 0; pass < requestedPasses; ++pass)
    {
        SetModelReadable(passOutput, false);
        modelFrame.color = passInput;
        modelFrame.output = passOutput;
        result = nr.models[pass].Evaluate(cmdList, modelFrame);
        modelRunning = result == NVSDK_NGX_Result_Success;
        if (!modelRunning)
            break;

        finalAnswer = passOutput;
        SetModelReadable(finalAnswer, true);

        if (pass + 1 < requestedPasses)
        {
            SetModelReadable(nr.passClamp, false);
            DlssNrConstants clamp {};
            clamp.Mode = DlssNrMode_ClampProxy;
            clamp.Width = workWidth;
            clamp.Height = workHeight;
            if (!shader.DispatchPass(cmdList, clamp, finalAnswer, nullptr, nullptr, nullptr, nr.passClamp,
                                     nullptr, &clampSlots[pass % 2]))
            {
                result = NVSDK_NGX_Result_Fail;
                break;
            }
            SetModelReadable(nr.passClamp, true);
            passInput = nr.passClamp;
            passOutput = passOutput == nr.output ? nr.passScratch : nr.output;
        }
    }

    nr.reset = result != NVSDK_NGX_Result_Success;

    if (result == NVSDK_NGX_Result_Success && finalAnswer != nullptr)
    {
        // Resolve takes the difference between what the model returned and what it was shown, and adds
        // that back to the frame. At strength zero the result is what the upscaler produced, exactly, and
        // anything the model left alone is untouched rather than round-tripped through the curve.
        auto resolveParams = DlssNr_Common::MakeConstants(DlssNrMode_Resolve, width, height, whitePoint, isHdrBuffer, cfg);
        const auto strength = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f; };
        resolveParams.SkinDetail = strength(resolveParams.SkinDetail);
        resolveParams.SkinColour = strength(resolveParams.SkinColour);
        resolveParams.EnvironmentDetail = strength(resolveParams.EnvironmentDetail);
        resolveParams.EnvironmentColour = strength(resolveParams.EnvironmentColour);


        // Downsample the model answer to native for composition; fall back to the working-size pair.
        // The final answer is NPSR and the native output rests in UAV.
        bool superDownOk = false;
        if (workScale > 1.0f && nr.superDown != nullptr && nr.outputNative != nullptr &&
            nr.superDown->DispatchResources(cmdList, finalAnswer, nr.outputNative))
        {
            Barrier(cmdList, nr.outputNative, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            superDownOk = true;
        }

        ID3D12Resource* resolveProxy = superDownOk ? nr.colorCopy : modelInput;
        ID3D12Resource* resolveAnswer = superDownOk ? nr.outputNative : finalAnswer;
        bool enlargementReady = true;
        if (cfg.DlssNrTransfer.value_or_default() == 2 && reduced)
        {
            auto* enlarged = EnlargeMatchedResidual(cmdList, device, modelInput, finalAnswer, depthIn, motionIn,
                                                    frame, resolveParams, enlargementReset, timingQueue);
            enlargementReady = enlarged != nullptr;
            if (enlarged) { resolveAnswer = enlarged; resolveParams.Transfer = 2; }
            if (enlarged && resolveParams.DebugView == 2)
            { resolveAnswer = finalAnswer; resolveParams.Transfer = 1; } // Inspect the actual model answer.
        }
        else
        {
            ReleaseEnlarger();
            enlargementStatus.clear();
        }

        // Resolve pre-SR inputs without UAV support through an owned scratch and copy-back.
        ID3D12Resource* resolveOriginal = targetSupportsUav ? nr.hdrCopy : target;
        ID3D12Resource* resolveTarget =
            targetSupportsUav ? target : nr.hdrCopy;

        if (targetSupportsUav)
        {
            TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        const bool resolved = enlargementReady && shader.DispatchPass(cmdList, resolveParams, resolveProxy, resolveAnswer,
                                                  resolveOriginal, motionIn, resolveTarget, nullptr);
        compositionSucceeded = resolved;

        if (resolved && !targetSupportsUav)
        {
            Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            const D3D12_RESOURCE_STATES priorTargetState = targetState;
            TransitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyResource(target, nr.hdrCopy);
            TransitionTarget(priorTargetState);
            Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        else if (!targetSupportsUav)
        {
            // The resolve target was made writable even while private DLSS was
            // warming up. Restore it before the common end-of-frame transition.
            Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        if (superDownOk)
            Barrier(cmdList, nr.outputNative, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // Schedule matched proxy/output capture for delayed readback.
        if (captureFrames.isActive())
        {
            captureFrames.record(cmdList, device, nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 target, targetState);

        }
    }
    else if (result != NVSDK_NGX_Result_Success)
    {
        nr.failed = true;
        nr.reason = "the Neural Rendering pass failed";

        LOG_ERROR("DLSS-NR evaluate returned 0x{:X}; use Retry to recreate the model", (uint32_t) result);
    }

    // Restore all intermediate surfaces to the UAV state expected by the next frame.
    SetModelReadable(nr.output, false);
    if (nr.passScratch != nullptr)
        SetModelReadable(nr.passScratch, false);

    Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (nr.passClamp != nullptr)
        SetModelReadable(nr.passClamp, false);

    // Failed evaluations leave the game's original image intact. A successful copy-back writes
    // only the active rectangle and restores both resources before DLSS consumes the image.
    FinishColor(compositionSucceeded);

    EndGpuTiming(cmdList);

    // Restore guide clones to COPY_DEST for the next frame's refresh.
    if (depthIn == nr.depthClone)
        Barrier(cmdList, nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (motionIn == nr.motionClone)
        Barrier(cmdList, nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (reduced && nr.colorSmall != nullptr)
        Barrier(cmdList, nr.colorSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Leave the staging copy as the next frame expects to find it.
    Barrier(cmdList, nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return compositionSucceeded;
}
