#include "pch.h"
#include "DlssNr_Dx12_State.h"

auto DlssNr_Dx12::State::ForgetCalibration() -> void
{
    nr.calibCount = 0;
    nr.calibSuggestion = 0.0f;
    nr.calibSteadiness = 0.0f;
    nr.calibUsable = false;
    nr.calibWhy = "measuring...";
}

void DlssNr_Dx12::State::CopyGridToReadback(ID3D12GraphicsCommandList* cmd, ID3D12Resource* grid,
                                          ID3D12Resource* readback)
{
    D3D12_TEXTURE_COPY_LOCATION source {};
    source.pResource = grid;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION target {};
    target.pResource = readback;
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, kDlssNrMeterGrid, kDlssNrMeterGrid,
                                       1, kMeterRowBytes };
    Barrier(cmd, grid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    Barrier(cmd, grid, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void DlssNr_Dx12::State::CopyCalibrationToReadback(ID3D12GraphicsCommandList* cmd)
{
    auto* readback = nr.calibReadback[nr.calibFrames % 4];
    if (!readback || !nr.calib) return;
    CopyGridToReadback(cmd, nr.calib, readback);
    ++nr.calibFrames;
}

void DlssNr_Dx12::State::CopyMeterToReadback(ID3D12GraphicsCommandList* cmd)
{
    const auto slot = nr.meterFrames % 4;
    if (!nr.meterReadback[slot]) return;
    nr.meterExposureValid[slot] = true;
    CopyGridToReadback(cmd, nr.meter, nr.meterReadback[slot]);
    ++nr.meterFrames;
}

auto DlssNr_Dx12::State::ConsumeCalibrationReadback() -> void
{
    if (nr.calibFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (nr.calibFrames % 4);
    ID3D12Resource* buffer = nr.calibReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, kMeterBytes };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    std::vector<float> tiles;
    tiles.reserve(kDlssNrMeterGrid * kDlssNrMeterGrid);

    for (unsigned int i = 0; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i)
    {
        if (std::isfinite(src[i]) && src[i] > 1e-6f)
            tiles.push_back(src[i]);
    }

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);

    if (tiles.size() < 16)
        return;

    const size_t nth = (size_t) ((float) (tiles.size() - 1) * 0.90f);
    std::nth_element(tiles.begin(), tiles.begin() + nth, tiles.end());

    // How much of the frame carries light, measured against its own brightest tile rather than an
    // absolute threshold -- the units here are the game's and there is no absolute scale.
    //
    // This is what separates "the buffer is scaled by 240" from "I am standing in a dark cave". A
    // percentile of tile peaks is a statement about scene content; it only describes the buffer when
    // enough of the picture is lit for the top of the range to actually appear in it.
    float brightest = 0.0f;

    for (float v : tiles)
        brightest = std::max(brightest, v);

    unsigned int lit = 0;

    for (float v : tiles)
    {
        if (v > brightest * 0.10f)
            ++lit;
    }

    const float litFraction = tiles.empty() ? 0.0f : (float) lit / (float) tiles.size();

    // A torn readback survives isfinite and would clamp to exactly the ceiling, which since the
    // ceiling became 2000 is a value the slider can hold -- so a garbage frame could be offered as a
    // real answer. Reject rather than clamp.
    if (!(tiles[nth] > 0.0f) || tiles[nth] >= 1999.0f)
        return;

    const float suggestion = std::clamp(tiles[nth], 0.25f, 1990.0f);

    nr.calibUsable = !nr.calibPassthrough && litFraction > 0.20f;
    nr.calibWhy = nr.calibPassthrough
                      ? "this game hands over a frame it already tone mapped, so there is nothing to normalise"
                  : litFraction <= 0.20f ? "too little of this scene is lit to say where the top of the range is"
                                         : "";

    nr.calibHistory[nr.calibCount % NrState::kCalibHistory] = suggestion;
    nr.calibCount++;
    nr.calibSuggestion = suggestion;

    // Confidence is the spread of recent answers, not their absolute size. A number that has held
    // still for a second is one worth taking; one that is swinging means the scene is changing under
    // the measurement, and no single value would serve anyway.
    const unsigned int have = std::min<unsigned int>(nr.calibCount, NrState::kCalibHistory);

    if (have >= 8)
    {
        float lo = nr.calibHistory[0];
        float hi = nr.calibHistory[0];

        for (unsigned int i = 0; i < have; ++i)
        {
            lo = std::min(lo, nr.calibHistory[i]);
            hi = std::max(hi, nr.calibHistory[i]);
        }

        // A spread of 1.0x is perfect agreement and 2x or worse is none.
        const float spread = hi / lo;
        nr.calibSteadiness = std::clamp(1.0f - (spread - 1.0f), 0.0f, 1.0f);
    }
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

auto DlssNr_Dx12::State::Calibration() -> DlssNr::CalibrationReading
{
    CalibrationReading r {};
    r.suggestion = nr.calibSuggestion;
    r.steadiness = nr.calibSteadiness;
    r.samples = nr.calibCount;
    r.usable = nr.calibUsable;
    r.why = nr.calibWhy;
    return r;
}
