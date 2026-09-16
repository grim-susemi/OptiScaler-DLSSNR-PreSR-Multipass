#include "pch.h"

#include "DlssNrFeature_Vk_Internal.h"
#include "DlssNr_Placement.h"
#include "DlssNrPipeline_Vk.h"
#include <algorithm>
#include <cmath>

namespace DlssNr
{

bool ModelVk::Impl::Evaluate(VkCommandBuffer cmdBuffer, const VkImageInfo& colourInfo, const VkImageInfo& depthInfo,
              const VkImageInfo& motionInfo, const VkImageInfo& target, const DlssNrFrameInfo_Vk& frame,
              VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, VkImageLayout inputLayout)
{
    const bool beforeSr = frame.BeforeUpscale;
    const bool rayReconstruction = frame.RayReconstruction;
    auto& cfg = *Config::Instance();

    if (ResolvePlacement(cfg.DlssNrRunBeforeSr.value_or_default(), cfg.DlssNrDeferredDlss.value_or_default(),
                         cfg.DlssNrResidualAcrossRr.value_or_default(), cfg.DlssNrFinishedPicture.value_or_default()).deferred &&
        !frame.FinishedPicture)
    {

        if (!warnedDeferred)
        {
            LOG_WARN("DLSS-NR DeferredDLSS requires the D3D12 path or a D3D12 bridge; "
                     "native Vulkan leaves the clean SR frame unchanged");
            warnedDeferred = true;
        }
        return false;
    }

    if (cfg.DlssNrFinishedPicture.value_or_default() && !frame.FinishedPicture)
        return false; // the presentation stage owns this mode

    if (!cfg.DlssNrEnabled.value_or_default())
        return false;

    if (cmdBuffer == VK_NULL_HANDLE || device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE)
        return false;

    std::lock_guard<std::mutex> lock(mutex);
    if (cfg.DlssNrTransfer.value_or_default() == 2 && cfg.DlssNrWorkingScale.value_or_default() < 1.0f)
    {
        PublishStatus(this, Backend::Vulkan,
                      { false, "Matched residual + DLSS requires the DX12 processing path." });
        return false;
    }

    struct ReportStatus
    {
        Impl* owner;
        ~ReportStatus()
        {
            const auto& state = owner->state;
            PublishStatus(owner, Backend::Vulkan,
                          { state.models[0].feature != nullptr && !state.failed, state.reason, state.lastGpuTime,
                            state.frames });
        }
    } report { this };
    const auto requestedRetry = RetryGeneration();
    if (requestedRetry != retryGeneration)
    {
        retryGeneration = requestedRetry;
        if (state.device && vkDeviceWaitIdle(state.device) != VK_SUCCESS)
            return Fail("the Vulkan device could not retire work for retry");
        Shutdown();
        state.failed = false;
        state.reason = "";
    }
    if (state.failed)
        return false;

    auto depthResource = WrapImage(depthInfo, frame.DepthReadWrite);
    auto motionResource = WrapImage(motionInfo, frame.MotionReadWrite);

    if (colourInfo.ImageView == VK_NULL_HANDLE ||
        depthInfo.ImageView == VK_NULL_HANDLE ||
        motionInfo.ImageView == VK_NULL_HANDLE)
        return false;

    uint32_t width = colourInfo.Width;
    uint32_t height = colourInfo.Height;
    const auto renderWidth = frame.RenderSubrectWidth, renderHeight = frame.RenderSubrectHeight;
    const auto baseX = frame.ColorSubrectBaseX, baseY = frame.ColorSubrectBaseY;
    if (beforeSr)
    {
        // Origin-zero padded inputs are common with dynamic resolution. Never use a preset table.
        if (baseX || baseY || ((renderWidth == 0) != (renderHeight == 0)) || renderWidth > width ||
            renderHeight > height)
            return false; // caller falls back to post-SR, without editing the input
        if (renderWidth && renderHeight)
        {
            width = renderWidth;
            height = renderHeight;
        }
    }
    const auto depthX = frame.DepthSubrectBaseX, depthY = frame.DepthSubrectBaseY;
    const auto motionX = frame.MotionSubrectBaseX, motionY = frame.MotionSubrectBaseY;
    const auto outputWidth = frame.OutputWidth ? frame.OutputWidth : target.Width;
    const auto outputHeight = frame.OutputHeight ? frame.OutputHeight : target.Height;
    const auto guides =
        ResolveGuideRegions({ depthInfo.Width, depthInfo.Height },
                            { motionInfo.Width, motionInfo.Height },
                            { renderWidth, renderHeight }, { outputWidth, outputHeight },
                            frame.MotionVectorsLowResolution, depthX, depthY, motionX, motionY);
    if (!guides.depth.valid() || !guides.motion.valid())
        return false;
    const auto guideWidth = guides.depth.width, guideHeight = guides.depth.height;

    if (!width || !height || width > target.Width || height > target.Height)
        return false;

    // The model uses working resolution; source and composition remain native.
    float workScale = cfg.DlssNrWorkingScale.value_or_default();
    workScale = std::isfinite(workScale) ? std::clamp(workScale, 0.25f, 2.0f) : 1.0f;
    const uint32_t workWidth = std::max(1u, (uint32_t) (width * workScale + 0.5f));
    const uint32_t workHeight = std::max(1u, (uint32_t) (height * workScale + 0.5f));
    const bool reduced = workWidth != width || workHeight != height;
    const unsigned int passes =
        std::clamp(cfg.DlssNrPasses.value_or_default(), 1u,
                   cfg.DlssNrUnlockPasses.value_or_default() ? DlssNr::MaxPassCount : DlssNr::DefaultMaxPassCount);

    state.instance = instance;
    state.physicalDevice = physicalDevice;

    // The shader belongs to one device. Replacement features own replacement models.
    if (state.device != VK_NULL_HANDLE && state.device != device)
        return Fail("a Vulkan model was dispatched on a different device");
    state.device = device;

    if (!PrepareModels(cmdBuffer, frame, width, height, workWidth, workHeight, workScale, passes))
        return false;

    // -----------------------------------------------------------------------------------------
    // Encode: the frame the upscaler wrote -> a display-referred proxy, plus an untouched copy
    // -----------------------------------------------------------------------------------------

    const bool gameSaysHdr = frame.ColourIsLinearHdr;
    const bool depthInverted = frame.DepthInverted;

    if (frame.Reset)
        state.reset = true;

    // Both have to agree. A game can set the HDR flag on a buffer that cannot hold open-ended light,
    // and encoding an already tone-mapped frame a second time looks washed out and banded.
    const bool linearHdr = gameSaysHdr && FormatCanHoldLinearHdr(colourInfo.Format);

    float whitePoint = cfg.DlssNrWhitePointScale.value_or_default();

    if (!saidEncoding)
    {
        saidEncoding = true;
        LOG_INFO("DLSS-NR Vulkan: the game's buffer is {} (flag {}, format {}), depth {}",
                 linearHdr ? "linear HDR" : "already tone-mapped", gameSaysHdr ? "set" : "clear",
                 (int) colourInfo.Format, depthInverted ? "inverted" : "normal");
    }

    if (frame.WhitePointOverride > 0.0f)
        whitePoint = frame.WhitePointOverride;

    auto encode = DlssNr_Common::MakeConstants(DlssNrMode_Encode, width, height, whitePoint, linearHdr, cfg);
    encode.GuideWidth = guideWidth;
    encode.GuideHeight = guideHeight;

    // Open the measurement. Reset immediately before writing: a query pool slot must be reset before
    // it is written again, and doing it here rather than at the end keeps the two in one place.
    const uint32_t timingSlot = (uint32_t) (state.timedFrames % kTimingSlots);

    if (state.queryPool != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(cmdBuffer, state.queryPool, timingSlot * 2, 2);
        vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, state.queryPool, timingSlot * 2);
    }

