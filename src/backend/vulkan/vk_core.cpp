#include "vk_core.h"

#include "../../core/utils_functions.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace hrlvk
{
static Core g_core;
Core& GetCore() { return g_core; }

void Error(const char* what, VkResult result)
{
	std::ostringstream ss;
	ss << "Vulkan: " << what << " failed (VkResult=" << static_cast<int>(result) << ")";
	SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, ss.str());
}

void Error(const std::string& what)
{
	SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "Vulkan: " + what);
}

bool SamplerState::operator==(const SamplerState& o) const
{
	return magFilter == o.magFilter && minFilter == o.minFilter && mipmapMode == o.mipmapMode &&
		useMips == o.useMips && wrapU == o.wrapU && wrapV == o.wrapV && wrapW == o.wrapW &&
		borderWhite == o.borderWhite && anisotropy == o.anisotropy;
}

// ---------------------------------------------------------------------------
// Instance / device
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
	if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && data && data->pMessage)
	{
		std::fprintf(stderr, "[HRL Vulkan validation] %s\n", data->pMessage);
		if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
			SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_WARNING, std::string("Vulkan validation: ") + data->pMessage);
	}
	return VK_FALSE;
}

static bool HasLayer(const char* name)
{
	uint32_t count = 0;
	vkEnumerateInstanceLayerProperties(&count, nullptr);
	std::vector<VkLayerProperties> layers(count);
	vkEnumerateInstanceLayerProperties(&count, layers.data());
	for (const auto& l : layers)
		if (std::strcmp(l.layerName, name) == 0) return true;
	return false;
}

static bool HasInstanceExtension(const char* name, const char* layer = nullptr)
{
	uint32_t count = 0;
	vkEnumerateInstanceExtensionProperties(layer, &count, nullptr);
	std::vector<VkExtensionProperties> exts(count);
	vkEnumerateInstanceExtensionProperties(layer, &count, exts.data());
	for (const auto& e : exts)
		if (std::strcmp(e.extensionName, name) == 0) return true;
	return false;
}

bool Core::CreateInstance()
{
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "Horizon Rendering Library";
	app.applicationVersion = VK_MAKE_VERSION(0, 8, 0);
	app.pEngineName = "HRL";
	app.engineVersion = VK_MAKE_VERSION(0, 8, 0);
	app.apiVersion = VK_API_VERSION_1_1;

	std::vector<const char*> extensions;
	auto addExt = [&](const char* e) {
		for (const char* x : extensions) if (std::strcmp(x, e) == 0) return;
		extensions.push_back(e);
	};
	if (surfaceCallback)
	{
		addExt(VK_KHR_SURFACE_EXTENSION_NAME);
		for (const std::string& e : instanceExtensions)
			if (!e.empty()) addExt(e.c_str());
	}

	std::vector<const char*> layers;
	const char* validationEnv = std::getenv("HRL_VULKAN_VALIDATION");
	const bool validation = validationEnv && *validationEnv && *validationEnv != '0' && HasLayer("VK_LAYER_KHRONOS_validation");
	// VK_EXT_debug_utils is provided by the validation layer itself.
	const bool debugUtils = validation && (HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) ||
		HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, "VK_LAYER_KHRONOS_validation"));
	if (validation) layers.push_back("VK_LAYER_KHRONOS_validation");
	if (debugUtils) addExt(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ci.pApplicationInfo = &app;
	ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	ci.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
	ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
	ci.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
	VkResult r = vkCreateInstance(&ci, nullptr, &instance);
	if (r != VK_SUCCESS) { Error("vkCreateInstance", r); return false; }

	if (debugUtils)
	{
		auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
		if (create)
		{
			VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
			mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
			mi.pfnUserCallback = DebugCallback;
			create(instance, &mi, nullptr, &messenger_);
		}
	}

	if (surfaceCallback)
	{
		r = static_cast<VkResult>(surfaceCallback(reinterpret_cast<void*>(instance), reinterpret_cast<void*>(&surface), surfaceUser));
		if (r != VK_SUCCESS || surface == VK_NULL_HANDLE)
		{
			Error("surface creation callback", r);
			return false;
		}
	}
	return true;
}

