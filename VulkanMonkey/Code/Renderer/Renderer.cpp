#include "Renderer.h"
#include "../Event/Event.h"
#include "../Core/Queue.h"
#include "../Model/Mesh.h"

namespace vm
{
	Renderer::Renderer(SDL_Window* window)
	{
		VulkanContext::get()->window = window;
		// INIT ALL VULKAN CONTEXT
		ctx.initVulkanContext();
		// INIT RENDERING
		ctx.initRendering();
		//LOAD RESOURCES
		ctx.loadResources();
		// CREATE UNIFORMS AND DESCRIPTOR SETS
		ctx.createUniforms();
	}

	Renderer::~Renderer()
	{
		VulkanContext::get()->device.waitIdle();
		if (Model::models.empty()) {
			if (Model::descriptorSetLayout) {
				VulkanContext::get()->device.destroyDescriptorSetLayout(Model::descriptorSetLayout);
				Model::descriptorSetLayout = nullptr;
			}
			if (Mesh::descriptorSetLayout) {
				VulkanContext::get()->device.destroyDescriptorSetLayout(Mesh::descriptorSetLayout);
				Mesh::descriptorSetLayout = nullptr;
			}
			if (Primitive::descriptorSetLayout) {
				VulkanContext::get()->device.destroyDescriptorSetLayout(Primitive::descriptorSetLayout);
				Primitive::descriptorSetLayout = nullptr;
			}
		}
#ifdef USE_SCRIPTS
		for (auto& script : ctx.scripts)
			delete script;
#endif
		for (auto& model : Model::models)
			model.destroy();
		for (auto& texture : Mesh::uniqueTextures)
			texture.second.destroy();
		Mesh::uniqueTextures.clear();

		ComputePool::get()->destroy();
		ComputePool::remove();
		ctx.shadows.destroy();
		ctx.deferred.destroy();
		ctx.ssao.destroy();
		ctx.ssr.destroy();
		ctx.fxaa.destroy();
		ctx.taa.destroy();
		ctx.bloom.destroy();
		ctx.dof.destroy();
		ctx.motionBlur.destroy();
		ctx.skyBoxDay.destroy();
		ctx.skyBoxNight.destroy();
		ctx.gui.destroy();
		ctx.lightUniforms.destroy();
		for (auto& metric : ctx.metrics)
			metric.destroy();
		ctx.destroyVkContext();
		VulkanContext::remove();
	}

	void Renderer::changeLayout(vk::CommandBuffer cmd, Image& image, LayoutState state)
	{
		if (state != image.layoutState.Value()) {
			if (state == LayoutState::ColorRead) {
				image.transitionImageLayout(
					cmd,
					vk::ImageLayout::eColorAttachmentOptimal,
					vk::ImageLayout::eShaderReadOnlyOptimal,
					vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::PipelineStageFlagBits::eFragmentShader,
					vk::AccessFlagBits::eColorAttachmentWrite,
					vk::AccessFlagBits::eShaderRead,
					vk::ImageAspectFlagBits::eColor
				);
			}
			else if (state == LayoutState::ColorWrite) {
				image.transitionImageLayout(
					cmd,
					vk::ImageLayout::eShaderReadOnlyOptimal,
					vk::ImageLayout::eColorAttachmentOptimal,
					vk::PipelineStageFlagBits::eFragmentShader,
					vk::PipelineStageFlagBits::eColorAttachmentOutput,
					vk::AccessFlagBits::eShaderRead,
					vk::AccessFlagBits::eColorAttachmentWrite,
					vk::ImageAspectFlagBits::eColor
				);
			}
			else if (state == LayoutState::DepthRead) {
				image.transitionImageLayout(
					cmd,
					vk::ImageLayout::eDepthStencilAttachmentOptimal,
					vk::ImageLayout::eDepthStencilReadOnlyOptimal,
					vk::PipelineStageFlagBits::eEarlyFragmentTests,
					vk::PipelineStageFlagBits::eFragmentShader,
					vk::AccessFlagBits::eDepthStencilAttachmentWrite,
					vk::AccessFlagBits::eShaderRead,
					vk::ImageAspectFlagBits::eDepth);
			}
			else if (state == LayoutState::DepthWrite) {
				image.transitionImageLayout(
					cmd,
					vk::ImageLayout::eDepthStencilReadOnlyOptimal,
					vk::ImageLayout::eDepthStencilAttachmentOptimal,
					vk::PipelineStageFlagBits::eFragmentShader,
					vk::PipelineStageFlagBits::eEarlyFragmentTests,
					vk::AccessFlagBits::eShaderRead,
					vk::AccessFlagBits::eDepthStencilAttachmentWrite,
					vk::ImageAspectFlagBits::eDepth);
			}
			image.layoutState = state;
		}
	}

