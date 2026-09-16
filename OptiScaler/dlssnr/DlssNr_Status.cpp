#include <pch.h>
#include "DlssNr_Status.h"

#include <array>
#include <atomic>
#include <mutex>

namespace DlssNr
{
namespace
{
struct PublishedStatus
{
    const void* owner = nullptr; // Identity only; never dereferenced.
    StatusSnapshot value;
};

std::mutex statusMutex;
std::array<PublishedStatus, 2> published;
std::atomic_uint64_t retryGeneration { 0 };
} // namespace

StatusSnapshot ReadStatus(Backend backend)
{
    std::lock_guard lock(statusMutex);
    return published[static_cast<size_t>(backend)].value;
}

void PublishStatus(const void* owner, Backend backend, const StatusSnapshot& status)
{
    std::lock_guard lock(statusMutex);
    published[static_cast<size_t>(backend)] = { owner, status };
}

void ClearStatus(const void* owner)
{
    std::lock_guard lock(statusMutex);
    for (auto& status : published)
        if (status.owner == owner)
            status = {};
}

uint64_t RetryGeneration() { return retryGeneration.load(); }
void RetryAfterFailure() { ++retryGeneration; }

std::optional<double> LastGpuTime() { return ReadStatus(Backend::Dx12).gpuTime; }
} // namespace DlssNr
