#pragma once

#include <cstdint>
#include <optional>
#include <string>

class Config;

namespace DlssNr
{
enum class Backend
{
    Dx12,
    Vulkan
};

// Menu telemetry contains values only. GPU resources remain owned by the shader instance.
struct StatusSnapshot
{
    bool running = false;
    std::string failureReason;
    std::optional<double> gpuTime;
    unsigned long long frames = 0;
};

void PublishStatus(const void* owner, Backend backend, const StatusSnapshot& status);
void ClearStatus(const void* owner);
StatusSnapshot ReadStatus(Backend backend);
uint64_t RetryGeneration();

void RenderMenu(::Config* config, float menuResScale);
void RetryAfterFailure();
std::optional<double> LastGpuTime();
} // namespace DlssNr
