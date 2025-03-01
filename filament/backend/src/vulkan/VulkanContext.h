/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef TNT_FILAMENT_BACKEND_VULKANCONTEXT_H
#define TNT_FILAMENT_BACKEND_VULKANCONTEXT_H

#include "VulkanCommands.h"
#include "VulkanConstants.h"
#include "VulkanImageUtility.h"
#include "VulkanPipelineCache.h"

#include <utils/Mutex.h>
#include <utils/Slice.h>
#include <utils/bitset.h>

#include <memory>

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaPool)

namespace filament::backend {

struct VulkanRenderTarget;
struct VulkanSwapChain;
struct VulkanTexture;
class VulkanStagePool;
struct VulkanTimerQuery;

// TODO: We used std::shared_ptr in various places across the vulkan backend, but VulkanTexture*
// also maps to HwTexture so it's not possible to switch in VulkanAttachment. Hence we introduce
// this temporary class. We need to revisit the ownership pattern and use of smart pointer in the
// vulkan backend, in particular std::shared_ptr uses atomic, which is overhead we do not need.
struct TexturePointer {
    TexturePointer() = default;
    explicit TexturePointer(VulkanTexture* tex) :mTexture(tex) {}
    explicit TexturePointer(std::shared_ptr<VulkanTexture> tex) :mTexture(tex) {}

    inline TexturePointer& operator=(VulkanTexture* tex) {
        mTexture = tex;
        return *this;
    }

    inline TexturePointer& operator=(std::shared_ptr<VulkanTexture> tex) {
        mTexture = tex;
        return *this;
    }

    // Be careful not to leak here. Should only be used in local scope.
    explicit operator VulkanTexture*() const {
        if (mTexture.index() == 0) {
            return std::get<0>(mTexture);
        }
        return std::get<1>(mTexture).get();
    }

    // Be careful not to leak here. Should only be used in local scope.
    explicit operator VulkanTexture const*() const {
        if (mTexture.index() == 0) {
            return std::get<0>(mTexture);
        }
        return std::get<1>(mTexture).get();
    }

    explicit operator std::shared_ptr<VulkanTexture>() const {
        assert_invariant(mTexture.index() == 1);
        return std::get<1>(mTexture);
    }

    inline operator bool() const  {
        if (mTexture.index() == 0) {
            return std::get<0>(mTexture) != nullptr;
        }
        return (bool) std::get<1>(mTexture);
    }

    inline VulkanTexture* operator->() {
        if (mTexture.index() == 0) {
            return std::get<0>(mTexture);
        }
        return std::get<1>(mTexture).get();
    }

    inline VulkanTexture const* operator->() const {
        if (mTexture.index() == 0) {
            return std::get<0>(mTexture);
        }
        return std::get<1>(mTexture).get();
    }

private:
    std::variant<VulkanTexture*, std::shared_ptr<VulkanTexture>> mTexture;
};

struct VulkanAttachment {
    TexturePointer texture;
    uint8_t level = 0;
    uint16_t layer = 0;
    VkImage getImage() const;
    VkFormat getFormat() const;
    VulkanLayout getLayout() const;
    VkExtent2D getExtent2D() const;
    VkImageView getImageView(VkImageAspectFlags aspect);
    // TODO: maybe embed aspect into the attachment or texture itself.
    VkImageSubresourceRange getSubresourceRange(VkImageAspectFlags aspect) const;
};

class VulkanTimestamps {
public:
    using QueryResult = std::array<uint64_t, 4>;

    VulkanTimestamps(VkDevice device);
    ~VulkanTimestamps();

    // Not copy-able.
    VulkanTimestamps(VulkanTimestamps const&) = delete;
    VulkanTimestamps& operator=(VulkanTimestamps const&) = delete;

    std::tuple<uint32_t, uint32_t> getNextQuery();
    void clearQuery(uint32_t queryIndex);

    void beginQuery(VulkanCommandBuffer const* commands, VulkanTimerQuery* query);
    void endQuery(VulkanCommandBuffer const* commands, VulkanTimerQuery const* query);
    QueryResult getResult(VulkanTimerQuery const* query);

private:
    VkDevice mDevice;
    VkQueryPool mPool;
    utils::bitset32 mUsed;
    utils::Mutex mMutex;
};

struct VulkanRenderPass {
    VulkanRenderTarget* renderTarget;
    VkRenderPass renderPass;
    RenderPassParams params;
    int currentSubpass;
};

// This is a collection of immutable data about the vulkan context. This actual handles to the
// context are stored in VulkanPlatform.
struct VulkanContext {
public:
    inline uint32_t selectMemoryType(uint32_t flags, VkFlags reqs) const {
        for (uint32_t i = 0; i < VK_MAX_MEMORY_TYPES; i++) {
            if (flags & 1) {
                if ((mMemoryProperties.memoryTypes[i].propertyFlags & reqs) == reqs) {
                    return i;
                }
            }
            flags >>= 1;
        }
        ASSERT_POSTCONDITION(false, "Unable to find a memory type that meets requirements.");
        return (uint32_t) ~0ul;
    }

    inline VkFormat getDepthFormat() const {
        return mDepthFormat;
    }

    inline VkPhysicalDeviceLimits const& getPhysicalDeviceLimits() const noexcept {
        return mPhysicalDeviceProperties.limits;
    }

    inline uint32_t getPhysicalDeviceVendorId() const noexcept {
        return mPhysicalDeviceProperties.vendorID;
    }

    inline bool isImageCubeArraySupported() const noexcept {
        return mPhysicalDeviceFeatures.imageCubeArray;
    }

    inline bool isDebugMarkersSupported() const noexcept {
        return mDebugMarkersSupported;
    }
    inline bool isDebugUtilsSupported() const noexcept {
        return mDebugUtilsSupported;
    }
    inline bool isPortabilitySubsetSupported() const noexcept {
        return mPortabilitySubsetSupported;
    }
    inline bool isPortabilityEnumerationSupported() const noexcept {
        return mPortabilityEnumerationSupported;
    }

    inline bool isMaintenance1Supported() const noexcept {
        return mMaintenanceSupported[0];
    }
    inline bool isMaintenance2Supported() const noexcept {
        return mMaintenanceSupported[1];
    }
    inline bool isMaintenance3Supported() const noexcept {
        return mMaintenanceSupported[2];
    }

private:
    VkPhysicalDeviceMemoryProperties mMemoryProperties = {};
    VkPhysicalDeviceProperties mPhysicalDeviceProperties = {};
    VkPhysicalDeviceFeatures mPhysicalDeviceFeatures = {};
    bool mDebugMarkersSupported = false;
    bool mDebugUtilsSupported = false;
    bool mPortabilitySubsetSupported = false;
    bool mPortabilityEnumerationSupported = false;
    bool mMaintenanceSupported[3] = {};

    VkFormat mDepthFormat;

    // For convenience so that VulkanPlatform can initialize the private fields.
    friend class VulkanPlatform;
};

}// namespace filament::backend

#endif// TNT_FILAMENT_BACKEND_VULKANCONTEXT_H
