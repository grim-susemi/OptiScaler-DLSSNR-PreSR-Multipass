// Shared NR shader contract; field order matches DlssNrConstants.
#ifdef VK_MODE
[[vk::binding(0, 0)]]
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    uint  gMode;
    float gWhitePoint;
    uint  gWidth;
    uint  gHeight;
    float gTransferStrength;
    float gColourStrength;
    uint  gDebugView;
    float gMaxRatio;
    uint  gPassthrough;
    float gMvScaleX;
    float gMvScaleY;
    uint  gGuideWidth;
    uint  gGuideHeight;
    uint  gCompareMode;
    float gCompareSplit;
    float gCompareZoom;
    uint  gCompareSwap;
    uint  gTransfer;
    float gDebugScale;
    uint  gReversibleMode;
    uint  gApplyModel;
    uint  gReserved;
    float gResidualScale;
    uint  gSkinProtection;
    uint  gShowSkinMask;
    float gSkinDetail;
    float gSkinColour;
    float gEnvironmentDetail;
    float gEnvironmentColour;
#ifdef NR_RESIDUAL
    float gResidualBlend;
    uint gResidualHistoryValid;
    uint gResidualMotionBaseX;
    uint gResidualMotionBaseY;
#endif
};

// Vulkan uses one binding namespace; DX12 uses separate b/t/u/s registers.
#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4>   gSource   : register(t0);
#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
Texture2D<float4>   gModel    : register(t1);
#ifdef VK_MODE
[[vk::binding(3, 0)]]
#endif
Texture2D<float4>   gOriginal : register(t2);
#ifdef VK_MODE
[[vk::binding(4, 0)]]
#endif
Texture2D<float4>   gMotion   : register(t3);
#if defined(NR_RESIDUAL) && !defined(VK_MODE)
Texture2D<float4>   gExposure : register(t4);
#endif
#ifdef VK_MODE
[[vk::binding(5, 0)]]
#endif
RWTexture2D<float4> gTarget   : register(u0);
#ifdef VK_MODE
[[vk::binding(6, 0)]]
#endif
RWTexture2D<float4> gKeep     : register(u1);
#ifdef VK_MODE
[[vk::binding(7, 0)]]
#endif
SamplerState        gLinear   : register(s0);

float SanitizeFinite(float v, float fallback) { return isfinite(v) ? v : fallback; }
float3 SanitizeFinite3(float3 v, float3 fallback)
{
    return float3(SanitizeFinite(v.x, fallback.x), SanitizeFinite(v.y, fallback.y),
                  SanitizeFinite(v.z, fallback.z));
}
