#pragma once
#include <dlssnr/DlssNr_Proxy.h>
#include <dlssnr/PassProfiles.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <shaders/output_scaling/OS_Dx12.h>

namespace DlssNr::Detail
{
struct ModelStateDx12
{
    unsigned long long successfulDispatches = 0;
    // Each model pass owns its NGX feature, parameters and temporal history.
    DlssNr::Proxy::Context models[DlssNr::MaxPassCount];
    bool passCreateFailed[DlssNr::MaxPassCount] = {};

    // The model cannot read and write one resource, so the frame is staged through these.
    ID3D12Resource* colorCopy = nullptr;
    ID3D12Resource* output = nullptr;

    // The second half of the model-output ping-pong. The base proxy stays immutable: pass 0 writes
    // output (A), pass 1 writes this (B), and pass 2 writes A again. Only the final answer is composed.
    ID3D12Resource* passScratch = nullptr;
    bool passScratchFailed = false;
    ID3D12Resource* passClamp = nullptr; // bounded input for the next model pass

    // The frame as the upscaler wrote it. The resolve adds the model's edit to this rather than
    // reconstructing it by inverting the tone curve, which is what turned every light in the frame into
    // a string of coloured cells.
    ID3D12Resource* hdrCopy = nullptr;

    // Compact origin-zero pre-SR image, only needed when Color has allocation padding. All codec,
    // hold and capture paths then see the real raster. UAV at rest, retired with the scratch set.
    ID3D12Resource* activeColor = nullptr;

    // The frame shrunk for the model, when it is working below full resolution.
    ID3D12Resource* colorSmall = nullptr;

    // Supersampling filters are allocated lazily; dispatch dimensions come from the resources.
    OS_Dx12* superUp = nullptr;

    // The down-leg returns the model answer to native size before composition.
    ID3D12Resource* outputNative = nullptr;
    OS_Dx12* superDown = nullptr;
    Scaler nrScaler = Scaler::Count;

    // Frame hold (design/frame-hold.md): a persistent copy of the output taken on hold-on and restored
    // over the live output before the encode reads it while held, so a setting change re-renders the
    // same frame. heldWhitePoint is the snapshot used while held -- measurement is suspended.
    ID3D12Resource* heldColor = nullptr;
    bool heldActive = false;
    unsigned int heldWidth = 0;
    unsigned int heldHeight = 0;
    DXGI_FORMAT heldFormat = DXGI_FORMAT_UNKNOWN;
    float heldWhitePoint = 1.0f;

    unsigned int workWidth = 0;
    unsigned int workHeight = 0;

    // The exposure readback ring contains the game's own exposure sample in texel zero.
    ID3D12Resource* meter = nullptr;
    ID3D12Resource* meterReadback[4] = {};

    // Validity travels with the readback slot: an unbound exposure slot contains fallback image data.
    bool meterExposureValid[4] = {};
    unsigned long long meterFrames = 0;

    // Only the setting off/on edge invalidates the held exposure; missing textures do not.
    bool exposureSettingWasOn = false;

    // The game's exposure, as last read back, and the pre-exposure that goes with it. Held rather
    // than defaulted: the texture comes and goes between frames and a fallback to 1.0 on the gaps
    // would be a flicker source.
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;

    // Track current and historical exposure availability separately for status reporting.
    bool exposureOfferedNow = false;
    bool exposureEverOffered = false;
    unsigned long long exposureFrames = 0;

    // Cloned unconditionally when running at present, and only for typeless formats otherwise.
    ID3D12Resource* depthClone = nullptr;
    ID3D12Resource* motionClone = nullptr;

    unsigned int width = 0;
    unsigned int height = 0;
    bool beforeUpscale = false;
    bool rayReconstruction = false;
    bool reset = true;

    // The preset, style and strengths each live feature was created with.
    ModelSettings builtSettings[DlssNr::MaxPassCount] {};

    // Latch failures until an explicit retry rather than recording failing GPU work every frame.
    bool failed = false;
    const char* reason = "";
};
}
