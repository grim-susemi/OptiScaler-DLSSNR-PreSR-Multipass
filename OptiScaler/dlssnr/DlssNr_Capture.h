#pragma once
#include "DlssNr_Readback.h"
#include "DlssNr_GpuLifetime.h"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace capture
{
constexpr unsigned int kMaxFrames = 8;

// Consecutive before/after images, with completion tracked for every recorded copy.
class FrameCapture
{
    struct Pair { DlssNr::ReadbackImage before, after; };
    struct Data
    {
        std::vector<Pair> pairs;
        DlssNr::CaptureTimestamps timestamps;
        unsigned recorded = 0;

        bool Init(ID3D12Device* device, ID3D12Resource* before, ID3D12Resource* after, unsigned count)
        {
            pairs.resize(count);
            for (auto& pair : pairs)
                if (!pair.before.Allocate(device, before->GetDesc()) || !pair.after.Allocate(device, after->GetDesc()))
                    return false;
            return timestamps.Init(device, count);
        }
    };
    std::unique_ptr<Data> data_;
    DlssNr::GpuLifetime lifetime_;
    unsigned wanted_ = 0;

    static bool Matches(const DlssNr::ReadbackImage& image, ID3D12Resource* source)
    {
        const auto desc = source->GetDesc();
        const auto& stored = image.layout.Footprint;
        return stored.Width == desc.Width && stored.Height == desc.Height &&
               stored.Format == DlssNr::TypedReadbackFormat(desc.Format);
    }

  public:
    ~FrameCapture() { release(); }
    void request(unsigned frames)
    {
        if (!isActive())
            wanted_ = std::min(frames, kMaxFrames);
    }
    bool isActive() const { return wanted_ != 0; }
    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
    { lifetime_.Submitted(queue, count, lists); }
    void ResetRecording(ID3D12CommandList* commands) { lifetime_.ResetRecording(commands); }

    void record(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* before,
                D3D12_RESOURCE_STATES beforeState, ID3D12Resource* after, D3D12_RESOURCE_STATES afterState)
    {
        if (!isActive() || (data_ && data_->recorded == wanted_))
            return;
        if (!data_)
        {
            auto next = std::make_unique<Data>();
            if (!next->Init(device, before, after, wanted_)) { release(); return; }
            data_ = std::move(next);
        }
        auto& pair = data_->pairs[data_->recorded];
        if (!Matches(pair.before, before) || !Matches(pair.after, after))
        {
            const auto retry = wanted_;
            release(); request(retry);
            return;
        }
        lifetime_.Record(cmd);
        pair.before.Copy(cmd, before, beforeState);
        pair.after.Copy(cmd, after, afterState);
        data_->timestamps.Record(cmd, data_->recorded);
        ++data_->recorded;
    }

    std::string write(const std::filesystem::path& directory)
    {
        if (!data_ || data_->recorded != wanted_ || !lifetime_.Idle())
            return {};
        if (!data_->timestamps.Completed(data_->recorded))
        {
            const auto retry = wanted_;
            release(); request(retry);
            return {};
        }
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        bool success = !error;
        for (unsigned i = 0; success && i < wanted_; ++i)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "before_%02u.raw", i);
            success = data_->pairs[i].before.Write(directory / name);
            std::snprintf(name, sizeof(name), "after_%02u.raw", i);
            success &= data_->pairs[i].after.Write(directory / name);
        }
        if (success)
        {
            std::ofstream manifest(directory / "manifest.txt");
            manifest << "frames " << wanted_ << '\n';
            const auto describe = [&](const char* name, const DlssNr::ReadbackImage& image)
            {
                const auto& f = image.layout.Footprint;
                manifest << name << " width " << f.Width << " height " << f.Height << " format " << int(f.Format)
                         << " rowPitch " << f.RowPitch << '\n';
            };
            describe("before", data_->pairs.front().before);
            describe("after", data_->pairs.front().after);
            manifest << "\nbefore_NN.raw is the frame as the upscaler produced it.\n"
                        "after_NN.raw is the same frame once the model's edit was applied.\n"
                        "Consecutive frames, same run, so the pair is a control.\n";
            manifest.close();
            success = !manifest.fail();
        }
        release();
        return success ? directory.string() : std::string {};
    }

    void release()
    {
        if (auto* retired = data_.release())
            lifetime_.Retire([retired] { delete retired; });
        lifetime_.BeginGeneration();
        wanted_ = 0;
    }
};
}
