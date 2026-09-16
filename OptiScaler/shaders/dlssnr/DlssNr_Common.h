#pragma once

// Shared frame metadata and the C++/HLSL constant-buffer contract.

#include <algorithm>
#include <cstddef>
#include <cstdint>

enum DlssNrMode : uint32_t
{
    DlssNrMode_Encode = 0,         // the frame -> a tone-mapped proxy, plus an untouched copy
    DlssNrMode_Resolve = 1,        // proxy + the model's answer + the untouched copy -> the edited frame
    DlssNrMode_Downsample = 2,     // the proxy -> a smaller proxy, when the model works below full size
    DlssNrMode_EncodeResidual = 5, // NR-composed minus original; signed difference encoded around 0.5
    DlssNrMode_ApplyResidual = 6,  // decode private DLSS result and add to clean SR output
    DlssNrMode_UnitExposure = 7,   // constant exposure for the private DLSS feature
    DlssNrMode_ClampProxy = 8,     // restore the encoded RGB range between model passes
    DlssNrMode_EncodeProxyResidual = 9,
    DlssNrMode_ResizePrivateGuides = 10
};

constexpr uint32_t kDlssNrHdrCurveBins = 48;

// Caller-supplied buffer conventions. User controls remain in Config; allocation sizes come from resources.
struct DlssNrFrameInfo
{
    uint32_t Width = 0, Height = 0, GuideWidth = 0, GuideHeight = 0;
    bool DepthInverted = false;

    // Game-reported motion scale; guides retain independent active regions.
    float MvScaleX = 1.0f;
    float MvScaleY = 1.0f;

    bool Reset = false;

    // True for scene-linear colour; false for an already tone-mapped image.
    bool ColourIsLinearHdr = true;

    bool BeforeUpscale = false;
    bool FinishedPicture = false;
    // DX12 internal calls provide the actual arrival state and restore it on return.
    uint32_t OutputArrivalState = 0;
    float WhitePointOverride = 0.0f;
    bool IndependentCommands = false; // owned command list, no game root signature to restore
    bool RayReconstruction = false;

    // Creation and evaluation must occur in different submission epochs.
    unsigned long long SubmissionEpoch = 0;
    float FrameTimeMs = 16.67f;

    // Scene pre-exposure for residual transport and finished-picture response matching.
    float PreExposure = 1.0f;

    // Active raster inside a potentially larger allocation; zero uses the resource dimensions.
    unsigned int RenderSubrectWidth = 0;
    unsigned int RenderSubrectHeight = 0;

    unsigned int DepthSubrectBaseX = 0;
    unsigned int DepthSubrectBaseY = 0;
    unsigned int MotionSubrectBaseX = 0;
    unsigned int MotionSubrectBaseY = 0;

    // Select render- or output-resolution motion regions from the game's feature flags.
    bool MotionVectorsLowResolution = false;
    unsigned int OutputWidth = 0;
    unsigned int OutputHeight = 0;
};

// Field order matches the HLSL cbuffers; DX12 constant buffers require 256-byte alignment.
struct alignas(256) DlssNrConstants
{
    uint32_t Mode;
    float WhitePoint;

    uint32_t Width;
    uint32_t Height;

    float TransferStrength;
    float ColourStrength;

    uint32_t DebugView;

    float MaxRatio;

    uint32_t Passthrough;

    float MvScaleX;
    float MvScaleY;

    uint32_t GuideWidth;
    uint32_t GuideHeight;

    // 0 off, 1 side by side, 2 wipe. CompareZoom: 1 fit, 2 fill/crop.
    uint32_t CompareMode;
    float CompareSplit;

    float CompareZoom;

    uint32_t CompareSwap;

    // 0 classic, 1 spatial matched residual, 2 privately upscaled residual.
    // Matched residual composition and cube scaling are based on hhkbble's multipass contribution.
    uint32_t Transfer;

    float DebugScale;

    // 0 soft knee; 1/2 Neutwo compose/replace; 3/4 hybrid compose/replace.
    uint32_t ReversibleMode;

    // Zero shows the clean frame while keeping model evaluation active.
    uint32_t ApplyModel;

    uint32_t Reserved; // Preserve the shared constant-buffer layout.
    float ResidualScale; // Scene pre-exposure used to encode/decode the private residual carrier.
    uint32_t SkinProtection;
    uint32_t ShowSkinMask;
    float SkinDetail;
    float SkinColour;
    float EnvironmentDetail;
    float EnvironmentColour;

    float ResidualBlend;
    uint32_t ResidualHistoryValid;
    uint32_t ResidualMotionBaseX;
    uint32_t ResidualMotionBaseY;
};
static_assert(sizeof(DlssNrConstants) == 256);

// Operation numbers for the separate residual shader.
enum DlssNrResidualMode : uint32_t
{
    DlssNrResidualMode_Accumulate = 0, // (edited - original) blended into the reprojected history
    DlssNrResidualMode_Apply = 1,      // base + delta * TransferStrength, after RR+SR
};

class DlssNr_Common
{
  public:
    // Common composition controls for DX12 and Vulkan.
    template <typename ConfigType>
    static DlssNrConstants MakeConstants(DlssNrMode mode, uint32_t width, uint32_t height, float whitePoint,
                                         bool linearHdr, const ConfigType& config)
    {
        DlssNrConstants constants {};
        constants.Mode = mode;
        constants.Width = width;
        constants.Height = height;
        constants.WhitePoint = whitePoint;
        constants.Passthrough = linearHdr ? 0u : 1u;
        constants.TransferStrength = config.DlssNrTransferStrength.value_or_default();
        constants.ColourStrength = config.DlssNrColourStrength.value_or_default();
        constants.DebugView = config.DlssNrDebugView.value_or_default();
        constants.MaxRatio = config.DlssNrMaxRatio.value_or_default();
        constants.Transfer = std::min(config.DlssNrTransfer.value_or_default(), 1u);
        constants.DebugScale = config.DlssNrWhitePointScale.value_or_default();
        constants.CompareMode = config.DlssNrCompare.value_or_default();
        constants.CompareSplit = config.DlssNrCompareSplit.value_or_default();
        constants.CompareZoom = std::max(1.0f, config.DlssNrCompareZoom.value_or_default());
        constants.CompareSwap = config.DlssNrCompareSwap.value_or_default() ? 1u : 0u;
        constants.ReversibleMode = config.DlssNrReversibleMode.value_or_default();
        constants.ApplyModel = config.DlssNrApplyModel.value_or_default() ? 1u : 0u;
        constants.SkinProtection = config.DlssNrSkinProtection.value_or_default() ? 1u : 0u;
        constants.ShowSkinMask = config.DlssNrShowSkinMask.value_or_default() ? 1u : 0u;
        constants.SkinDetail = config.DlssNrSkinDetail.value_or_default();
        constants.SkinColour = config.DlssNrSkinColour.value_or_default();
        constants.EnvironmentDetail = config.DlssNrEnvironmentDetail.value_or_default();
        constants.EnvironmentColour = config.DlssNrEnvironmentColour.value_or_default();
        return constants;
    }

};
