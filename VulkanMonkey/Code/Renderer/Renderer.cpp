#include "Renderer.h"
#include <iostream>
#include <random>
#include <chrono>

using namespace vm;

Renderer::Renderer(SDL_Window* window)
{
	ctx.vulkan.window = window;
	// INIT ALL VULKAN CONTEXT
	ctx.initVulkanContext();
	// INIT RENDERING
	ctx.initRendering();
	//LOAD RESOURCES
	ctx.loadResources();
	// CREATE UNIFORMS AND DESCRIPTOR SETS
	ctx.createUniforms();
	// init is done
	prepared = true;
}

Renderer::~Renderer()
{
	ctx.vulkan.device.waitIdle();

	if (Context::models.size() == 0) {
		if (Model::descriptorSetLayout) {
			ctx.vulkan.device.destroyDescriptorSetLayout(Model::descriptorSetLayout);
			Model::descriptorSetLayout = nullptr;
		}

		if (Mesh::descriptorSetLayout) {
			ctx.vulkan.device.destroyDescriptorSetLayout(Mesh::descriptorSetLayout);
			Mesh::descriptorSetLayout = nullptr;
		}
	}
	for (auto &model : Context::models)
		model.destroy();
	ctx.shadows.destroy();
	ctx.compute.destroy();
	ctx.forward.destroy();
	ctx.deferred.destroy();
	ctx.ssao.destroy();
	ctx.ssr.destroy();
	ctx.motionBlur.destroy();
	ctx.skyBox.destroy();
	ctx.terrain.destroy();
	ctx.gui.destroy();
	ctx.lightUniforms.destroy();
	ctx.destroyVkContext();
}

void Renderer::update(float delta)
{
	// TODO: make an other command pool for multithreading
	for (int i = static_cast<int>(Queue::loadModel.size()) - 1; i >= 0; i--) {
		Queue::func.push_back( std::async( std::launch::async, [](std::tuple<std::string, std::string>& temp) {
			VulkanContext::getVulkanContext().device.waitIdle();
			std::string path = std::get<0>(temp);
			std::string name = std::get<1>(temp);
			Context::models.push_back(Model());
			Context::models.back().loadModel(path, name);
		}, Queue::loadModel[i]));
		Queue::func.back().get();
		Queue::loadModel.pop_back();
		Queue::func.pop_back();
	}

	ctx.camera_main.update();
	const mat4 projection = ctx.camera_main.getPerspective();
	const mat4 view = ctx.camera_main.getView();
	const mat4 invProjection = ctx.camera_main.getInvPerspective();
	const mat4 invViewProjection = ctx.camera_main.getInvViewPerspective();


	// MODELS
	for (auto &model : Context::models) {
		model.render = GUI::render_models;
		if (model.render) {
			const mat4 pvm[3]{ projection, view, model.transform };
			ctx.camera_main.ExtractFrustum(model.transform);
			for (auto &mesh : model.meshes) {
				mesh.cull = !ctx.camera_main.SphereInFrustum(mesh.boundingSphere);
				if (!mesh.cull) {
					memcpy(model.uniformBuffer.data, &pvm, sizeof(pvm));
				}
			}
		}
	}

	// SHADOWS
	Shadows::shadowCast = GUI::shadow_cast;
	const vec3 pos = Light::sun().position;
	const vec3 front = normalize(-pos);
	const vec3 right = normalize(cross(front, ctx.camera_main.worldUp()));
	const vec3 up = normalize(cross(right, front));
	std::vector<ShadowsUBO> shadows_UBO(Context::models.size());
	for (uint32_t i = 0; i < Context::models.size(); i++)
		shadows_UBO[i] = { ortho(-40.f, 40.f, -40.f, 40.f, 500.f, 0.005f), lookAt(pos, front, right, up), Context::models[i].transform, Shadows::shadowCast ? 1.0f : 0.0f };
	memcpy(ctx.shadows.uniformBuffer.data, shadows_UBO.data(), sizeof(ShadowsUBO)*shadows_UBO.size());

	// GUI
	if (ctx.gui.render)
		ctx.gui.newFrame();

	// LIGHTS
	if (GUI::randomize_lights) {
		GUI::randomize_lights = false;
		LightsUBO lubo;
		lubo.camPos = vec4(ctx.camera_main.position, 1.0f);
		memcpy(ctx.lightUniforms.uniform.data, &lubo, sizeof(LightsUBO));
	}
	else {
		vec4 camPos(ctx.camera_main.position, 1.0f);
		memcpy(ctx.lightUniforms.uniform.data, &camPos, sizeof(vec4));
	}

	// SSAO
	if (GUI::show_ssao) {
		mat4 pvm[3]{ projection, view, invProjection };
		memcpy(ctx.ssao.UBssaoPVM.data, pvm, sizeof(pvm));
	}

	// SSR
	if (GUI::show_ssr) {
		mat4 reflectionInput[4];
		reflectionInput[0][0] = vec4(ctx.camera_main.position, 1.0f);
		reflectionInput[0][1] = vec4(ctx.camera_main.front(), 1.0f);
		reflectionInput[0][2] = vec4(static_cast<float>(ctx.vulkan.surface->actualExtent.width), static_cast<float>(ctx.vulkan.surface->actualExtent.height), 0.f, 0.f);
		reflectionInput[0][3] = vec4();
		reflectionInput[1] = projection;
		reflectionInput[2] = view;
		reflectionInput[3] = invProjection;
		memcpy(ctx.ssr.UBReflection.data, &reflectionInput, sizeof(reflectionInput));
	}

	// MOTION BLUR
	if (GUI::show_motionBlur) {
		static mat4 previousView = view;
		mat4 motionBlurInput[4]{ projection, view, previousView, invViewProjection };
		memcpy(ctx.motionBlur.UBmotionBlur.data, &motionBlurInput, sizeof(motionBlurInput));
		previousView = view;
	}

	//TERRAIN
	//if (ctx.terrain.render) {
	//	const mat4 pvm[3]{ projection, view };
	//	memcpy(ctx.terrain.uniformBuffer.data, &pvm, sizeof(pvm));
	//}
	// SKYBOX
	//if (ctx.skyBox.render) {
	//	const mat4 pvm[2]{ projection, view };
	//	memcpy(ctx.skyBox.uniformBuffer.data, &pvm, sizeof(pvm));
	//}

}

