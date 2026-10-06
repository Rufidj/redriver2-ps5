/*
 * REDRIVER 2 - REDRIVER2 on the PS5, drawing through ps5/src/red2_render.cpp.
 *
 * The base class gives the game its display: the device, the swapchain, the frame slots and the
 * present pass. The render loop is the game's own main (game/libred2vk.a), which calls the GR_*
 * layer for every frame it draws.
 *
 * Copyright (C) 2026 Mihawk
 *
 * This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
 */

#include "vulkanexamplebase.h"
#include "red2_host.h"

extern "C" int red2_main(int argc, char **argv);

class VulkanExample : public VulkanExampleBase
{
public:
	// the renderer's barriers are synchronization2's
	VkPhysicalDeviceSynchronization2Features sync2{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, .pNext = nullptr, .synchronization2 = VK_TRUE };

	VulkanExample() : VulkanExampleBase()
	{
		title = "REDRIVER 2";
		apiVersion = VK_API_VERSION_1_3;
		useDynamicRendering = true;
		requiresStencil = true;
		sync2.pNext = &baseDynamicRenderingFeatures;
		deviceCreatepNextChain = &sync2;
		settings.overlay = false;
	}

	void getEnabledFeatures() override
	{
		if (deviceFeatures.samplerAnisotropy) {
			enabledFeatures.samplerAnisotropy = VK_TRUE;
		}
	}

	void prepare() override
	{
		VulkanExampleBase::prepare();
		prepared = true;
	}

	void render() override {}

	// The game is the loop: it returns when the player quits
	void renderLoop() override
	{
		Red2Host host{};
		host.device = device;
		host.physicalDevice = physicalDevice;
		host.queue = queue;
		host.vulkanDevice = vulkanDevice;
		host.pipelineCache = pipelineCache;
		host.swapFormat = swapChain.colorFormat;
		host.depthFormat = depthFormat;
		host.surfaceW = width;
		host.surfaceH = height;
		host.shadersPath = getShadersPath();
		host.beginFrame = [this]() {
			VulkanExampleBase::prepareFrame();
			VkCommandBuffer cmd = drawCmdBuffers[currentBuffer];
			VkCommandBufferBeginInfo info = vks::initializers::commandBufferBeginInfo();
			VK_CHECK_RESULT(vkBeginCommandBuffer(cmd, &info));
			return cmd;
		};
		host.beginPresentPass = [this](VkCommandBuffer cmd) { beginDynamicRendering(cmd); };
		host.endPresentPass = [this](VkCommandBuffer cmd) { endDynamicRendering(cmd); };
		host.endFrame = [this]() {
			VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[currentBuffer]));
			VulkanExampleBase::submitFrame();
		};
		host.frameSlot = [this]() { return currentBuffer; };
		red2_attach(&host);
		static char arg0[] = "REDRIVER2";
		static char *argv[] = { arg0, nullptr };
		const int code = red2_main(1, argv);
		red2_attach(nullptr);
		vkDeviceWaitIdle(device);
		(void)code;
	}
};

VULKAN_EXAMPLE_MAIN()
