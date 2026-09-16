#include "pch.h"

#include "DlssNrFeature_Vk_Internal.h"

namespace DlssNr
{

bool ModelVk::Impl::CreateImage(ImageVk& img, uint32_t width, uint32_t height, VkFormat format)
{
    // Model/profile changes deliberately start with fresh storage after retiring previous work.
    img.Destroy(state.device);
    return img.Ensure(state.device, state.physicalDevice, width, height, format);
}

bool ModelVk::Impl::CreateMeterReadback()
{
    for (unsigned long long i = 0; i < kMeterSlots; ++i)
    {
        if (state.meterReadback[i] != VK_NULL_HANDLE)
            continue;

        VkBufferCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = kMeterBytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(state.device, &info, nullptr, &state.meterReadback[i]) != VK_SUCCESS)
        {
            LOG_WARN("DLSS-NR Vulkan: could not create the exposure readback buffer");
            return false;
        }

        VkMemoryRequirements req {};
        vkGetBufferMemoryRequirements(state.device, state.meterReadback[i], &req);

        VkMemoryAllocateInfo alloc {};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = FindVkMemoryType(state.physicalDevice, req.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (alloc.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(state.device, &alloc, nullptr, &state.meterReadbackMemory[i]) != VK_SUCCESS ||
            vkBindBufferMemory(state.device, state.meterReadback[i], state.meterReadbackMemory[i], 0) !=
                VK_SUCCESS ||
            vkMapMemory(state.device, state.meterReadbackMemory[i], 0, kMeterBytes, 0, &state.meterMapped[i]) !=
                VK_SUCCESS)
        {
            LOG_WARN("DLSS-NR Vulkan: could not back the exposure readback buffer");
            return false;
        }
    }

    return true;
}

void ModelVk::Impl::DestroyMeterReadback()
{
    for (unsigned long long i = 0; i < kMeterSlots; ++i)
    {
        if (state.meterReadbackMemory[i] != VK_NULL_HANDLE)
        {
            if (state.meterMapped[i] != nullptr)
                vkUnmapMemory(state.device, state.meterReadbackMemory[i]);

            vkFreeMemory(state.device, state.meterReadbackMemory[i], nullptr);
        }

        if (state.meterReadback[i] != VK_NULL_HANDLE)
            vkDestroyBuffer(state.device, state.meterReadback[i], nullptr);

        state.meterMapped[i] = nullptr;
        state.meterReadbackMemory[i] = VK_NULL_HANDLE;
        state.meterReadback[i] = VK_NULL_HANDLE;
    }

    state.meterFrames = 0;
}

void ModelVk::Impl::Transition(VkCommandBuffer cmd, ImageVk& img, VkImageLayout to)
{
    if (img.info.Image == VK_NULL_HANDLE || img.layout == to)
        return;

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = img.layout;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img.info.Image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    const auto access = [](VkImageLayout layout) -> VkAccessFlags
    {
        switch (layout)
        {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            return 0;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return VK_ACCESS_TRANSFER_READ_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return VK_ACCESS_TRANSFER_WRITE_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return VK_ACCESS_SHADER_READ_BIT;
        default:
            return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        }
    };
    barrier.srcAccessMask = access(img.layout);
    barrier.dstAccessMask = access(to);

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

    img.layout = to;
}

void ModelVk::Impl::TransitionForeign(VkCommandBuffer cmd, VkImage image, VkImageSubresourceRange range, VkImageLayout from,
                       VkImageLayout to)
{
    if (image == VK_NULL_HANDLE || from == to)
        return;

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
}

} // namespace DlssNr
