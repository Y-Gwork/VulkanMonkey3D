#pragma once
#include "../VulkanContext/VulkanContext.h"

namespace vm {
	struct Buffer
	{
		vk::Buffer buffer;
		vk::DeviceMemory memory;
		vk::DeviceSize size = 0;
		std::string name;
		void *data = nullptr;

		void createBuffer(vk::DeviceSize size, const vk::BufferUsageFlags& usage, const vk::MemoryPropertyFlags& properties);
		void map(vk::DeviceSize offset = 0);
		void unmap();
		void zero();
		void copyData(const void* srcData, vk::DeviceSize srcSize = 0);
		void copyBuffer(vk::Buffer srcBuffer, vk::DeviceSize size) const;
		void flush();
		void destroy();
	};
}