bool Core::PickDevice()
{
	uint32_t count = 0;
	if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0)
	{
		Error("no physical device found");
		return false;
	}
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(instance, &count, devices.data());

	int bestScore = -1;
	for (VkPhysicalDevice candidate : devices)
	{
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
		int gfx = -1, present = -1;
		for (uint32_t i = 0; i < familyCount; ++i)
		{
			const bool isGfx = families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT);
			VkBool32 supportsPresent = VK_FALSE;
			if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supportsPresent);
			if (isGfx && (!surface || supportsPresent)) { gfx = present = (int)i; break; }
			if (isGfx && gfx < 0) gfx = (int)i;
			if (supportsPresent && present < 0) present = (int)i;
		}
		if (gfx < 0 || (surface && present < 0))
			continue;
		if (surface)
		{
			uint32_t extCount = 0;
			vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
			std::vector<VkExtensionProperties> exts(extCount);
			vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, exts.data());
			bool hasSwapchain = false;
			for (const auto& e : exts) if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) hasSwapchain = true;
			if (!hasSwapchain) continue;
		}
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(candidate, &props);
		int score = 1;
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score = 4;
		else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 3;
		else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) score = 2;
		if (score > bestScore)
		{
			bestScore = score;
			physical = candidate;
			queueFamily = (uint32_t)gfx;
			presentFamily = (uint32_t)(surface ? present : gfx);
		}
	}
	if (!physical)
	{
		Error("no suitable graphics/present queue family found");
		return false;
	}
	vkGetPhysicalDeviceProperties(physical, &properties);
	return true;
}

bool Core::CreateDevice()
{
	float priority = 1.f;
	std::array<VkDeviceQueueCreateInfo, 2> queues{};
	uint32_t queueCount = 1;
	queues[0] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queues[0].queueFamilyIndex = queueFamily;
	queues[0].queueCount = 1;
	queues[0].pQueuePriorities = &priority;
	if (presentFamily != queueFamily)
	{
		queues[1] = queues[0];
		queues[1].queueFamilyIndex = presentFamily;
		queueCount = 2;
	}

	VkPhysicalDeviceFeatures available{};
	vkGetPhysicalDeviceFeatures(physical, &available);
	enabledFeatures = {};
	enabledFeatures.samplerAnisotropy = available.samplerAnisotropy;
	enabledFeatures.fillModeNonSolid = available.fillModeNonSolid;
	enabledFeatures.wideLines = available.wideLines;
	enabledFeatures.independentBlend = available.independentBlend;
	enabledFeatures.sampleRateShading = VK_FALSE;

	std::vector<const char*> extensions;
	if (surface) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

	VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	ci.queueCreateInfoCount = queueCount;
	ci.pQueueCreateInfos = queues.data();
	ci.pEnabledFeatures = &enabledFeatures;
	ci.enabledExtensionCount = (uint32_t)extensions.size();
	ci.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
	VkResult r = vkCreateDevice(physical, &ci, nullptr, &device);
	if (r != VK_SUCCESS) { Error("vkCreateDevice", r); return false; }
	vkGetDeviceQueue(device, queueFamily, 0, &queue);
	vkGetDeviceQueue(device, presentFamily, 0, &presentQueue);

	supportedSamples = (properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts) | VK_SAMPLE_COUNT_1_BIT;

	// Depth format: must be usable both as attachment and sampled image.
	const VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
	for (VkFormat f : candidates)
	{
		VkFormatProperties p{};
		vkGetPhysicalDeviceFormatProperties(physical, f, &p);
		const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		if ((p.optimalTilingFeatures & need) == need) { depthFormat = f; break; }
	}

	VkPipelineCacheCreateInfo pc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
	vkCreatePipelineCache(device, &pc, nullptr, &pipelineCache);
	return true;
}

