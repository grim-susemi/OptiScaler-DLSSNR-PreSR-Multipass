#pragma once
#include "DlssNr_Readback.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace DlssNr
{
// Diagnostic readbacks only. The caller must retain this object with GpuLifetime
// until the recording is closed and every submission has completed.
struct PipelineCaptureFrame
{
    struct Image : ReadbackImage
    {
        std::string name;
    };
    std::vector<Image> images;
    std::ostringstream metadata;
    std::filesystem::path directory;
    CaptureTimestamps timestamps;
    UINT64 totalBytes = 0;
    bool ended = false;

    bool Init(ID3D12Device* device) { return timestamps.Init(device, 1); }
    void Copy(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const char* name,
              ID3D12Resource* resource, D3D12_RESOURCE_STATES state)
    {
        metadata << name << " resource " << resource << " state " << unsigned(state);
        if (!resource) { metadata << " missing\n"; return; }
        auto desc = resource->GetDesc();
        metadata << " width " << desc.Width << " height " << desc.Height << " format " << desc.Format;
        desc.Format = TypedReadbackFormat(desc.Format);
        // Deliberately narrow: reject planar/depth-stencil and unusual layouts.
        const bool format = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
            desc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT || desc.Format == DXGI_FORMAT_R32_FLOAT ||
            desc.Format == DXGI_FORMAT_R16_FLOAT || desc.Format == DXGI_FORMAT_R16G16_FLOAT ||
            desc.Format == DXGI_FORMAT_R32G32_FLOAT || desc.Format == DXGI_FORMAT_R11G11B10_FLOAT;
        if (!format || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.DepthOrArraySize != 1)
        { metadata << " skipped_unsupported\n"; return; }
        Image image; image.name = name;
        if (!image.Allocate(device, desc, 256ull * 1024 * 1024 - totalBytes))
        { metadata << " skipped_budget_or_allocation\n"; return; }
        totalBytes += image.bytes;
        metadata << " storedFormat " << desc.Format << " rowPitch " << image.layout.Footprint.RowPitch
                 << " bytes " << image.bytes << '\n';
        image.Copy(cmd, resource, state);
        images.push_back(std::move(image));
    }
    void End(ID3D12GraphicsCommandList* cmd)
    {
        timestamps.Record(cmd, 0);
        ended = true;
    }
    bool Write()
    {
        // A discarded, never-submitted recording must not produce uninitialized images.
        if (!ended) return false;
        const auto timestamp = timestamps.Completed(1);
        if (!timestamp) return false;
        std::error_code error; std::filesystem::create_directories(directory, error);
        if (error) return false;
        bool success = true;
        for (const auto& image : images)
            success &= image.Write(directory / (image.name + ".raw"));
        std::ofstream manifest(directory / "manifest.txt");
        manifest << "gpu_timestamp " << timestamp << "\n" << metadata.str();
        manifest.close();
        return success && !manifest.fail();
    }
};
}
