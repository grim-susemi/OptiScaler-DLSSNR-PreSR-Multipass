#include "pch.h"

#include "DlssNr_ExposureScan_Internal.h"

#include <Config.h>
#include <Util.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <mutex>
#include <string>

namespace DlssNr
{
namespace ExposureScan
{
namespace Detail
{

bool Wanted()
{
    return Config::Instance()->DlssNrWhitePointSource.value_or_default() == 2 ||
           Config::Instance()->DlssNrScanExposure.value_or_default();
}

} // namespace Detail
using namespace Detail;

// Describe small floating-point UAVs without duplicating output-parameter setup in both hooks.
std::optional<Tracked> DescribeCandidate(const D3D12_RESOURCE_DESC& desc)
{
    if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        return std::nullopt;
    Tracked candidate;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        if (desc.Width < candidate.bytes || desc.Width > 128)
            return std::nullopt;
        candidate.isBuffer = true;
        candidate.shape = "buffer, " + std::to_string(desc.Width) + " bytes";
        return candidate;
    }
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !desc.Width || !desc.Height ||
        desc.Width * desc.Height > 256)
        return std::nullopt;
    const char* name;
    switch (desc.Format)
    {
    case DXGI_FORMAT_R32_FLOAT: name = "R32_FLOAT"; break;
    case DXGI_FORMAT_R32G32_FLOAT: name = "R32G32_FLOAT"; break;
    case DXGI_FORMAT_R16_FLOAT: name = "R16_FLOAT"; candidate.bytes = 2; break;
    case DXGI_FORMAT_R16G16_FLOAT: name = "R16G16_FLOAT"; candidate.bytes = 2; break;
    default: return std::nullopt;
    }
    candidate.texFormat = desc.Format;
    candidate.shape = std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + " " + name;
    return candidate;
}

void Adopt(ID3D12Resource* resource, Tracked candidate)
{
    ID3D12Device* resourceDevice = nullptr;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&resourceDevice))) || resourceDevice == nullptr)
        return;
    const bool differentDevice = g_scan.device != nullptr && g_scan.device != resourceDevice;
    resourceDevice->Release();
    if (differentDevice)
        return;

    for (const Tracked& t : g_scan.tracked)
    {
        if (t.resource == resource)
            return;
    }

    if (g_scan.tracked.size() >= kMaxCandidates)
    {
        if (!g_scan.complained)
        {
            g_scan.complained = true;
            LOG_WARN("DLSS-NR exposure scan: more than {} candidates, so the filter is too loose here "
                     "rather than the game having {} exposures",
                     kMaxCandidates, kMaxCandidates);
        }

        return;
    }

    candidate.resource = resource;
    candidate.device = resourceDevice;
    resource->AddRef();
    LOG_INFO("DLSS-NR exposure scan: candidate {} -- {}", g_scan.tracked.size() + 1, candidate.shape);
    g_scan.tracked.push_back(std::move(candidate));
}

void NoteResource(const D3D12_RESOURCE_DESC* desc, ID3D12Resource* resource)
{
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
        return;

    if (desc == nullptr || resource == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_scanMutex);

    auto candidate = DescribeCandidate(*desc);
    if (!candidate)
    {
        // Bound diagnostic output for rejected UAVs.
        if ((desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 && g_scan.nearMissLogged < 40)
        {
            g_scan.nearMissLogged++;
            LOG_INFO("DLSS-NR scan near-miss #{}: UAV dim {} {}x{}x{} fmt {} (filter rejected)",
                     g_scan.nearMissLogged, (int) desc->Dimension, (unsigned int) desc->Width,
                     desc->Height, desc->DepthOrArraySize, (int) desc->Format);
        }

        return;
    }

    Adopt(resource, std::move(*candidate));
}

void NoteUav(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc)
{
    // Discover candidates before scanning is selected; GPU reads remain gated in Tick.
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
        return;

    if (resource == nullptr)
        return;

    const D3D12_RESOURCE_DESC rd = resource->GetDesc();

    std::lock_guard<std::mutex> lock(g_scanMutex);

    auto candidate = DescribeCandidate(rd);
    if (!candidate)
        return;

    Adopt(resource, std::move(*candidate));
}

float BestValue(float* outLowest, float* outHighest)
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    const Tracked* best = nullptr;
    float bestRatio = 0.0f;

    for (const auto& t : g_scan.tracked)
    {
        if (!t.moves || t.lowest <= kFloor)
            continue;

        const float ratio = t.highest / t.lowest;

        if (ratio > bestRatio)
        {
            bestRatio = ratio;
            best = &t;
        }
    }

    if (!best)
        return 0.0f;

    if (outLowest != nullptr)
        *outLowest = best->lowest;

    if (outHighest != nullptr)
        *outHighest = best->highest;

    return best->latest;
}

std::vector<Candidate> Report()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    std::vector<Candidate> out;
    out.reserve(g_scan.tracked.size());

    for (const Tracked& t : g_scan.tracked)
    {
        Candidate c;
        c.shape = t.shape;
        c.latest = t.latest;
        c.lowest = t.lowest;
        c.highest = t.highest;
        c.reads = t.reads;
        c.moves = t.moves;
        out.push_back(c);
    }

    return out;
}

const char* Status()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);
    return g_scan.status;
}

bool Scanning() { return Wanted(); }

} // namespace ExposureScan
} // namespace DlssNr
