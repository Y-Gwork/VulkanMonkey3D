#pragma once
#include "../Core/Base.h"
#include <vector>

namespace vk
{
	class Framebuffer;
	class ImageView;
}

namespace vm
{
	class RenderPass;

	class Framebuffer : public Ref_t<vk::Framebuffer>
	{
	public:
		Framebuffer() = default;
		~Framebuffer() = default;

		void Create(uint32_t width, uint32_t height, const vk::ImageView& view, const RenderPass& renderPass);
		void Create(uint32_t width, uint32_t height, const std::vector<vk::ImageView>& views, const RenderPass& renderPass);
		void Destroy();
	};
}