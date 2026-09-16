#include "pch.h"
#include "DlssNr_Dx12_State.h"

void DlssNr_Dx12::State::CopyMeterToReadback(ID3D12GraphicsCommandList* cmd)
{
    const auto slot = nr.meterFrames % 4;
    auto* readback = nr.meterReadback[slot];
    if (!readback) return;
    nr.meterExposureValid[slot] = true;
    D3D12_TEXTURE_COPY_LOCATION source {};
    source.pResource = nr.meter;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION target {};
    target.pResource = readback;
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, kDlssNrMeterGrid, kDlssNrMeterGrid,
                                       1, kMeterRowBytes };
    Barrier(cmd, nr.meter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    Barrier(cmd, nr.meter, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ++nr.meterFrames;
}

auto DlssNr_Dx12::State::ConsumeMeterReadback() -> void
{
    if (nr.meterFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (nr.meterFrames % 4);
    ID3D12Resource* buffer = nr.meterReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, sizeof(float) };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    // Keep the last valid exposure during gaps; a fallback source texel is not an exposure sample.
    if (nr.meterExposureValid[slot] && std::isfinite(src[0]) && src[0] > 0.0f)
        nr.gameExposure = src[0];

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);
}

auto DlssNr_Dx12::State::InvalidateExposureMeter() -> void
{
    nr.gameExposure = 0.0f;

    for (bool& valid : nr.meterExposureValid)
        valid = false;

    // Re-arms the `< 4` guard in ConsumeMeterReadback, so nothing is read back until four frames
    // have genuinely been queued since this point.
    nr.meterFrames = 0;
}

float DlssNr_Dx12::State::ResolveWhitePoint(const Config& cfg, bool isHdrBuffer)
{
    const float manual = cfg.DlssNrWhitePointScale.value_or_default();
    if (!isHdrBuffer) return manual;

    switch (cfg.DlssNrWhitePointSource.value_or_default())
    {
    case 2:
    {
        const float anchored = DlssNr::ExposureScan::AnchoredWhitePoint(
            DlssNr::ExposureScan::BestValue(), cfg.DlssNrScanInverted.value_or_default(),
            cfg.DlssNrScanTrim.value_or_default());
        return anchored > 0.0f ? anchored : manual;
    }
    case 1:
        if (nr.gameExposure > 1e-6f)
        {
            // Reverse the game's exposure using a separate trim; never overwrite the manual setting.
            const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
            return std::clamp(nr.gamePreExposure / nr.gameExposure * trim, 0.01f, 4096.0f);
        }
    }
    return manual;
}
