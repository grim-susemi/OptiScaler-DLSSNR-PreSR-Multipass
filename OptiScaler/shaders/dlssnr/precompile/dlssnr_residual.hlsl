// MV-reprojected temporal accumulation of the pre-SR NR residual.
// Mode 0 blends (edited - original) into history; invalid reprojection fades in from zero.
// Mode 1 applies the signed residual after upscaling.
// Bindings match the main NR shader; appended history fields fit its existing constant-buffer allocation.

#define NR_RESIDUAL
#include "dlssnr_common.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    if (gMode == 0)
    {
        float3 delta = SanitizeFinite3(gModel.Load(int3(id.xy, 0)).rgb -
                                       gSource.Load(int3(id.xy, 0)).rgb, float3(0.0, 0.0, 0.0));

        float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
        uint2 guideSize = uint2(gGuideWidth, gGuideHeight);
        uint2 guidePos = min(uint2(uv * guideSize), guideSize - 1) +
                         uint2(gResidualMotionBaseX, gResidualMotionBaseY);
        float2 motion = gMotion.Load(int3(guidePos, 0)).xy * float2(gMvScaleX, gMvScaleY);
        float2 prevUV = uv + motion;

        bool valid = gResidualHistoryValid != 0 && all(isfinite(motion)) && all(abs(motion) < 2.0) &&
                     all(prevUV >= 0.0) && all(prevUV <= 1.0);

        float3 history = valid ? gOriginal.SampleLevel(gLinear, prevUV, 0).rgb : float3(0.0, 0.0, 0.0);
        history = SanitizeFinite3(history, float3(0.0, 0.0, 0.0));

        // Invalid reprojection: history is 0, so the pixel fades in from no edit at the normal blend
        // rate over the next frames. A cold start/cut also fades in, without sampling uninitialized history.
        float a = clamp(gResidualBlend, 0.0, 1.0);

        gTarget[id.xy] = float4(lerp(history, delta, a), 1.0);
        return;
    }

    if (gMode == 1)
    {
        float4 base  = gSource.Load(int3(id.xy, 0));
        float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
        float3 delta = SanitizeFinite3(gModel.SampleLevel(gLinear, uv, 0).rgb, float3(0.0, 0.0, 0.0));

        gTarget[id.xy] = float4(max(base.rgb + delta * gTransferStrength, 0.0), base.a);
        return;
    }

    gTarget[id.xy] = gSource.Load(int3(id.xy, 0));
}