	void Renderer::checkQueue() const
	{
		for (auto it = Queue::loadModel.begin(); it != Queue::loadModel.end();) {
			VulkanContext::get()->device.waitIdle();
			Queue::loadModelFutures.push_back(std::async(std::launch::async, [](const std::string& folderPath, const std::string& modelName, bool show = true) {
				Model model;
				model.loadModel(folderPath, modelName, show);
				for (auto& _model : Model::models)
					if (_model.name == model.name)
						model.name = "_" + model.name;
				return std::any(std::move(model));
				}, std::get<0>(*it), std::get<1>(*it), true));
			it = Queue::loadModel.erase(it);
		}

		for (auto it = Queue::loadModelFutures.begin(); it != Queue::loadModelFutures.end();) {
			if (it->wait_for(std::chrono::seconds(0)) != std::future_status::timeout) {
				Model::models.push_back(std::any_cast<Model>(it->get()));
				GUI::modelList.push_back(Model::models.back().name);
				GUI::model_scale.push_back({ 1.f, 1.f, 1.f });
				GUI::model_pos.push_back({ 0.f, 0.f, 0.f });
				GUI::model_rot.push_back({ 0.f, 0.f, 0.f });
				it = Queue::loadModelFutures.erase(it);
			}
			else {
				++it;
			}
		}

		for (auto it = Queue::unloadModel.begin(); it != Queue::unloadModel.end();) {
			VulkanContext::get()->device.waitIdle();
			Model::models[*it].destroy();
			Model::models.erase(Model::models.begin() + *it);
			GUI::modelList.erase(GUI::modelList.begin() + *it);
			GUI::model_scale.erase(GUI::model_scale.begin() + *it);
			GUI::model_pos.erase(GUI::model_pos.begin() + *it);
			GUI::model_rot.erase(GUI::model_rot.begin() + *it);
			GUI::modelItemSelected = -1;
			it = Queue::unloadModel.erase(it);
		}
#ifdef USE_SCRIPTS
		for (auto it = Queue::addScript.begin(); it != Queue::addScript.end();) {
			delete Model::models[std::get<0>(*it)].script;
			Model::models[std::get<0>(*it)].script = new Script(std::get<1>(*it).c_str());
			it = Queue::addScript.erase(it);
		}
		for (auto it = Queue::removeScript.begin(); it != Queue::removeScript.end();) {
			if (Model::models[*it].script) {
				delete Model::models[*it].script;
				Model::models[*it].script = nullptr;
			}
			it = Queue::removeScript.erase(it);
		}

		for (auto it = Queue::compileScript.begin(); it != Queue::compileScript.end();) {
			std::string name;
			if (Model::models[*it].script) {
				name = Model::models[*it].script->name;
				delete Model::models[*it].script;
				Model::models[*it].script = new Script(name.c_str());
			}
			it = Queue::compileScript.erase(it);
		}

#endif
	}

