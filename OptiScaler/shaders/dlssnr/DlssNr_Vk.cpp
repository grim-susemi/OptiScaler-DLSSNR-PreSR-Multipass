#include "pch.h"

#include "DlssNr_Vk.h"
#include <dlssnr/DlssNrFeature_Vk.h>

#include "precompile/DlssNr_Shader_Vk.h"
#include "precompile/dlssnr_finished_color_Shader_Vk.h"
#include <dlssnr/DlssNrFinished_Vk.h>

#include <algorithm>
#include <array>
#include <cstring>

// Binding numbers match dlssnr_common.hlsli for both codec pipelines.
static constexpr VkDescriptorType kBindings[] = {
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_SAMPLER,
};

DlssNr_Vk::DlssNr_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice)
    : Shader_Vk(InName, InDevice, InPhysicalDevice)
{
    if (InDevice == VK_NULL_HANDLE || InPhysicalDevice == VK_NULL_HANDLE)
    {
        LOG_ERROR("DLSS-NR Vulkan pass: no device");
        return;
    }

    _maxFramesInFlight = kFramesInFlight;

    // Linear, because the resolve reads the model's answer at a different size than it writes -- the
    // model may have run at a reduced resolution and the edit has to be stretched back over the frame.
    // The D3D12 pass uses a linear sampler for the same reason and the two must agree.
    CreateSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    // The constant ring. A uniform buffer binding can be offset into, but only to a multiple of the
    // device's own alignment, so the stride is the struct rounded up rather than the struct itself.
    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(_physicalDevice, &props);

    const VkDeviceSize alignment = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 1);
    _slotStride = ((sizeof(DlssNrConstants) + alignment - 1) / alignment) * alignment;

    if (!CreateBufferResource(_device, _physicalDevice, &_constantBuffer, &_constantBufferMemory,
                              _slotStride * kSlots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    {
        LOG_ERROR("DLSS-NR Vulkan pass: could not allocate the constant ring");
        return;
    }

    if (vkMapMemory(_device, _constantBufferMemory, 0, _slotStride * kSlots, 0, &_mappedConstantBuffer) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan pass: could not map the constant ring");
        return;
    }

    // The layout mirrors the [[vk::binding]] numbers in dlssnr.hlsl, entry for entry. Combined image
    // samplers for the reads: the shader declares its sampler separately, and a combined descriptor
    // satisfies a separately declared sampled image with the sampler half simply unused.
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t binding = 0; binding < std::size(kBindings); ++binding)
        bindings.push_back(CreateBinding(binding, kBindings[binding]));
    CreateLayouts(bindings);

    std::vector<VkDescriptorPoolSize> poolSizes = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlots },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * kSlots },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kSlots },
        { VK_DESCRIPTOR_TYPE_SAMPLER, kSlots },
    };

    CreateDescriptorPool(poolSizes, kSlots);

    // One set per slot rather than per frame: two dispatches in the same frame need two sets, or the
    // second overwrites bindings the first has not consumed yet.
    _maxFramesInFlight = kSlots;
    CreateDescriptorSets(_descriptorSetLayout, _descriptorPool, _descriptorSets);
    _maxFramesInFlight = kFramesInFlight;

    std::vector<char> shaderCode(dlssnr_spv, dlssnr_spv + sizeof(dlssnr_spv));

    if (!CreateComputePipeline(_device, _pipelineLayout, &_pipeline, shaderCode))
    {
        LOG_ERROR("DLSS-NR Vulkan pass: could not create the compute pipeline");
        return;
    }

    std::vector<char> finishedCode(dlssnr_finished_color_spv, dlssnr_finished_color_spv + sizeof(dlssnr_finished_color_spv));
    CreateComputePipeline(_device, _pipelineLayout, &_finishedPipeline, finishedCode);
    _init = true;
    LOG_INFO("DLSS-NR Vulkan pass up: {} constant slots, stride {}", kSlots, (uint64_t) _slotStride);
}

DlssNr_Vk::~DlssNr_Vk()
{
    _finished.reset();
    _model.reset();
    if (_finishedPipeline) vkDestroyPipeline(_device, _finishedPipeline, nullptr);
}