bool Core::CreateFrames()
{
	VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pi.queueFamilyIndex = queueFamily;
	pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	if (vkCreateCommandPool(device, &pi, nullptr, &pool_) != VK_SUCCESS) return false;

	VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = pool_;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	for (auto& f : frames)
	{
		if (vkAllocateCommandBuffers(device, &ai, &f.cmd) != VK_SUCCESS) return false;
		if (vkCreateFence(device, &fi, nullptr, &f.fence) != VK_SUCCESS) return false;
		if (vkCreateSemaphore(device, &si, nullptr, &f.imageAvailable) != VK_SUCCESS) return false;
		if (vkCreateSemaphore(device, &si, nullptr, &f.renderFinished) != VK_SUCCESS) return false;
	}
	if (vkAllocateCommandBuffers(device, &ai, &setupCmd_) != VK_SUCCESS) return false;
	VkFenceCreateInfo sf{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (vkCreateFence(device, &sf, nullptr, &setupFence_) != VK_SUCCESS) return false;
	return true;
}

bool Core::CreateSwapchain(uint32_t width, uint32_t height)
{
	if (!surface) return true;
	VkSurfaceCapabilitiesKHR caps{};
	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps) != VK_SUCCESS) return false;
	uint32_t formatCount = 0, modeCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr);
	vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, nullptr);
	if (formatCount == 0) return false;
	std::vector<VkSurfaceFormatKHR> formats(formatCount);
	std::vector<VkPresentModeKHR> modes(modeCount);
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats.data());
	if (modeCount) vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, modes.data());

	// Same as the default OpenGL framebuffer : 8-bit UNORM (no automatic sRGB encoding).
	VkSurfaceFormatKHR format = formats[0];
	for (const auto& f : formats)
		if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
			f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { format = f; break; }
	swapchainFormat = format.format;

	if (caps.currentExtent.width != UINT32_MAX) swapchainExtent = caps.currentExtent;
	else
	{
		swapchainExtent.width = std::clamp(width, caps.minImageExtent.width, std::max(caps.minImageExtent.width, caps.maxImageExtent.width));
		swapchainExtent.height = std::clamp(height, caps.minImageExtent.height, std::max(caps.minImageExtent.height, caps.maxImageExtent.height));
	}
	if (swapchainExtent.width == 0 || swapchainExtent.height == 0)
		return true; // minimized : no swapchain until the next resize

	// Immediate presentation like the OpenGL backend with swap interval 0 is
	// left to the application; FIFO is always available and never tears.
	VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
	if (const char* m = std::getenv("HRL_VULKAN_PRESENT_MODE"))
	{
		const VkPresentModeKHR wanted = std::strcmp(m, "mailbox") == 0 ? VK_PRESENT_MODE_MAILBOX_KHR
			: std::strcmp(m, "immediate") == 0 ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
		for (auto pm : modes) if (pm == wanted) presentMode = wanted;
	}

	uint32_t imageCount = caps.minImageCount + 1;
	if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;
	if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
	{
		Error("the surface does not support transfer-destination swapchain images");
		return false;
	}
	VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	ci.surface = surface;
	ci.minImageCount = imageCount;
	ci.imageFormat = swapchainFormat;
	ci.imageColorSpace = format.colorSpace;
	ci.imageExtent = swapchainExtent;
	ci.imageArrayLayers = 1;
	ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	const uint32_t families[2] = {queueFamily, presentFamily};
	if (queueFamily != presentFamily)
	{
		ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		ci.queueFamilyIndexCount = 2;
		ci.pQueueFamilyIndices = families;
	}
	else ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
	ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
	{
		for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
			if (caps.supportedCompositeAlpha & a) { ci.compositeAlpha = a; break; }
	}
	ci.presentMode = presentMode;
	ci.clipped = VK_TRUE;
	VkResult r = vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain);
	if (r != VK_SUCCESS) { Error("vkCreateSwapchainKHR", r); return false; }
	vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
	swapchainImages.resize(imageCount);
	vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data());
	return true;
}

void Core::DestroySwapchain()
{
	if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
	swapchain = VK_NULL_HANDLE;
	swapchainImages.clear();
}

bool Core::RecreateSwapchain(uint32_t width, uint32_t height)
{
	if (!surface) return true;
	vkDeviceWaitIdle(device);
	DestroySwapchain();
	return CreateSwapchain(width, height);
}

bool Core::Init(uint32_t width, uint32_t height)
{
	if (device) return true;
	if (!CreateInstance() || !PickDevice() || !CreateDevice() || !CreateFrames())
	{
		Shutdown();
		return false;
	}
	if (surface && !CreateSwapchain(width, height))
	{
		Error("failed to create the swapchain");
		Shutdown();
		return false;
	}
	return true;
}

