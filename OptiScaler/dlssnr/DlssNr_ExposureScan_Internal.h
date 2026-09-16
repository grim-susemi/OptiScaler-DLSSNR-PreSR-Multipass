#pragma once
#include "DlssNr_ExposureScan.h"
#include <mutex>
namespace DlssNr::ExposureScan::Detail
{
// Keep enough candidates for buffer-heavy engines without unbounded resource tracking.
constexpr size_t kMaxCandidates = 64;

// Ring depth for the readbacks. Four, so the slot being read is four frames behind the slot being
// written and the read never waits on the GPU. Same depth and the same reason as the meter's.
constexpr unsigned int kSlots = 4;

// Each candidate has a 512-byte slot to satisfy D3D12 texture-placement alignment.
constexpr unsigned int kStride = 512;

// What an exposure could plausibly be. Outside these it is a flag, a counter, a sentinel or a
// zeroed buffer nobody has written yet -- Nioh 3 offers all four, including one holding 1000000.
constexpr float kFloor = 1e-6f;
constexpr float kCeiling = 1e4f;

struct Tracked
{
    ID3D12Resource* resource = nullptr;
    ID3D12Device* device = nullptr; // Identity; the retained resource keeps its device alive.
    std::string shape;
    bool isBuffer = false;
    unsigned int bytes = 4;
    DXGI_FORMAT texFormat = DXGI_FORMAT_UNKNOWN;  // the source texture's format, for CopyTextureRegion

    float latest = 0.0f;
    float lowest = 0.0f;
    float highest = 0.0f;
    unsigned int reads = 0;
    unsigned int inRange = 0;   // reads that could plausibly be an exposure
    bool moves = false;
};

struct ScanState
{
    ID3D12Device* device = nullptr;
    unsigned long long lastEpoch = UINT64_MAX;
    std::vector<Tracked> tracked;
    ID3D12Resource* readback[kSlots] = {};
    size_t readbackCounts[kSlots] = {};
    unsigned long long frames = 0;
    const char* status = "not started";
    bool complained = false;
    unsigned int nearMissLogged = 0;   // bounded diagnostic; see NoteResource
};

extern ScanState g_scan;
extern std::mutex g_scanMutex;
// Serializes ring allocation, recording and shutdown. Resource creation hooks only take
// g_scanMutex, so allocating readbacks while holding this mutex cannot reenter it.
extern std::mutex g_tickMutex;

bool Wanted();
}