	void Renderer::update(double delta)
	{
		static Timer timer;
		timer.Start();

		// check for commands in queue
		checkQueue();

#ifdef USE_SCRIPTS
		// universal scripts
		for (auto& s : ctx.scripts)
			s->update(static_cast<float>(delta));
#endif

		// update camera matrices
		ctx.camera_main.update();

		// Model updates + 8(the rest updates)
		std::vector<std::future<void>> futureUpdates;
		futureUpdates.reserve(Model::models.size() + 8);

		// MODELS
		if (GUI::modelItemSelected > -1) {
			Model::models[GUI::modelItemSelected].scale = vec3(GUI::model_scale[GUI::modelItemSelected].data());
			Model::models[GUI::modelItemSelected].pos = vec3(GUI::model_pos[GUI::modelItemSelected].data());
			Model::models[GUI::modelItemSelected].rot = vec3(GUI::model_rot[GUI::modelItemSelected].data());
		}
		for (auto& model : Model::models)
		{
			const auto updateModel = [&]() { model.update(ctx.camera_main, delta); };
			futureUpdates.push_back(std::async(std::launch::async, updateModel));
		}

		// GUI
		auto updateGUI = [&]() { ctx.gui.update(); };
		futureUpdates.push_back(std::async(std::launch::async, updateGUI));

		// LIGHTS
		auto updateLights = [&]() { ctx.lightUniforms.update(ctx.camera_main); };
		futureUpdates.push_back(std::async(std::launch::async, updateLights));

		// SSAO
		auto updateSSAO = [&]() { ctx.ssao.update(ctx.camera_main); };
		futureUpdates.push_back(std::async(std::launch::async, updateSSAO));

		// SSR
		auto updateSSR = [&]() { ctx.ssr.update(ctx.camera_main); };
		futureUpdates.push_back(std::async(std::launch::async, updateSSR));

		// TAA
		auto updateTAA = [&]() { ctx.taa.update(ctx.camera_main); };
		futureUpdates.push_back(std::async(std::launch::async, updateTAA));

		// MOTION BLUR
		auto updateMotionBlur = [&]() { ctx.motionBlur.update(ctx.camera_main); };
		futureUpdates.push_back(std::async(std::launch::async, updateMotionBlur));

		// SHADOWS
		auto updateShadows = [&]() { ctx.shadows.update(ctx.camera_main); };
		futureUpdates.push_back(std::async(std::launch::async, updateShadows));

		// COMPOSITION UNIFORMS
		auto updateDeferred = [&]() { ctx.deferred.update(ctx.camera_main.invViewProjection); };
		futureUpdates.push_back(std::async(std::launch::async, updateDeferred));

		for (auto& f : futureUpdates)
			f.get();

		Queue::exec_memcpyRequests(previousImageIndex);

		GUI::updatesTimeCount = static_cast<float>(timer.Count());
	}

	void Renderer::recordComputeCmds(const uint32_t sizeX, const uint32_t sizeY, const uint32_t sizeZ)
	{
		//auto beginInfo = vk::CommandBufferBeginInfo()
		//	.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit)
		//	.setPInheritanceInfo(nullptr);
		//
		//auto& cmd = VulkanContext::get()->computeCmdBuffer;
		//cmd.begin(beginInfo);
		//
		//ctx.metrics[13].start(cmd);
		//cmd.bindPipeline(vk::PipelineBindPoint::eCompute, ctx.compute.pipeline.pipeline);
		//cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, ctx.compute.pipeline.compinfo.layout, 0, ctx.compute.DSCompute, nullptr);
		//cmd.dispatch(sizeX, sizeY, sizeZ);
		//ctx.metrics[13].end(&GUI::metrics[13]);
		//
		//cmd.end();
	}

