#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;
struct D3D12_RESOURCE_DESC;
struct D3D12_UNORDERED_ACCESS_VIEW_DESC;

namespace DlssNr::ExposureScan
{
// Discovered values have no exposure-unit contract; calibrate them against user-selected white points.
struct Candidate
{
    std::string shape;
    float latest = 0.0f, lowest = 0.0f, highest = 0.0f;
    unsigned int reads = 0;
    bool moves = false;
};

// Resource/view creation discovers candidates before the user selects scanning.
void NoteUav(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc);
void NoteResource(const D3D12_RESOURCE_DESC* desc, ID3D12Resource* resource);

// Widest observed exposure ratio wins; returns zero if no candidate is usable.
float BestValue(int* outIndex = nullptr, float* outLowest = nullptr, float* outHighest = nullptr);
std::vector<Candidate> Report();
const char* Status();
bool Scanning();

// Follows the first rendering device until Shutdown. Epoch deduplicates calls; it is not GPU completion.
void Tick(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, uint64_t submissionEpoch = UINT64_MAX);

// Sorted snapshots; mutation and interpolation are serialized by the anchor mutex.
// See design/multi-point-anchoring.md for calibration behavior.
struct AnchorPoint { float scan, white; };
std::vector<AnchorPoint> Anchors();
bool AnchorAdd(float scan, float white); // Nearby scan values update an existing point.
void AnchorSetWhite(int index, float white);
void AnchorRemove(int index);

// One anchor uses a ratio; multiple anchors interpolate in log space. Zero means no usable anchor.
float AnchoredWhitePoint(float scanNow, bool inverted, float trim);
void LoadAnchors(const std::string& serialized); // "v0:w0;v1:w1;..."
std::string SerializeAnchors();

void ReleaseTrackedResources(); // Release foreign references before their owning heaps are destroyed.
void Shutdown(); // Call after scanner GPU copies finish, before destroying its device.
}