void Core::Shutdown()
{
	if (device)
	{
		vkDeviceWaitIdle(device);
		for (auto& f : frames)
		{
			for (auto& fn : f.deletions) fn();
			f.deletions.clear();
		}
		for (auto& fn : setupDeletions_) fn();
		setupDeletions_.clear();
		for (auto& s : samplers_) vkDestroySampler(device, s.sampler, nullptr);
		samplers_.clear();
		DestroySwapchain();
		for (auto& f : frames)
		{
			if (f.fence) vkDestroyFence(device, f.fence, nullptr);
			if (f.imageAvailable) vkDestroySemaphore(device, f.imageAvailable, nullptr);
			if (f.renderFinished) vkDestroySemaphore(device, f.renderFinished, nullptr);
			f = FrameData{};
		}
		if (setupFence_) vkDestroyFence(device, setupFence_, nullptr);
		setupFence_ = VK_NULL_HANDLE;
		if (pool_) vkDestroyCommandPool(device, pool_, nullptr);
		pool_ = VK_NULL_HANDLE;
		setupCmd_ = VK_NULL_HANDLE;
		setupOpen_ = false;
		for (auto& b : blocks_)
			if (b.memory) vkFreeMemory(device, b.memory, nullptr);
		blocks_.clear();
		if (pipelineCache) vkDestroyPipelineCache(device, pipelineCache, nullptr);
		pipelineCache = VK_NULL_HANDLE;
		vkDestroyDevice(device, nullptr);
		device = VK_NULL_HANDLE;
	}
	if (instance)
	{
		if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
		surface = VK_NULL_HANDLE;
		if (messenger_)
		{
			auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
			if (destroy) destroy(instance, messenger_, nullptr);
			messenger_ = VK_NULL_HANDLE;
		}
		vkDestroyInstance(instance, nullptr);
		instance = VK_NULL_HANDLE;
	}
	physical = VK_NULL_HANDLE;
	queue = presentQueue = VK_NULL_HANDLE;
	frameOpen = false;
	swapchainImageAcquired = false;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------
VkCommandBuffer Core::Cmd()
{
	if (frameOpen)
		return Frame().cmd;
	if (!setupOpen_)
	{
		vkResetCommandBuffer(setupCmd_, 0);
		VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(setupCmd_, &bi);
		setupOpen_ = true;
	}
	return setupCmd_;
}

void Core::Defer(std::function<void()> fn)
{
	if (!device) { fn(); return; }
	if (frameOpen) Frame().deletions.push_back(std::move(fn));
	else setupDeletions_.push_back(std::move(fn));
}

void Core::SubmitSetup(bool wait)
{
	if (!setupOpen_) return;
	if (onBeforeSubmit) onBeforeSubmit();
	vkEndCommandBuffer(setupCmd_);
	setupOpen_ = false;
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &setupCmd_;
	vkResetFences(device, 1, &setupFence_);
	VkResult r = vkQueueSubmit(queue, 1, &si, setupFence_);
	if (r != VK_SUCCESS) Error("vkQueueSubmit(setup)", r);
	if (wait || true)
	{
		// Setup work is rare (resource creation outside a frame). Waiting keeps
		// the lifetime of its staging buffers trivial.
		vkWaitForFences(device, 1, &setupFence_, VK_TRUE, UINT64_MAX);
		for (auto& fn : setupDeletions_) fn();
		setupDeletions_.clear();
	}
}

bool Core::BeginFrame()
{
	if (!device || frameOpen) return frameOpen;
	SubmitSetup(true);

	FrameData& f = Frame();
	vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
	for (auto& fn : f.deletions) fn();
	f.deletions.clear();

	swapchainImageAcquired = false;
	swapchainImageWritten = false;
	if (swapchain)
	{
		VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &swapchainIndex);
		if (r == VK_ERROR_OUT_OF_DATE_KHR)
		{
			RecreateSwapchain(swapchainExtent.width, swapchainExtent.height);
			if (swapchain)
				r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &swapchainIndex);
		}
		if (swapchain && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) swapchainImageAcquired = true;
		else if (swapchain) Error("vkAcquireNextImageKHR", r);
	}

	vkResetCommandBuffer(f.cmd, 0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(f.cmd, &bi);
	frameOpen = true;
	f.submitted = false;
	return true;
}