void Renderer::recordComputeCmds(const uint32_t sizeX, const uint32_t sizeY, const uint32_t sizeZ)
{
	auto beginInfo = vk::CommandBufferBeginInfo()
		.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit)
		.setPInheritanceInfo(nullptr);
	ctx.vulkan.computeCmdBuffer.begin(beginInfo);

	ctx.vulkan.computeCmdBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, ctx.compute.pipeline.pipeline);
	ctx.vulkan.computeCmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, ctx.compute.pipeline.compinfo.layout, 0, ctx.compute.DSCompute, nullptr);

	ctx.vulkan.computeCmdBuffer.dispatch(sizeX, sizeY, sizeZ);
	ctx.vulkan.computeCmdBuffer.end();
}

void Renderer::recordForwardCmds(const uint32_t& imageIndex)
{

	vec2 surfSize((float)ctx.vulkan.surface->actualExtent.width, (float)ctx.vulkan.surface->actualExtent.height);
	vec2 winPos((float*)&GUI::winPos);
	vec2 winSize((float*)&GUI::winSize);

	vec2 UVOffset[2] = { winPos / surfSize, winSize / surfSize };

	ctx.camera_main.renderArea.update(winPos, winSize, 0.f, 1.f);

	// Render Pass (color)
	std::vector<vk::ClearValue> clearValues = {
		vk::ClearColorValue().setFloat32(GUI::clearColor),
		vk::ClearColorValue().setFloat32(GUI::clearColor),
		vk::ClearDepthStencilValue({ 0.0f, 0 }) };

	auto beginInfo = vk::CommandBufferBeginInfo()
		.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit)
		.setPInheritanceInfo(nullptr);
	auto renderPassInfo = vk::RenderPassBeginInfo()
		.setRenderPass(ctx.forward.renderPass)
		.setFramebuffer(ctx.forward.frameBuffers[imageIndex])
		.setRenderArea({ { 0, 0 }, ctx.vulkan.surface->actualExtent })
		.setClearValueCount(static_cast<uint32_t>(clearValues.size()))
		.setPClearValues(clearValues.data());

	ctx.vulkan.dynamicCmdBuffer.begin(beginInfo);
	ctx.vulkan.dynamicCmdBuffer.setViewport(0, ctx.camera_main.renderArea.viewport);
	ctx.vulkan.dynamicCmdBuffer.setScissor(0, ctx.camera_main.renderArea.scissor);

	ctx.vulkan.dynamicCmdBuffer.beginRenderPass(renderPassInfo, vk::SubpassContents::eInline);

	// MODELS
	for (uint32_t m = 0; m < Context::models.size(); m++)
		Context::models[m].draw(ctx.forward.pipeline, ctx.vulkan.dynamicCmdBuffer, m, false, &ctx.shadows, &ctx.lightUniforms.descriptorSet);

	for (auto& cam : ctx.camera) {
		cam.renderArea.update(winPos + winSize * .7f, winSize * .2f, 0.f, 0.5f);
		ctx.vulkan.dynamicCmdBuffer.setViewport(0, cam.renderArea.viewport);
		ctx.vulkan.dynamicCmdBuffer.setScissor(0, cam.renderArea.scissor);

		// MODELS
		for (uint32_t m = 0; m < Context::models.size(); m++)
			Context::models[m].draw(ctx.forward.pipeline, ctx.vulkan.dynamicCmdBuffer, m, false, &ctx.shadows, &ctx.lightUniforms.descriptorSet);
	}
	ctx.vulkan.dynamicCmdBuffer.endRenderPass();

	// GUI
	ctx.gui.draw(ctx.gui.renderPass, ctx.gui.frameBuffers[imageIndex], ctx.gui.pipeline, ctx.vulkan.dynamicCmdBuffer);

	ctx.vulkan.dynamicCmdBuffer.end();
}