void DlssNr_Vk::WriteDescriptors(VkDescriptorSet set, VkDeviceSize constantOffset, VkImageView source,
                                 VkImageView model, VkImageView original, VkImageView motion, VkImageView target,
                                 VkImageView keep, VkImageLayout sourceLayout, VkImageLayout motionLayout)
{
    VkDescriptorBufferInfo bufferInfo { _constantBuffer, constantOffset, sizeof(DlssNrConstants) };

    // Unread bindings use the valid source/target, as on DX12; no placeholder image is needed.
    const auto readInfo = [&](VkImageView v, VkImageLayout layout)
    {
        return VkDescriptorImageInfo { _textureSampler, v ? v : source, v ? layout : sourceLayout };
    };

    const VkDescriptorImageInfo images[] = {
        readInfo(source, sourceLayout),
        readInfo(model, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        readInfo(original, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        readInfo(motion, motionLayout),
        { VK_NULL_HANDLE, target, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, keep ? keep : target, VK_IMAGE_LAYOUT_GENERAL },
        { _textureSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED },
    };

    std::array<VkWriteDescriptorSet, std::size(kBindings)> writes;
    for (uint32_t binding = 0; binding < writes.size(); ++binding)
        writes[binding] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1, kBindings[binding],
                            binding ? &images[binding - 1] : nullptr, binding ? nullptr : &bufferInfo, nullptr };
    vkUpdateDescriptorSets(_device, (uint32_t) writes.size(), writes.data(), 0, nullptr);
}

bool DlssNr_Vk::Dispatch(VkCommandBuffer InCmdList, const DlssNrConstants& InConstants,
                         VkImageView InSource, VkImageView InModel, VkImageView InOriginal,
                         VkImageView InMotion, VkImageView InTarget, VkImageView InKeep, VkImageLayout InSourceLayout,
                         VkImageLayout InMotionLayout, bool finishedColor, uint32_t* immutableSlot)
{
    if (!CanRender() || InCmdList == VK_NULL_HANDLE || (finishedColor && !_finishedPipeline))
        return false;

    if (!InSource || !InTarget)
        return false;

    const bool reuse = immutableSlot && *immutableSlot != UINT32_MAX;
    const uint32_t slot = reuse ? *immutableSlot : _slot;
    if (!reuse)
    {
        _slot = (_slot + 1) % kSlots;
        const VkDeviceSize offset = _slotStride * slot;
        std::memcpy((char*) _mappedConstantBuffer + offset, &InConstants, sizeof(DlssNrConstants));

        WriteDescriptors(_descriptorSets[slot], offset, InSource, InModel, InOriginal, InMotion, InTarget, InKeep,
                         InSourceLayout, InMotionLayout);
        if (immutableSlot)
            *immutableSlot = slot;
    }

    vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, finishedColor ? _finishedPipeline : _pipeline);
    vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &_descriptorSets[slot], 0,
                            nullptr);

    // The shader's thread group is 8x8, the same as the D3D12 path.
    const uint32_t groupsX = (InConstants.Width + 7) / 8;
    const uint32_t groupsY = (InConstants.Height + 7) / 8;

    vkCmdDispatch(InCmdList, groupsX, groupsY, 1);

    // What this dispatch wrote, the next one reads. Left to the caller and it is a race that shows up
    // as a frame of stale detail rather than as an error, which is the worst kind to chase.
    VkMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    vkCmdPipelineBarrier(InCmdList, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);

    return true;
}

VkImageInfo DlssNr_Vk::PrepareInput(VkCommandBuffer cmd, const VkImageInfo& nextOutput)
{
    if (!nextOutput.Image || !nextOutput.Width || !nextOutput.Height)
        return {};
    const bool resized = nextOutput.Width != _width || nextOutput.Height != _height || nextOutput.Format != _format;
    if (resized && GetImage() != VK_NULL_HANDLE && vkDeviceWaitIdle(_device) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan: device did not retire the previous intermediate");
        return {};
    }
    if (!CreateImageResource(nextOutput.Width, nextOutput.Height, nextOutput.Format,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return {};
    if (resized)
        _intermediateLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageInfo image { GetImageView(),    GetImage(),       { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
                        nextOutput.Format, nextOutput.Width, nextOutput.Height };
    SetImageLayout(cmd, image.Image, _intermediateLayout, VK_IMAGE_LAYOUT_GENERAL, image.SubresourceRange);
    _intermediateLayout = VK_IMAGE_LAYOUT_GENERAL;
    return image;
}

bool DlssNr_Vk::Dispatch(VkCommandBuffer cmd, const VkImageInfo& colour, const VkImageInfo& depth,
                         const VkImageInfo& motion, const VkImageInfo& output, const DlssNrFrameInfo_Vk& frame,
                         VkInstance instance, VkImageLayout inputLayout, bool* modelRan)
{
    if (!CanRender() || !colour.Image || !output.Image || colour.Image == output.Image)
        return false;

    // Always produce the unmodified frame first. A missing model, retryable failure, or missing
    // guides must leave the downstream pipeline a valid image. Compute copy needs only SAMPLED
    // usage on the game's input, unlike a transfer copy.
    SetImageLayout(cmd, colour.Image, inputLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, colour.SubresourceRange);
    DlssNrConstants copy {};
    copy.Mode = DlssNrMode_Downsample;
    copy.Width = output.Width;
    copy.Height = output.Height;
    const bool copied = Dispatch(cmd, copy, colour.ImageView, VK_NULL_HANDLE,
                                 VK_NULL_HANDLE, VK_NULL_HANDLE, output.ImageView, VK_NULL_HANDLE);
    SetImageLayout(cmd, colour.Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, inputLayout, colour.SubresourceRange);
    if (!copied)
        return false;
    if (!_model)
        _model = std::make_unique<DlssNr::ModelVk>(*this);
    const bool ran = _model->Evaluate(cmd, colour, depth, motion, output, frame, instance, _physicalDevice, _device, inputLayout);
    if (modelRan) *modelRan = ran;
    return true;
}

void DlssNr_Vk::CaptureFinished(VkCommandBuffer cmd, const VkImageInfo& depth, const VkImageInfo& motion,
                                const DlssNrFrameInfo_Vk& frame, VkInstance instance)
{
    if (!CanRender()) return;
    if (!_finished) _finished = std::make_unique<DlssNr::FinishedVk>(*this, _device, _physicalDevice);
    _finished->Capture(cmd, depth, motion, frame, instance);
}