void Core::EndFrame()
{
	if (!frameOpen) return;
	if (onBeforeSubmit) onBeforeSubmit();
	FrameData& f = Frame();

	if (swapchainImageAcquired)
	{
		VkImage img = swapchainImages[swapchainIndex];
		VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = img;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		if (!swapchainImageWritten)
		{
			// Nothing was presented this frame : clear like an empty default framebuffer.
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			VkClearColorValue black{{0.f, 0.f, 0.f, 1.f}};
			VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdClearColorImage(f.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		}
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = 0;
		vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
	}

	vkEndCommandBuffer(f.cmd);
	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &f.cmd;
	if (swapchainImageAcquired)
	{
		si.waitSemaphoreCount = 1;
		si.pWaitSemaphores = &f.imageAvailable;
		si.pWaitDstStageMask = &waitStage;
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &f.renderFinished;
	}
	vkResetFences(device, 1, &f.fence);
	VkResult r = vkQueueSubmit(queue, 1, &si, f.fence);
	if (r != VK_SUCCESS) Error("vkQueueSubmit", r);
	f.submitted = true;
	frameOpen = false;

	if (swapchainImageAcquired)
	{
		VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &f.renderFinished;
		pi.swapchainCount = 1;
		pi.pSwapchains = &swapchain;
		pi.pImageIndices = &swapchainIndex;
		r = vkQueuePresentKHR(presentQueue, &pi);
		if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
			RecreateSwapchain(swapchainExtent.width, swapchainExtent.height);
		else if (r != VK_SUCCESS)
			Error("vkQueuePresentKHR", r);
		swapchainImageAcquired = false;
	}
	frameIndex = (frameIndex + 1) % kFramesInFlight;
	++frameNumber;
}

void Core::FlushAndWait()
{
	if (!device) return;
	if (frameOpen)
	{
		// Split the frame : submit what is recorded so far, wait, continue
		// recording in the same command buffer slot.
		if (onBeforeSubmit) onBeforeSubmit();
		FrameData& f = Frame();
		vkEndCommandBuffer(f.cmd);
		VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &f.cmd;
		vkResetFences(device, 1, &f.fence);
		VkResult r = vkQueueSubmit(queue, 1, &si, f.fence);
		if (r != VK_SUCCESS) Error("vkQueueSubmit", r);
		vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
		vkResetCommandBuffer(f.cmd, 0);
		VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(f.cmd, &bi);
		// The fence is signaled again: EndFrame resets it before its own submit.
		return;
	}
	SubmitSetup(true);
	vkQueueWaitIdle(queue);
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
uint32_t Core::FindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
{
	VkPhysicalDeviceMemoryProperties mem{};
	vkGetPhysicalDeviceMemoryProperties(physical, &mem);
	for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
		if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & flags) == flags)
			return i;
	return UINT32_MAX;
}

static VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a) { return a ? (v + a - 1) / a * a : v; }