	void Renderer::recordDeferredCmds(const uint32_t& imageIndex)
	{
		vk::CommandBufferBeginInfo beginInfo;
		beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

		const auto& cmd = VulkanContext::get()->dynamicCmdBuffers[imageIndex];

		cmd.begin(beginInfo);
		// TODO: add more queries (times the swapchain images), so they are not overlapped from previous frame
		ctx.metrics[0].start(&cmd);

		// SKYBOX
		SkyBox& skybox = GUI::shadow_cast ? ctx.skyBoxDay : ctx.skyBoxNight;

		// MODELS
		ctx.metrics[2].start(&cmd);
		ctx.deferred.batchStart(cmd, imageIndex, ctx.renderTargets["viewport"].extent.Value());
		for (auto& model : Model::models)
			model.draw();
		ctx.deferred.batchEnd();
		ctx.metrics[2].end(&GUI::metrics[2]);

		changeLayout(cmd, ctx.renderTargets["albedo"], LayoutState::ColorRead);
		changeLayout(cmd, ctx.renderTargets["depth"], LayoutState::ColorRead);
		changeLayout(cmd, ctx.renderTargets["normal"], LayoutState::ColorRead);
		changeLayout(cmd, ctx.renderTargets["srm"], LayoutState::ColorRead);
		changeLayout(cmd, ctx.renderTargets["emissive"], LayoutState::ColorRead);
		changeLayout(cmd, ctx.renderTargets["ssr"], LayoutState::ColorRead);
		changeLayout(cmd, ctx.renderTargets["ssaoBlur"], LayoutState::ColorRead);
		changeLayout(cmd, ctx.renderTargets["velocity"], LayoutState::ColorRead);
		changeLayout(cmd, ctx.renderTargets["taa"], LayoutState::ColorRead);
		for (auto& image : ctx.shadows.textures)
			changeLayout(cmd, image, LayoutState::DepthRead);

		// SCREEN SPACE AMBIENT OCCLUSION
		if (GUI::show_ssao) {
			ctx.metrics[3].start(&cmd);
			changeLayout(cmd, ctx.renderTargets["ssaoBlur"], LayoutState::ColorWrite);
			//const auto changeLayoutFunc = std::bind(&Renderer::changeLayout, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
			ctx.ssao.draw(cmd, imageIndex, changeLayout, ctx.renderTargets["ssao"]);
			changeLayout(cmd, ctx.renderTargets["ssaoBlur"], LayoutState::ColorRead);
			ctx.metrics[3].end(&GUI::metrics[3]);
		}

		// SCREEN SPACE REFLECTIONS
		if (GUI::show_ssr) {
			ctx.metrics[4].start(&cmd);
			changeLayout(cmd, ctx.renderTargets["ssr"], LayoutState::ColorWrite);
			ctx.ssr.draw(cmd, imageIndex, ctx.renderTargets["ssr"].extent.Value());
			changeLayout(cmd, ctx.renderTargets["ssr"], LayoutState::ColorRead);
			ctx.metrics[4].end(&GUI::metrics[4]);
		}

		// COMPOSITION
		ctx.metrics[5].start(&cmd);
		ctx.deferred.draw(cmd, imageIndex, ctx.shadows, skybox, ctx.renderTargets["viewport"].extent.Value());
		ctx.metrics[5].end(&GUI::metrics[5]);

		if (GUI::use_AntiAliasing) {
			// TAA
			if (GUI::use_TAA) {
				ctx.metrics[6].start(&cmd);
				ctx.taa.copyFrameImage(cmd, ctx.renderTargets["viewport"]);
				ctx.taa.draw(cmd, imageIndex, changeLayout, ctx.renderTargets);
				ctx.metrics[6].end(&GUI::metrics[6]);
			}
			// FXAA
			else if (GUI::use_FXAA) {
				ctx.metrics[6].start(&cmd);
				ctx.fxaa.copyFrameImage(cmd, ctx.renderTargets["viewport"]);
				ctx.fxaa.draw(cmd, imageIndex, ctx.renderTargets["viewport"].extent.Value());
				ctx.metrics[6].end(&GUI::metrics[6]);
			}
		}

		// BLOOM
		if (GUI::show_Bloom) {
			ctx.metrics[7].start(&cmd);
			ctx.bloom.copyFrameImage(cmd, ctx.renderTargets["viewport"]);
			ctx.bloom.draw(cmd, imageIndex, static_cast<uint32_t>(VulkanContext::get()->swapchain->images.size()), changeLayout, ctx.renderTargets);
			ctx.metrics[7].end(&GUI::metrics[7]);
		}

		// Depth of Field
		if (GUI::use_DOF) {
			ctx.metrics[8].start(&cmd);
			ctx.dof.copyFrameImage(cmd, ctx.renderTargets["viewport"]);
			ctx.dof.draw(cmd, imageIndex, ctx.renderTargets);
			ctx.metrics[8].end(&GUI::metrics[8]);
		}

		// MOTION BLUR
		if (GUI::show_motionBlur) {
			ctx.metrics[9].start(&cmd);
			ctx.motionBlur.copyFrameImage(cmd, ctx.renderTargets["viewport"]);
			ctx.motionBlur.draw(cmd, imageIndex, ctx.renderTargets["viewport"].extent.Value());
			ctx.metrics[9].end(&GUI::metrics[9]);
		}

		changeLayout(cmd, ctx.renderTargets["albedo"], LayoutState::ColorWrite);
		changeLayout(cmd, ctx.renderTargets["depth"], LayoutState::ColorWrite);
		changeLayout(cmd, ctx.renderTargets["normal"], LayoutState::ColorWrite);
		changeLayout(cmd, ctx.renderTargets["srm"], LayoutState::ColorWrite);
		changeLayout(cmd, ctx.renderTargets["emissive"], LayoutState::ColorWrite);
		changeLayout(cmd, ctx.renderTargets["ssr"], LayoutState::ColorWrite);
		changeLayout(cmd, ctx.renderTargets["ssaoBlur"], LayoutState::ColorWrite);
		changeLayout(cmd, ctx.renderTargets["velocity"], LayoutState::ColorWrite);
		changeLayout(cmd, ctx.renderTargets["taa"], LayoutState::ColorWrite);
		for (auto& image : ctx.shadows.textures)
			changeLayout(cmd, image, LayoutState::DepthWrite);

		// GUI
		ctx.metrics[10].start(&cmd);
		ctx.gui.scaleToRenderArea(cmd, ctx.renderTargets["viewport"], imageIndex);
		ctx.gui.draw(cmd, imageIndex);
		ctx.metrics[10].end(&GUI::metrics[10]);

		ctx.metrics[0].end(&GUI::metrics[0]);

		cmd.end();
	}

