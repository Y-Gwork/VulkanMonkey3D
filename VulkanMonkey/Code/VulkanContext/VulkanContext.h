#pragma once

#include "../../include/Vulkan.h"
#include "../../include/SDL.h"
#include <fstream>
#include <vector>
#include <string>
#include <atomic>

#define WIDTH VulkanContext::get().surface->actualExtent.width
#define HEIGHT VulkanContext::get().surface->actualExtent.height
#define WIDTH_f static_cast<float>(WIDTH)
#define HEIGHT_f static_cast<float>(HEIGHT)

namespace vm {

	struct Image;
	struct Surface;
	struct Swapchain;

	struct VulkanContext {
		SDL_Window* window = nullptr;
		vk::Instance instance;
		Surface* surface = nullptr;
		int graphicsFamilyId{}, computeFamilyId{}, transferFamilyId{};
		vk::PhysicalDevice gpu;
		vk::PhysicalDeviceProperties gpuProperties;
		vk::PhysicalDeviceFeatures gpuFeatures;
		std::vector<vk::QueueFamilyProperties> queueFamilyProperties{};
		vk::Device device;
		vk::Queue graphicsQueue, computeQueue, transferQueue;
		vk::CommandPool commandPool;
		vk::CommandPool commandPool2;
		vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e4;
		Swapchain* swapchain = nullptr;
		Image* depth = nullptr;
		vk::CommandBuffer dynamicCmdBuffer;
		std::vector<vk::CommandBuffer> shadowCmdBuffer{};
		vk::DescriptorPool descriptorPool;
		std::vector<vk::Fence> fences{};
		std::vector<vk::Semaphore> semaphores{};

		// Helper
		static inline std::atomic_bool submiting = false;

		static VulkanContext& get() noexcept { static VulkanContext VkCTX; return VkCTX; }
		static const VulkanContext& getSafe() noexcept { return get(); }
	private:
		VulkanContext() {};									// default constructor
		VulkanContext(VulkanContext const&) {};				// copy constructor
		VulkanContext operator=(VulkanContext const&) {};	// copy assignment
		VulkanContext(VulkanContext&&) {};					// move constructor
		VulkanContext operator=(VulkanContext&&) {};		// move assignment
		~VulkanContext() {};								// destructor
	};

	static std::vector<char> readFile(const std::string& filename)
	{
		std::ifstream file(filename, std::ios::ate | std::ios::binary);
		if (!file.is_open()) {
			throw std::runtime_error("failed to open file!");
		}
		const size_t fileSize = (size_t)file.tellg();
		std::vector<char> buffer(fileSize);
		file.seekg(0);
		file.read(buffer.data(), fileSize);
		file.close();

		return buffer;
	}
}