bool Core::Allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags flags, bool hostVisible, Allocation& out)
{
	out = {};
	uint32_t type = FindMemoryType(req.memoryTypeBits, flags);
	if (type == UINT32_MAX && hostVisible)
		type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (type == UINT32_MAX)
		type = FindMemoryType(req.memoryTypeBits, 0);
	if (type == UINT32_MAX)
	{
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "Vulkan: no compatible memory type");
		return false;
	}
	const VkDeviceSize blockSize = hostVisible ? (16ull << 20) : (64ull << 20);
	// Buffers and images never share a block (bufferImageGranularity).
	const VkDeviceSize alignment = std::max<VkDeviceSize>(req.alignment, properties.limits.bufferImageGranularity);
	if (req.size <= blockSize / 2)
	{
		for (size_t bi = 0; bi < blocks_.size(); ++bi)
		{
			Block& b = blocks_[bi];
			if (b.type != type || b.memory == VK_NULL_HANDLE) continue;
			for (size_t i = 0; i < b.freeList.size(); ++i)
			{
				auto& range = b.freeList[i];
				const VkDeviceSize offset = AlignUp(range.offset, alignment);
				const VkDeviceSize end = offset + req.size;
				if (end > range.offset + range.size) continue;
				const VkDeviceSize rangeEnd = range.offset + range.size;
				std::vector<Block::Range> replacement;
				if (offset > range.offset) replacement.push_back({range.offset, offset - range.offset});
				if (end < rangeEnd) replacement.push_back({end, rangeEnd - end});
				b.freeList.erase(b.freeList.begin() + (ptrdiff_t)i);
				b.freeList.insert(b.freeList.begin() + (ptrdiff_t)i, replacement.begin(), replacement.end());
				out.memory = b.memory;
				out.offset = offset;
				out.size = req.size;
				out.block = (int32_t)bi;
				out.type = type;
				out.mapped = b.mapped ? static_cast<char*>(b.mapped) + offset : nullptr;
				return true;
			}
		}
		Block nb;
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = blockSize;
		ai.memoryTypeIndex = type;
		VkResult r = vkAllocateMemory(device, &ai, nullptr, &nb.memory);
		if (r == VK_SUCCESS)
		{
			nb.size = blockSize;
			nb.type = type;
			VkPhysicalDeviceMemoryProperties mem{};
			vkGetPhysicalDeviceMemoryProperties(physical, &mem);
			if (mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
				vkMapMemory(device, nb.memory, 0, VK_WHOLE_SIZE, 0, &nb.mapped);
			nb.freeList.push_back({0, blockSize});
			size_t slot = blocks_.size();
			for (size_t i = 0; i < blocks_.size(); ++i) if (blocks_[i].memory == VK_NULL_HANDLE) { slot = i; break; }
			if (slot == blocks_.size()) blocks_.push_back(nb); else blocks_[slot] = nb;
			return Allocate(req, flags, hostVisible, out);
		}
	}
	// Dedicated allocation
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = type;
	VkResult r = vkAllocateMemory(device, &ai, nullptr, &out.memory);
	if (r != VK_SUCCESS) { Error("vkAllocateMemory", r); return false; }
	out.offset = 0;
	out.size = req.size;
	out.block = -1;
	out.type = type;
	VkPhysicalDeviceMemoryProperties mem{};
	vkGetPhysicalDeviceMemoryProperties(physical, &mem);
	if (mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
		vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
	return true;
}

void Core::Free(Allocation& alloc)
{
	if (!alloc.memory) return;
	if (alloc.block < 0)
	{
		vkFreeMemory(device, alloc.memory, nullptr);
	}
	else if ((size_t)alloc.block < blocks_.size())
	{
		Block& b = blocks_[(size_t)alloc.block];
		// Return the range (alignment padding stays merged with neighbours).
		b.freeList.push_back({alloc.offset, alloc.size});
		std::sort(b.freeList.begin(), b.freeList.end(), [](const Block::Range& x, const Block::Range& y) { return x.offset < y.offset; });
		std::vector<Block::Range> merged;
		for (const auto& r : b.freeList)
		{
			if (!merged.empty() && merged.back().offset + merged.back().size >= r.offset)
				merged.back().size = std::max(merged.back().offset + merged.back().size, r.offset + r.size) - merged.back().offset;
			else merged.push_back(r);
		}
		b.freeList.swap(merged);
		if (b.freeList.size() == 1 && b.freeList[0].offset == 0 && b.freeList[0].size == b.size && blocks_.size() > 4)
		{
			vkFreeMemory(device, b.memory, nullptr);
			b = Block{};
		}
	}
	alloc = {};
}

bool Core::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, Buffer& out)
{
	out = {};
	VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = std::max<VkDeviceSize>(size, 4);
	bi.usage = usage;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkResult r = vkCreateBuffer(device, &bi, nullptr, &out.buffer);
	if (r != VK_SUCCESS) { Error("vkCreateBuffer", r); return false; }
	VkMemoryRequirements req{};
	vkGetBufferMemoryRequirements(device, out.buffer, &req);
	const VkMemoryPropertyFlags flags = hostVisible
		? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
		: VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if (!Allocate(req, flags, hostVisible, out.alloc))
	{
		vkDestroyBuffer(device, out.buffer, nullptr);
		out = {};
		return false;
	}
	vkBindBufferMemory(device, out.buffer, out.alloc.memory, out.alloc.offset);
	out.size = size;
	out.mapped = out.alloc.mapped;
	return true;
}

void Core::DestroyBuffer(Buffer& buffer, bool deferred)
{
	if (!buffer.buffer) { buffer = {}; return; }
	Buffer copy = buffer;
	buffer = {};
	auto fn = [this, copy]() mutable {
		vkDestroyBuffer(device, copy.buffer, nullptr);
		Free(copy.alloc);
	};
	if (deferred) Defer(fn); else fn();
}

