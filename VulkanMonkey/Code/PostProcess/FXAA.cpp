#include "vulkanPCH.h"
#include "FXAA.h"
#include "../GUI/GUI.h"
#include "../Swapchain/Swapchain.h"
#include "../Core/Surface.h"
#include"../Shader/Shader.h"
#include "../VulkanContext/VulkanContext.h"

namespace vm
{
	FXAA::FXAA()
	{
		DSet = make_ref(vk::DescriptorSet());
	}

	FXAA::~FXAA()
	{
	}

	void FXAA::Init()
	{
		frameImage.format = make_ref(VulkanContext::get()->surface.formatKHR->format);
		frameImage.initialLayout = make_ref(vk::ImageLayout::eUndefined);
		frameImage.createImage(
			static_cast<uint32_t>(WIDTH_f * GUI::renderTargetsScale),
			static_cast<uint32_t>(HEIGHT_f * GUI::renderTargetsScale),
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
			vk::MemoryPropertyFlagBits::eDeviceLocal);
		frameImage.transitionImageLayout(vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal);
		frameImage.createImageView(vk::ImageAspectFlagBits::eColor);
		frameImage.createSampler();
	}

	void FXAA::createUniforms(std::map<std::string, Image>& renderTargets)
	{
		vk::DescriptorSetAllocateInfo allocateInfo2;
		allocateInfo2.descriptorPool = *VulkanContext::get()->descriptorPool;
		allocateInfo2.descriptorSetCount = 1;
		allocateInfo2.pSetLayouts = &Pipeline::getDescriptorSetLayoutFXAA();
		DSet = make_ref(VulkanContext::get()->device->allocateDescriptorSets(allocateInfo2).at(0));

		updateDescriptorSets(renderTargets);
	}

	void FXAA::updateDescriptorSets(std::map<std::string, Image>& renderTargets) const
	{
		// Composition sampler
		vk::DescriptorImageInfo dii;
		dii.sampler = *frameImage.sampler;
		dii.imageView = *frameImage.view;
		dii.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

		vk::WriteDescriptorSet textureWriteSet;
		textureWriteSet.dstSet = *DSet;
		textureWriteSet.dstBinding = 0;
		textureWriteSet.dstArrayElement = 0;
		textureWriteSet.descriptorCount = 1;
		textureWriteSet.descriptorType = vk::DescriptorType::eCombinedImageSampler;
		textureWriteSet.pImageInfo = &dii;

		VulkanContext::get()->device->updateDescriptorSets(textureWriteSet, nullptr);
	}

	void FXAA::draw(vk::CommandBuffer cmd, uint32_t imageIndex, const vk::Extent2D& extent)
	{
		vk::ClearValue clearColor;
		memcpy(clearColor.color.float32, GUI::clearColor.data(), 4 * sizeof(float));

		std::vector<vk::ClearValue> clearValues = { clearColor };

		vk::RenderPassBeginInfo rpi;
		rpi.renderPass = *renderPass.handle;
		rpi.framebuffer = *framebuffers[imageIndex].handle;
		rpi.renderArea.offset = vk::Offset2D{ 0, 0 };
		rpi.renderArea.extent = extent;
		rpi.clearValueCount = 1;
		rpi.pClearValues = clearValues.data();

		cmd.beginRenderPass(rpi, vk::SubpassContents::eInline);
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline.handle);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipeline.layout, 0, *DSet, nullptr);
		cmd.draw(3, 1, 0, 0);
		cmd.endRenderPass();
	}

	void vm::FXAA::createRenderPass(std::map<std::string, Image>& renderTargets)
	{
		renderPass.Create(*renderTargets["viewport"].format, vk::Format::eUndefined);
	}

	void vm::FXAA::createFrameBuffers(std::map<std::string, Image>& renderTargets)
	{
		auto vulkan = VulkanContext::get();
		framebuffers.resize(vulkan->swapchain.images.size());
		for (size_t i = 0; i < vulkan->swapchain.images.size(); ++i)
		{
			uint32_t width = renderTargets["viewport"].width;
			uint32_t height = renderTargets["viewport"].height;
			vk::ImageView view = *renderTargets["viewport"].view;
			framebuffers[i].Create(width, height, view, renderPass);
		}
	}

	void FXAA::createPipeline(std::map<std::string, Image>& renderTargets)
	{
		Shader vert{ "shaders/Common/quad.vert", ShaderType::Vertex, true };
		Shader frag{ "shaders/FXAA/FXAA.frag", ShaderType::Fragment, true };

		pipeline.info.pVertShader = &vert;
		pipeline.info.pFragShader = &frag;
		pipeline.info.width = renderTargets["viewport"].width_f;
		pipeline.info.height = renderTargets["viewport"].height_f;
		pipeline.info.cullMode = CullMode::Back;
		pipeline.info.colorBlendAttachments = make_ref(std::vector<vk::PipelineColorBlendAttachmentState>{ *renderTargets["viewport"].blentAttachment });
		pipeline.info.descriptorSetLayouts = make_ref(std::vector<vk::DescriptorSetLayout>{ Pipeline::getDescriptorSetLayoutDOF() });
		pipeline.info.renderPass = renderPass;

		pipeline.createGraphicsPipeline();
	}

	void FXAA::copyFrameImage(const vk::CommandBuffer& cmd, Image& renderedImage) const
	{
		frameImage.copyColorAttachment(cmd, renderedImage);
	}

	void FXAA::destroy()
	{
		for (auto& frameBuffer : framebuffers)
			frameBuffer.Destroy();

		renderPass.Destroy();

		if (Pipeline::getDescriptorSetLayoutFXAA()) {
			VulkanContext::get()->device->destroyDescriptorSetLayout(Pipeline::getDescriptorSetLayoutFXAA());
			Pipeline::getDescriptorSetLayoutFXAA() = nullptr;
		}
		frameImage.destroy();
		pipeline.destroy();
	}
}
