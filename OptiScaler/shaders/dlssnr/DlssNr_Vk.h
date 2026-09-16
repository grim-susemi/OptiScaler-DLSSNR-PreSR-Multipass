#pragma once

// Vulkan NR owner and shared colour-codec dispatch.

#include "SysUtils.h"
#include <shaders/Shader_Vk.h>
#include "DlssNr_Common.h"
#include <memory>

namespace DlssNr
{
class ModelVk;
class FinishedVk;
}

struct DlssNrFrameInfo_Vk : DlssNrFrameInfo
{
    bool DepthReadWrite = false;
    bool MotionReadWrite = false;
    unsigned int ColorSubrectBaseX = 0;
    unsigned int ColorSubrectBaseY = 0;
};

class DlssNr_Vk : public Shader_Vk
{
    static constexpr uint32_t kSlotsPerFrame = 12;
    static constexpr uint32_t kFramesInFlight = 4;
    static constexpr uint32_t kSlots = kSlotsPerFrame * kFramesInFlight;

    std::unique_ptr<DlssNr::ModelVk> _model;
    std::unique_ptr<DlssNr::FinishedVk> _finished;
    VkPipeline _finishedPipeline = VK_NULL_HANDLE;
    VkImageLayout _intermediateLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkDeviceSize _slotStride = 0;   // sizeof(DlssNrConstants), rounded up to the device's alignment
    uint32_t _slot = 0;             // next slot to hand out, wrapping

    void WriteDescriptors(VkDescriptorSet set, VkDeviceSize constantOffset, VkImageView source, VkImageView model,
                          VkImageView original, VkImageView motion, VkImageView target, VkImageView keep,
                          VkImageLayout sourceLayout, VkImageLayout motionLayout);

  public:
    DlssNr_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);
    ~DlssNr_Vk();

    VkImageInfo PrepareInput(VkCommandBuffer cmd, const VkImageInfo& nextOutput);
    using Shader_Vk::SetImageLayout;
    bool Dispatch(VkCommandBuffer cmd, const VkImageInfo& colour, const VkImageInfo& depth, const VkImageInfo& motion,
                  const VkImageInfo& output, const DlssNrFrameInfo_Vk& frame, VkInstance instance,
                  VkImageLayout inputLayout = VK_IMAGE_LAYOUT_GENERAL, bool* modelRan = nullptr);
    void CaptureFinished(VkCommandBuffer cmd, const VkImageInfo& depth, const VkImageInfo& motion,
                         const DlssNrFrameInfo_Vk& frame, VkInstance instance);

    // The caller supplies layouts for borrowed images. Unused views receive stand-ins.
    // Reuse immutableSlot only for identical bindings/constants; initialize it to UINT32_MAX.
    bool Dispatch(VkCommandBuffer InCmdList, const DlssNrConstants& InConstants,
                  VkImageView InSource, VkImageView InModel, VkImageView InOriginal,
                  VkImageView InMotion, VkImageView InTarget, VkImageView InKeep,
                  VkImageLayout InSourceLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VkImageLayout InMotionLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, bool finishedColor = false,
                  uint32_t* immutableSlot = nullptr);
};