    // The game's colour is read here and written at the end. Its layout on arrival is GENERAL, which
    // is what NGX requires of a resource it is handed, so it is left alone.
    Transition(cmdBuffer, state.proxy, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmdBuffer, state.keep, VK_IMAGE_LAYOUT_GENERAL);

    // Read the caller's actual input layout; the resolve restores it after writing.
    if (!state.pass->Dispatch(cmdBuffer, encode, colourInfo.ImageView,
                              VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, state.proxy.info.ImageView,
                              state.keep.info.ImageView, inputLayout))
        return Fail("the encode dispatch failed");

    // The model's input: the full proxy, or a downsampled copy of it when the working scale is below
    // the frame. Mirrors the D3D12 path -- the encode always writes a full proxy, and a separate
    // downsample makes the small one the model actually reads.
    ImageVk* modelInput = &state.proxy;

    if (reduced)
    {
        bool built = false;

        if (workScale > 1.0f)
        {
            // Supersample: upscale the proxy to the super-native working size with the chosen filter so
            // the model sees a clean input. Rebuild both scalers when the NR downscaler changed (baked
            // at construction). proxy -> SHADER_READ_ONLY (sampled), proxySmall -> GENERAL (storage).
            const Scaler wantScaler = cfg.DlssNrScalingDownscaler.value_or_default();
            if (state.nrScaler != wantScaler)
            {
                // Drain submitted work before replacing filter pipelines and descriptor resources.
                if (state.device != VK_NULL_HANDLE && vkDeviceWaitIdle(state.device) != VK_SUCCESS)
                    return Fail("the Vulkan device could not retire the supersampling filters");
                state.superUp.reset();
                state.superDown.reset();
                state.nrScaler = wantScaler;
            }
            if (!state.superUp)
                state.superUp =
                    std::make_unique<OS_Vk>("DLSS-NR VK supersample up", device, physicalDevice, true, wantScaler);
            if (!state.superDown)
                state.superDown = std::make_unique<OS_Vk>("DLSS-NR VK supersample down", device, physicalDevice,
                                                          false, wantScaler);

            Transition(cmdBuffer, state.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, state.proxySmall, VK_IMAGE_LAYOUT_GENERAL);

            VkImageInfo upin = state.proxy.info;
            VkImageInfo upout = state.proxySmall.info;

            built = state.superUp->IsInit() && state.superUp->DispatchResources(cmdBuffer, upin, upout);
            if (!built && !warnedVkSuper)
            {
                warnedVkSuper = true;
                LOG_WARN("DLSS-NR Vulkan supersample: upscaler unavailable, falling back to box enlarge.");
            }
        }

        if (!built)
        {
            DlssNrConstants down = encode;
            down.Mode = DlssNrMode_Downsample;
            down.Width = workWidth;
            down.Height = workHeight;

            Transition(cmdBuffer, state.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, state.proxySmall, VK_IMAGE_LAYOUT_GENERAL);

            if (!state.pass->Dispatch(cmdBuffer, down, state.proxy.info.ImageView, VK_NULL_HANDLE,
                                      VK_NULL_HANDLE, VK_NULL_HANDLE, state.proxySmall.info.ImageView, VK_NULL_HANDLE,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
                return Fail("the downsample dispatch failed");
        }

        modelInput = &state.proxySmall;
    }

    // -----------------------------------------------------------------------------------------
    // The model
    // -----------------------------------------------------------------------------------------

    float mvX = frame.MvScaleX, mvY = frame.MvScaleY;
    // Match D3D12: preserve the game's vector encoding, then adjust only for the NR working scale.
    mvX *= (float) workWidth / width;
    mvY *= (float) workHeight / height;
    ImageVk* answer = &state.output;
    ImageVk* input = modelInput;
    uint32_t clampSlots[2] = { UINT32_MAX, UINT32_MAX };
    NVSDK_NGX_Result evaluated = NVSDK_NGX_Result_Success;
    for (unsigned int pass = 0; pass < passes; ++pass)
    {
        Transition(cmdBuffer, *input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, *answer, VK_IMAGE_LAYOUT_GENERAL);
        auto inputResource = WrapImage(input->info, true);
        auto answerResource = WrapImage(answer->info, true);
        evaluated = EvaluateModel(cmdBuffer, pass, &inputResource, &depthResource, &motionResource, &answerResource,
                                  workWidth, workHeight, guides, depthInverted, mvX, mvY, cfg);
        if (evaluated != NVSDK_NGX_Result_Success)
            break;
        if (pass + 1 < passes)
        {
            Transition(cmdBuffer, *answer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, state.passClamp, VK_IMAGE_LAYOUT_GENERAL);
            DlssNrConstants clamp {};
            clamp.Mode = DlssNrMode_ClampProxy;
            clamp.Width = workWidth;
            clamp.Height = workHeight;
            if (!state.pass->Dispatch(cmdBuffer, clamp, answer->info.ImageView, VK_NULL_HANDLE,
                                      VK_NULL_HANDLE, VK_NULL_HANDLE, state.passClamp.info.ImageView, VK_NULL_HANDLE,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false, &clampSlots[pass % 2]))
            {
                evaluated = NVSDK_NGX_Result_Fail;
                break;
            }
            input = &state.passClamp;
            answer = answer == &state.output ? &state.scratch : &state.output;
        }
    }

    state.reset = evaluated != NVSDK_NGX_Result_Success;
    state.frames++;

    if (evaluated != NVSDK_NGX_Result_Success)
    {
        LOG_ERROR("DLSS-NR Vulkan: evaluate returned 0x{:X}", static_cast<unsigned int>(evaluated));
        return Fail("the Neural Rendering pass failed");
    }

    // -----------------------------------------------------------------------------------------
    // Resolve: proxy + the model's answer + the untouched copy -> the frame
    // -----------------------------------------------------------------------------------------

    DlssNrConstants resolve = encode;
    resolve.Mode = DlssNrMode_Resolve;

    // Downsample the model answer to native before composition.
    ImageVk* resolveProxy = modelInput;
    ImageVk* resolveAnswer = answer;

    if (workScale > 1.0f && state.superDown && state.superDown->IsInit())
    {
        Transition(cmdBuffer, *answer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, state.outputNative, VK_IMAGE_LAYOUT_GENERAL);

        VkImageInfo dsin = answer->info;
        VkImageInfo dsout = state.outputNative.info;

        if (state.superDown->DispatchResources(cmdBuffer, dsin, dsout))
        {
            resolveProxy = &state.proxy;
            resolveAnswer = &state.outputNative;
        }
    }

    Transition(cmdBuffer, *resolveProxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmdBuffer, *resolveAnswer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmdBuffer, state.keep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (!state.pass->Dispatch(cmdBuffer, resolve, resolveProxy->info.ImageView,
                              resolveAnswer->info.ImageView, state.keep.info.ImageView, VK_NULL_HANDLE, target.ImageView, VK_NULL_HANDLE,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
        return Fail("the resolve dispatch failed");

    // Close it, and read the pair from three frames ago -- retired by now, so the read does not wait.
    if (state.queryPool != VK_NULL_HANDLE)
    {
        vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, state.queryPool, timingSlot * 2 + 1);
        state.timedFrames++;

        if (state.timedFrames > kTimingSlots)
        {
            const uint32_t readSlot = (uint32_t) (state.timedFrames % kTimingSlots);
            uint64_t ticks[2] = {};

            // Without WAIT: a slot this old is retired, and if it somehow is not, NOT_READY is the
            // right answer rather than a stall.
            if (vkGetQueryPoolResults(device, state.queryPool, readSlot * 2, 2, sizeof(ticks), ticks,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                ticks[1] > ticks[0])
            {
                const double ms = (double) (ticks[1] - ticks[0]) * (double) state.timestampPeriod / 1e6;

                // A pass that appears to have taken over a second did not; the queue was reset under
                // it or the pair straddled a device change.
                if (ms > 0.0 && ms < 1000.0)
                    state.lastGpuTime = ms;
            }
        }
    }

    if (!reported && state.frames > 2)
    {
        reported = true;
        LOG_INFO("DLSS-NR Vulkan: running {} SR at {}x{}, guides {}x{}", beforeSr ? "before" : "after", width,
                 height, guideWidth, guideHeight);
    }
    return true;
}


ModelVk::ModelVk(DlssNr_Vk& shader) : _impl(std::make_unique<Impl>(shader)) {}
ModelVk::~ModelVk() = default;
bool ModelVk::Evaluate(VkCommandBuffer cmd, const VkImageInfo& colour, const VkImageInfo& depth,
                       const VkImageInfo& motion, const VkImageInfo& output, const DlssNrFrameInfo_Vk& frame,
                       VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, VkImageLayout inputLayout)
{
    return _impl->Evaluate(cmd, colour, depth, motion, output, frame, instance, physicalDevice, device, inputLayout);
}
} // namespace DlssNr