void Renderer::recordDeferredCmds(const uint32_t& imageIndex)
{
	vec2 surfSize((float)ctx.vulkan.surface->actualExtent.width, (float)ctx.vulkan.surface->actualExtent.height);
	vec2 winPos((float*)&GUI::winPos);
	vec2 winSize((float*)&GUI::winSize);

	vec2 UVOffset[2] = { winPos / surfSize, winSize / surfSize };

	ctx.camera_main.renderArea.update(winPos, winSize);

	auto beginInfo = vk::CommandBufferBeginInfo()
		.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit)
		.setPInheritanceInfo(nullptr);

	std::vector<vk::ClearValue> clearValues = {
		vk::ClearColorValue().setFloat32(GUI::clearColor),
		vk::ClearColorValue().setFloat32(GUI::clearColor),
		vk::ClearColorValue().setFloat32(GUI::clearColor),
		vk::ClearColorValue().setFloat32(GUI::clearColor),
		vk::ClearDepthStencilValue({ 0.0f, 0 }) };
	auto renderPassInfo = vk::RenderPassBeginInfo()
		.setRenderPass(ctx.deferred.renderPass)
		.setFramebuffer(ctx.deferred.frameBuffers[imageIndex])
		.setRenderArea({ { 0, 0 }, ctx.vulkan.surface->actualExtent })
		.setClearValueCount(static_cast<uint32_t>(clearValues.size()))
		.setPClearValues(clearValues.data());

	ctx.vulkan.dynamicCmdBuffer.begin(beginInfo);
	ctx.vulkan.dynamicCmdBuffer.setViewport(0, ctx.camera_main.renderArea.viewport);
	ctx.vulkan.dynamicCmdBuffer.setScissor(0, ctx.camera_main.renderArea.scissor);

	if (Context::models.size() > 0) {
		ctx.vulkan.dynamicCmdBuffer.beginRenderPass(renderPassInfo, vk::SubpassContents::eInline);
		// MODELS
		for (uint32_t m = 0; m < Context::models.size(); m++)
			Context::models[m].draw(ctx.deferred.pipeline, ctx.vulkan.dynamicCmdBuffer, m, true);
		ctx.vulkan.dynamicCmdBuffer.endRenderPass();

		// SCREEN SPACE AMBIENT OCCLUSION
		if (GUI::show_ssao)
			ctx.ssao.draw(imageIndex, UVOffset);

		// SCREEN SPACE REFLECTIONS
		if (GUI::show_ssr)
			ctx.ssr.draw(imageIndex, UVOffset);

		// COMPOSITION
		ctx.deferred.draw(imageIndex, ctx.shadows, ctx.camera_main.getInvViewPerspective(), UVOffset);
		
		// MOTION BLUR
		if (GUI::show_motionBlur)
			ctx.motionBlur.draw(imageIndex, UVOffset);
	}

	// GUI
	ctx.gui.draw(ctx.gui.renderPass, ctx.gui.frameBuffers[imageIndex], ctx.gui.pipeline, ctx.vulkan.dynamicCmdBuffer);

	ctx.vulkan.dynamicCmdBuffer.end();
}

