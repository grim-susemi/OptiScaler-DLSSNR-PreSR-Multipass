#include "pch.h"
#include "DlssNr_Dx12_State.h"

auto DlssNr_Dx12::State::ParkNrResource(ID3D12Resource*& resource) -> void
{
    if (!resource) return;
    auto* retired = resource;
    resource = nullptr;
    lifetime.Retire([retired] { retired->Release(); });
}

auto DlssNr_Dx12::State::CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width, unsigned int height) -> ID3D12Resource*
{
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT, 0, 0);
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0,
                                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    nullptr, IID_PPV_ARGS(&res));
    return res;
}

auto DlssNr_Dx12::State::TypedGuideFormat(DXGI_FORMAT f) -> DXGI_FORMAT
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return f;
    }
}

auto DlssNr_Dx12::State::CreateGuideClone(ID3D12Device* device, ID3D12Resource* source) -> ID3D12Resource*
{
    D3D12_RESOURCE_DESC desc = source->GetDesc();
    desc.Format = TypedGuideFormat(desc.Format);
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                    IID_PPV_ARGS(&res));
    return res;
}

auto DlssNr_Dx12::State::ReadableGuide(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                                  ID3D12Resource** clone) -> ID3D12Resource*
{
    const auto want = source->GetDesc();
    const auto format = TypedGuideFormat(want.Format);
    if (format == want.Format)
        return source;

    // Dynamic resolution changes the copy shape; retire the old clone before replacing it.
    if (*clone)
    {
        const auto have = (*clone)->GetDesc();
        if (have.Width != want.Width || have.Height != want.Height || have.Format != format)
            ParkNrResource(*clone);
    }
    if (!*clone)
        *clone = CreateGuideClone(device, source);
    if (!*clone)
        return nullptr;

    Barrier(cmdList, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(*clone, source);
    Barrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, *clone, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return *clone;
}

auto DlssNr_Dx12::State::GetResource(NVSDK_NGX_Parameter* params, const char* a, const char* b) -> ID3D12Resource*
{
    // Preserve typed-key precedence; DX11/Vulkan bridges can supply untyped resources.
    for (const char* name : { a, b })
    {
        ID3D12Resource* resource = nullptr;
        if (params->Get(name, &resource) == NVSDK_NGX_Result_Success && resource)
            return resource;
    }
    for (const char* name : { a, b })
    {
        void* resource = nullptr;
        if (params->Get(name, &resource) == NVSDK_NGX_Result_Success && resource)
            return static_cast<ID3D12Resource*>(resource);
    }
    return nullptr;
}

void DlssNr_Dx12::State::ReleaseSupersamplers()
{
    for (auto** scaler : { &nr.superUp, &nr.superDown })
        if (auto* retired = std::exchange(*scaler, nullptr))
            lifetime.Retire([retired] { delete retired; });
}

auto DlssNr_Dx12::State::ReleaseResources() -> void
{
    std::lock_guard<std::recursive_mutex> nrLock(mutex);
    ReleaseEnlarger();
    enlargementStatus.clear();
    deferredSr.ReleaseResources();

    lifetime.Collect();

    for (auto& model : nr.models)
        model.Release();
    modelRunning = false;

    for (auto** resource : { &nr.output, &nr.passScratch, &nr.passClamp, &nr.colorCopy, &nr.hdrCopy,
                             &nr.activeColor, &nr.colorSmall })
        ParkNrResource(*resource);

    ReleaseSupersamplers();

    ParkNrResource(nr.outputNative);

    ParkNrResource(nr.heldColor);

    ParkNrResource(nr.depthClone);

    ParkNrResource(nr.motionClone);

    captureFrames.release();
    if (auto* timer = gpuTime.release()) lifetime.Retire([timer] { delete timer; });
    lastGpuTime.reset();
}