	void Renderer::recordShadowsCmds(const uint32_t& imageIndex)
	{
		// Render Pass (shadows mapping) (outputs the depth image with the light POV)

		const vk::DeviceSize offset = vk::DeviceSize();
		std::array<vk::ClearValue, 1> clearValuesShadows{};
		clearValuesShadows[0].depthStencil = vk::ClearDepthStencilValue{ 0.0f, 0 };

		vk::CommandBufferBeginInfo beginInfoShadows;
		beginInfoShadows.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

		vk::RenderPassBeginInfo renderPassInfoShadows;
		renderPassInfoShadows.renderPass = *ctx.shadows.renderPass;
		renderPassInfoShadows.renderArea = vk::Rect2D{ { 0, 0 },{ Shadows::imageSize, Shadows::imageSize } };
		renderPassInfoShadows.clearValueCount = static_cast<uint32_t>(clearValuesShadows.size());
		renderPassInfoShadows.pClearValues = clearValuesShadows.data();

		for (uint32_t i = 0; i < ctx.shadows.textures.size(); i++) {
			auto& cmd = VulkanContext::get()->shadowCmdBuffers[ctx.shadows.textures.size() * imageIndex + i];
			cmd.begin(beginInfoShadows);
			ctx.metrics[11 + static_cast<size_t>(i)].start(&cmd);
			cmd.setDepthBias(GUI::depthBias[0], GUI::depthBias[1], GUI::depthBias[2]);

			// depth[i] image ===========================================================
			renderPassInfoShadows.framebuffer = *ctx.shadows.framebuffers[ctx.shadows.textures.size() * imageIndex + i];
			cmd.beginRenderPass(renderPassInfoShadows, vk::SubpassContents::eInline);
			cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *ctx.shadows.pipeline.pipeline);
			for (auto& model : Model::models) {
				if (model.render) {
					cmd.bindVertexBuffers(0, model.vertexBuffer.buffer.Value(), offset);
					cmd.bindIndexBuffer(model.indexBuffer.buffer.Value(), 0, vk::IndexType::eUint32);

					for (auto& node : model.linearNodes) {
						if (node->mesh) {
							cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, ctx.shadows.pipeline.pipeinfo->layout, 0, { ctx.shadows.descriptorSets[i], node->mesh->descriptorSet, model.descriptorSet }, nullptr);
							for (auto& primitive : node->mesh->primitives) {
								if (primitive.render)
									cmd.drawIndexed(primitive.indicesSize, 1, node->mesh->indexOffset + primitive.indexOffset, node->mesh->vertexOffset + primitive.vertexOffset, 0);
							}
						}
					}
				}
			}
			cmd.endRenderPass();
			ctx.metrics[11 + static_cast<size_t>(i)].end(&GUI::metrics[11 + static_cast<size_t>(i)]);
			// ==========================================================================
			cmd.end();
		}
	}

	void Renderer::present()
	{
		auto& vCtx = *VulkanContext::get();

		static const vk::PipelineStageFlags waitStages[] = { vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader };

		FIRE_EVENT(Event::OnRender);

		if (GUI::use_compute) {
			//recordComputeCmds(2, 2, 1);
			//vk::SubmitInfo siCompute;
			//siCompute.commandBufferCount = 1;
			//siCompute.setPCommandBuffers = &VulkanContext::get()->computeCmdBuffer;
			//VulkanContext::get()->computeQueue.submit(siCompute, VulkanContext::get()->fences[1]);
			//VulkanContext::get()->device.waitForFences(VulkanContext::get()->fences[1], VK_TRUE, UINT64_MAX);
			//VulkanContext::get()->device.resetFences(VulkanContext::get()->fences[1]);
		}

		// aquire the image
		auto aquireSignalSemaphore = vCtx.semaphores[0];
		const uint32_t imageIndex = vCtx.swapchain->aquire(aquireSignalSemaphore, nullptr);
		this->previousImageIndex = imageIndex;

		//static Timer timer;
		//timer.Start();
		//vCtx.waitFences(vCtx.fences[imageIndex]);
		//FrameTimer::Instance().timestamps[0] = timer.Count();

		const auto& cmd = vCtx.dynamicCmdBuffers[imageIndex];

		vCtx.waitAndLockSubmits();

		if (GUI::shadow_cast) {

			// record the shadow command buffers
			recordShadowsCmds(imageIndex);

			// submit the shadow command buffers
			const auto& shadowWaitSemaphore = aquireSignalSemaphore;
			const auto& shadowSignalSemaphore = vCtx.semaphores[imageIndex * 3 + 1];
			const auto& scb = vCtx.shadowCmdBuffers;
			const auto size = ctx.shadows.textures.size();
			const auto i = size * imageIndex;
			const std::vector<vk::CommandBuffer> activeShadowCmdBuffers(scb.begin() + i, scb.begin() + i + size);
			vCtx.submit(activeShadowCmdBuffers, waitStages[0], shadowWaitSemaphore, shadowSignalSemaphore);

			aquireSignalSemaphore = shadowSignalSemaphore;
		}

		// record the command buffers
		recordDeferredCmds(imageIndex);

		// submit the command buffers
		const auto& deferredWaitStage = GUI::shadow_cast ? waitStages[1] : waitStages[0];
		const auto& deferredWaitSemaphore = aquireSignalSemaphore;
		const auto& deferredSignalSemaphore = vCtx.semaphores[imageIndex * 3 + 2];
		const auto& deferredSignalFence = vCtx.fences[imageIndex];
		vCtx.submit(cmd, deferredWaitStage, deferredWaitSemaphore, deferredSignalSemaphore, deferredSignalFence);

		// Presentation
		const auto& presentWaitSemaphore = deferredSignalSemaphore;
		vCtx.swapchain->present(imageIndex, presentWaitSemaphore);

		vCtx.unlockSubmits();
	}
}