void Renderer::recordShadowsCmds(const uint32_t& imageIndex)
{
	// Render Pass (shadows mapping) (outputs the depth image with the light POV)

	std::array<vk::ClearValue, 1> clearValuesShadows = {};
	clearValuesShadows[0].setDepthStencil({ 0.0f, 0 });
	auto beginInfoShadows = vk::CommandBufferBeginInfo()
		.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit)
		.setPInheritanceInfo(nullptr);
	auto renderPassInfoShadows = vk::RenderPassBeginInfo()
		.setRenderPass(ctx.shadows.getRenderPass())
		.setFramebuffer(ctx.shadows.frameBuffer[imageIndex])
		.setRenderArea({ { 0, 0 },{ ctx.shadows.imageSize, ctx.shadows.imageSize } })
		.setClearValueCount(static_cast<uint32_t>(clearValuesShadows.size()))
		.setPClearValues(clearValuesShadows.data());

	ctx.vulkan.shadowCmdBuffer.begin(beginInfoShadows);
	ctx.vulkan.shadowCmdBuffer.beginRenderPass(renderPassInfoShadows, vk::SubpassContents::eInline);

	vk::DeviceSize offset = vk::DeviceSize();

	ctx.vulkan.shadowCmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, ctx.shadows.pipeline.pipeline);

	for (uint32_t m = 0; m < Context::models.size(); m++) {
		if (Context::models[m].render) {
			ctx.vulkan.shadowCmdBuffer.bindVertexBuffers(0, Context::models[m].vertexBuffer.buffer, offset);
			ctx.vulkan.shadowCmdBuffer.bindIndexBuffer(Context::models[m].indexBuffer.buffer, 0, vk::IndexType::eUint32);

			const uint32_t dOffsets =  m * sizeof(ShadowsUBO);
			ctx.vulkan.shadowCmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, ctx.shadows.pipeline.pipeinfo.layout, 0, ctx.shadows.descriptorSet, dOffsets);

			for (auto& mesh : Context::models[m].meshes) {
				if (mesh.render)
					ctx.vulkan.shadowCmdBuffer.drawIndexed(static_cast<uint32_t>(mesh.indices.size()), 1, mesh.indexOffset, mesh.vertexOffset, 0);
			}
		}
	}

	ctx.vulkan.shadowCmdBuffer.endRenderPass();
	ctx.vulkan.shadowCmdBuffer.end();
}

void Renderer::present()
{
	if (!prepared) return;

	if (useCompute) {
		recordComputeCmds(2, 2, 1);
		auto const siCompute = vk::SubmitInfo()
			.setCommandBufferCount(1)
			.setPCommandBuffers(&ctx.vulkan.computeCmdBuffer);
		ctx.vulkan.computeQueue.submit(siCompute, ctx.vulkan.fences[1]);
		ctx.vulkan.device.waitForFences(ctx.vulkan.fences[1], VK_TRUE, UINT64_MAX);
		ctx.vulkan.device.resetFences(ctx.vulkan.fences[1]);
	}

	// what stage of a pipeline at a command buffer to wait for the semaphores to be done until keep going
	const vk::PipelineStageFlags waitStages[] = { vk::PipelineStageFlagBits::eColorAttachmentOutput };

	uint32_t imageIndex;

	// if using shadows use the semaphore[0], record and submit the shadow commands, else use the semaphore[1]
	if (Shadows::shadowCast) {
		imageIndex = ctx.vulkan.device.acquireNextImageKHR(ctx.vulkan.swapchain->swapchain, UINT64_MAX, ctx.vulkan.semaphores[0], vk::Fence()).value;

		recordShadowsCmds(imageIndex);

		auto const siShadows = vk::SubmitInfo()
			.setWaitSemaphoreCount(1)
			.setPWaitSemaphores(&ctx.vulkan.semaphores[0])
			.setPWaitDstStageMask(waitStages)
			.setCommandBufferCount(1)
			.setPCommandBuffers(&ctx.vulkan.shadowCmdBuffer)
			.setSignalSemaphoreCount(1)
			.setPSignalSemaphores(&ctx.vulkan.semaphores[1]);
		ctx.vulkan.graphicsQueue.submit(siShadows, nullptr);
	}
	else
		imageIndex = ctx.vulkan.device.acquireNextImageKHR(ctx.vulkan.swapchain->swapchain, UINT64_MAX, ctx.vulkan.semaphores[1], vk::Fence()).value;

	if (GUI::deferred_rendering) 
		recordDeferredCmds(imageIndex);
	else
		recordForwardCmds(imageIndex);

	// submit the command buffer
	auto const si = vk::SubmitInfo()
		.setWaitSemaphoreCount(1)
		.setPWaitSemaphores(&ctx.vulkan.semaphores[1])
		.setPWaitDstStageMask(waitStages)
		.setCommandBufferCount(1)
		.setPCommandBuffers(&ctx.vulkan.dynamicCmdBuffer)
		.setSignalSemaphoreCount(1)
		.setPSignalSemaphores(&ctx.vulkan.semaphores[2]);
	ctx.vulkan.graphicsQueue.submit(si, ctx.vulkan.fences[0]);

	// Presentation
	auto const pi = vk::PresentInfoKHR()
		.setWaitSemaphoreCount(1)
		.setPWaitSemaphores(&ctx.vulkan.semaphores[2])
		.setSwapchainCount(1)
		.setPSwapchains(&ctx.vulkan.swapchain->swapchain)
		.setPImageIndices(&imageIndex)
		.setPResults(nullptr); //optional
	ctx.vulkan.presentQueue.presentKHR(pi);

	ctx.vulkan.device.waitForFences(ctx.vulkan.fences[0], VK_TRUE, UINT64_MAX);
	ctx.vulkan.device.resetFences(ctx.vulkan.fences[0]);

	if (overloadedGPU)
		ctx.vulkan.presentQueue.waitIdle(); // user set, when GPU can't catch the CPU commands 
}