bool Core::CreateStaging(const void* data, VkDeviceSize size, Buffer& out)
{
	if (!CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, out))
		return false;
	if (data && out.mapped) std::memcpy(out.mapped, data, (size_t)size);
	return true;
}

bool Core::CreateBufferWithData(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out)
{
	if (!CreateBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, out))
		return false;
	if (out.mapped)
	{
		// Unified memory (integrated GPU / software renderer) : no copy needed.
		std::memcpy(out.mapped, data, (size_t)size);
		return true;
	}
	Buffer staging;
	if (!CreateStaging(data, size, staging))
	{
		DestroyBuffer(out, false);
		return false;
	}
	VkCommandBuffer cmd = Cmd();
	if (onBeforeTransfer) onBeforeTransfer();
	VkBufferCopy copy{0, 0, size};
	vkCmdCopyBuffer(cmd, staging.buffer, out.buffer, 1, &copy);
	VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 1, &mb, 0, nullptr, 0, nullptr);
	DestroyBuffer(staging, true);
	return true;
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------
bool Core::CreateImage(const ImageDesc& desc, Image& out)
{
	out = {};
	const bool depth = desc.format == VK_FORMAT_D32_SFLOAT || desc.format == VK_FORMAT_D16_UNORM ||
		desc.format == VK_FORMAT_X8_D24_UNORM_PACK32 || desc.format == VK_FORMAT_D24_UNORM_S8_UINT ||
		desc.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
	VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.extent = {std::max(1u, desc.width), std::max(1u, desc.height), 1};
	ii.mipLevels = std::max(1u, desc.mips);
	ii.arrayLayers = desc.kind == ImageKind::Cube ? 6u : std::max(1u, desc.layers);
	ii.format = desc.format;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ii.samples = desc.samples;
	ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (desc.kind == ImageKind::Cube) ii.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (desc.renderTarget)
		ii.usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	VkResult r = vkCreateImage(device, &ii, nullptr, &out.image);
	if (r != VK_SUCCESS) { Error("vkCreateImage", r); return false; }
	VkMemoryRequirements req{};
	vkGetImageMemoryRequirements(device, out.image, &req);
	if (!Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, out.alloc))
	{
		vkDestroyImage(device, out.image, nullptr);
		out = {};
		return false;
	}
	vkBindImageMemory(device, out.image, out.alloc.memory, out.alloc.offset);
	out.format = desc.format;
	out.kind = desc.kind;
	out.width = ii.extent.width;
	out.height = ii.extent.height;
	out.layers = ii.arrayLayers;
	out.mips = ii.mipLevels;
	out.samples = desc.samples;
	out.usage = ii.usage;
	out.aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	out.serial = NextSerial();

	VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = out.image;
	vi.viewType = desc.kind == ImageKind::Cube ? VK_IMAGE_VIEW_TYPE_CUBE
		: desc.kind == ImageKind::Array2D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
	vi.format = desc.format;
	vi.subresourceRange = {out.aspect, 0, out.mips, 0, out.layers};
	r = vkCreateImageView(device, &vi, nullptr, &out.view);
	if (r != VK_SUCCESS) { Error("vkCreateImageView", r); DestroyImage(out, false); return false; }
	if (desc.renderTarget)
	{
		out.layerViews.resize(out.layers, VK_NULL_HANDLE);
		for (uint32_t l = 0; l < out.layers; ++l)
		{
			VkImageViewCreateInfo lv = vi;
			lv.viewType = VK_IMAGE_VIEW_TYPE_2D;
			lv.subresourceRange = {out.aspect, 0, 1, l, 1};
			vkCreateImageView(device, &lv, nullptr, &out.layerViews[l]);
		}
	}
	return true;
}

void Core::DestroyImage(Image& image, bool deferred)
{
	if (!image.image) { image = {}; return; }
	Image copy = image;
	image = {};
	auto fn = [this, copy]() mutable {
		for (VkImageView v : copy.layerViews) if (v) vkDestroyImageView(device, v, nullptr);
		if (copy.view) vkDestroyImageView(device, copy.view, nullptr);
		vkDestroyImage(device, copy.image, nullptr);
		Free(copy.alloc);
	};
	if (deferred) Defer(fn); else fn();
}

