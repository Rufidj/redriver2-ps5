#pragma once
// What the sample's base class gives REDRIVER2's Vulkan renderer (ps5/src/red2_render.cpp).
#include "vulkanexamplebase.h"
#include <functional>
#include <string>
struct Red2Host {
	VkDevice device;
	VkPhysicalDevice physicalDevice;
	VkQueue queue;
	vks::VulkanDevice* vulkanDevice;
	VkPipelineCache pipelineCache;
	VkFormat swapFormat;
	VkFormat depthFormat;
	uint32_t surfaceW, surfaceH;
	std::string shadersPath;
	std::function<VkCommandBuffer()> beginFrame;               // waits the frame slot, acquires the image, begins the command buffer
	std::function<void(VkCommandBuffer)> beginPresentPass;      // base class: barriers + dynamic rendering on the swapchain
	std::function<void(VkCommandBuffer)> endPresentPass;
	std::function<void()> endFrame;                             // ends the command buffer, submits, presents
	std::function<uint32_t()> frameSlot;
};

extern "C" void red2_attach(Red2Host* host);
