#pragma once

#include "../shaders/dlssnr/DlssNr_Guides.h"
#include <nvsdk_ngx_params.h>

namespace DlssNr
{
struct ModelSettings
{
    unsigned int preset = 0, style = 0;
    float intensity = 1.0f, localStructure = 1.0f, localTone = 0.0f, skinStructure = -1.0f;
    bool autoMask = true;
    bool operator==(const ModelSettings&) const = default;
};

// Borrowed inputs; Vulkan uses void pointers to NGX image wrappers, DX12 uses typed resources.
template <typename Resource> struct ModelFrame
{
    Resource *color = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr;
    GuideExtent size {};
    GuideRegions guides {};
    bool depthInverted = false, reset = false;
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
};

inline void SetModelTuning(NVSDK_NGX_Parameter* params, const ModelSettings& settings)
{
    params->Set("DLSSNR.Intensity", settings.intensity);
    params->Set("DLSSNR.Style", settings.style);
    params->Set("DLSSNR.LocalStructureStrength", settings.localStructure);
    params->Set("DLSSNR.LocalToneStrength", settings.localTone);
    params->Set("DLSSNR.SkinStructureStrength", settings.skinStructure);
    params->Set("DLSSNR.UseAutoMask", settings.autoMask ? 1u : 0u);
}

inline void SetModelCreation(NVSDK_NGX_Parameter* params, const ModelSettings& settings, unsigned width,
                             unsigned height)
{
    params->Set("DLSSNR.Enabled", 1u);
    params->Set("DLSSNR.Width", width);
    params->Set("DLSSNR.Height", height);
    params->Set("CreationNodeMask", 1u);
    params->Set("VisibilityNodeMask", 1u);
    params->Set("DLSSNR.Hint.Render.Preset", settings.preset);
    params->Set("DLSSNR.UICorrection", 1u);
    SetModelTuning(params, settings);
}

// The game supplies the same guide metadata on both APIs; resource wrappers and reset types differ.
template <typename Frame>
void ReadModelGuides(NVSDK_NGX_Parameter* params, unsigned flags, Frame& frame)
{
    frame.DepthInverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.MotionVectorsLowResolution = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX);
    params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &frame.DepthSubrectBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &frame.DepthSubrectBaseY);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &frame.MotionSubrectBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &frame.MotionSubrectBaseY);
}

inline void SetModelRegions(NVSDK_NGX_Parameter* params, GuideExtent size, const GuideRegions& guides)
{
    params->Set("DLSSNR.ColorSubrectBaseX", 0u);
    params->Set("DLSSNR.ColorSubrectBaseY", 0u);
    params->Set("DLSSNR.ColorSubrectWidth", size.width);
    params->Set("DLSSNR.ColorSubrectHeight", size.height);
    params->Set("DLSSNR.OutputSubrectBaseX", 0u);
    params->Set("DLSSNR.OutputSubrectBaseY", 0u);
    params->Set("DLSSNR.OutputSubrectWidth", size.width);
    params->Set("DLSSNR.OutputSubrectHeight", size.height);
    params->Set("DLSSNR.DepthSubrectBaseX", guides.depth.x);
    params->Set("DLSSNR.DepthSubrectBaseY", guides.depth.y);
    params->Set("DLSSNR.DepthSubrectWidth", guides.depth.width);
    params->Set("DLSSNR.DepthSubrectHeight", guides.depth.height);
    params->Set("DLSSNR.MVecSubrectBaseX", guides.motion.x);
    params->Set("DLSSNR.MVecSubrectBaseY", guides.motion.y);
    params->Set("DLSSNR.MVecSubrectWidth", guides.motion.width);
    params->Set("DLSSNR.MVecSubrectHeight", guides.motion.height);
}

template <typename Resource>
void SetModelEvaluation(NVSDK_NGX_Parameter* params, const ModelFrame<Resource>& frame,
                        const ModelSettings& settings, bool forceReset = false)
{
    params->Set("DLSSNR.Color", frame.color);
    params->Set("DLSSNR.Depth", frame.depth);
    params->Set("DLSSNR.MVec", frame.motion);
    params->Set("DLSSNR.Output", frame.output);
    params->Set("DLSSNR.Enabled", 1u);
    params->Set("DLSSNR.Width", frame.size.width);
    params->Set("DLSSNR.Height", frame.size.height);
    params->Set("DLSSNR.DepthInverted", frame.depthInverted ? 1u : 0u);
    params->Set("DLSSNR.Reset", (frame.reset || forceReset) ? 1u : 0u);
    SetModelRegions(params, frame.size, frame.guides);
    params->Set("DLSSNR.MVecScaleX", frame.mvScaleX);
    params->Set("DLSSNR.MVecScaleY", frame.mvScaleY);
    SetModelTuning(params, settings);
}
}