static void LayoutAccess(VkImageLayout layout, VkAccessFlags& access, VkPipelineStageFlags& stage, bool src)
{
	switch (layout)
	{
	case VK_IMAGE_LAYOUT_UNDEFINED:
		access = 0; stage = src ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; break;
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT; break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		access = VK_ACCESS_SHADER_READ_BIT;
		stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; break;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		access = VK_ACCESS_TRANSFER_READ_BIT; stage = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		access = VK_ACCESS_TRANSFER_WRITE_BIT; stage = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
	default:
		access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; break;
	}
}

void Core::Transition(VkCommandBuffer cmd, Image& image, VkImageLayout newLayout)
{
	if (!image.image) return;
	if (image.layout == newLayout &&
		newLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
		newLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
		newLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		return;
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.oldLayout = image.layout;
	b.newLayout = newLayout;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image.image;
	b.subresourceRange = {image.aspect, 0, image.mips, 0, image.layers};
	VkPipelineStageFlags srcStage = 0, dstStage = 0;
	LayoutAccess(image.layout, b.srcAccessMask, srcStage, true);
	LayoutAccess(newLayout, b.dstAccessMask, dstStage, false);
	if (image.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) b.srcAccessMask = 0; // read -> X : execution dependency only
	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
	image.layout = newLayout;
}

bool Core::UploadImage(Image& image, uint32_t layer, const void* pixels, size_t size, uint32_t width, uint32_t height)
{
	Buffer staging;
	if (!CreateStaging(pixels, size, staging)) return false;
	if (onBeforeTransfer) onBeforeTransfer();
	VkCommandBuffer cmd = Cmd();
	Transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy region{};
	region.imageSubresource = {image.aspect, 0, layer, 1};
	region.imageExtent = {width, height, 1};
	vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	Transition(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	DestroyBuffer(staging, true);
	return true;
}

void Core::GenerateMips(Image& image)
{
	if (image.mips <= 1) return;
	if (onBeforeTransfer) onBeforeTransfer();
	VkCommandBuffer cmd = Cmd();
	// Level 0 -> TRANSFER_SRC, others TRANSFER_DST, then blit down the chain.
	Transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	int32_t w = (int32_t)image.width, h = (int32_t)image.height;
	for (uint32_t level = 1; level < image.mips; ++level)
	{
		VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image.image;
		b.subresourceRange = {image.aspect, level - 1, 1, 0, image.layers};
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
		const int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
		VkImageBlit blit{};
		blit.srcSubresource = {image.aspect, level - 1, 0, image.layers};
		blit.srcOffsets[1] = {w, h, 1};
		blit.dstSubresource = {image.aspect, level, 0, image.layers};
		blit.dstOffsets[1] = {nw, nh, 1};
		vkCmdBlitImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
		w = nw; h = nh;
	}
	// Bring every level to SHADER_READ_ONLY.
	VkImageMemoryBarrier b[2]{};
	for (auto& x : b)
	{
		x = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		x.image = image.image;
		x.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		x.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	}
	b[0].subresourceRange = {image.aspect, 0, image.mips - 1, 0, image.layers};
	b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b[1].subresourceRange = {image.aspect, image.mips - 1, 1, 0, image.layers};
	b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
	image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkSampler Core::GetSampler(const SamplerState& state)
{
	for (const auto& s : samplers_)
		if (s.state == state) return s.sampler;
	VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = state.magFilter;
	si.minFilter = state.minFilter;
	si.mipmapMode = state.mipmapMode;
	si.addressModeU = state.wrapU;
	si.addressModeV = state.wrapV;
	si.addressModeW = state.wrapW;
	si.borderColor = state.borderWhite ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	si.minLod = 0.f;
	// GL_NEAREST / GL_LINEAR minification never reads the mip chain.
	si.maxLod = state.useMips ? VK_LOD_CLAMP_NONE : 0.25f;
	if (state.anisotropy > 1.f && enabledFeatures.samplerAnisotropy)
	{
		si.anisotropyEnable = VK_TRUE;
		si.maxAnisotropy = std::min(state.anisotropy, properties.limits.maxSamplerAnisotropy);
	}
	VkSampler sampler = VK_NULL_HANDLE;
	VkResult r = vkCreateSampler(device, &si, nullptr, &sampler);
	if (r != VK_SUCCESS) { Error("vkCreateSampler", r); return VK_NULL_HANDLE; }
	samplers_.push_back({state, sampler});
	return sampler;
}
} // namespace hrlvk
