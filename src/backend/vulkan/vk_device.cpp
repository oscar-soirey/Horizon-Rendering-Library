// HRL native Vulkan backend - device layer implementation.
#include "vk_device.h"

#include "../../core/backend_api.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace hrlvk
{
namespace
{
Device g_device;

constexpr VkDeviceSize kFrameLinearSize = 4ull << 20;
constexpr VkDeviceSize kMaxPendingStaging = 256ull << 20;

const char* ResultName(VkResult r)
{
	switch (r)
	{
	case VK_SUCCESS: return "VK_SUCCESS";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
	case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
	case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
	default: return "VkResult";
	}
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
	const std::string msg = std::string("Vulkan validation: ") + (data && data->pMessage ? data->pMessage : "");
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, msg);
	else
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_WARNING, msg);
	return VK_FALSE;
}

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name)
{
	for (const auto& e : list)
		if (std::strcmp(e.extensionName, name) == 0) return true;
	return false;
}

struct LayoutInfo { VkPipelineStageFlags2 stage; VkAccessFlags2 access; };

LayoutInfo InfoForLayout(VkImageLayout layout)
{
	switch (layout)
	{
	case VK_IMAGE_LAYOUT_UNDEFINED:
		return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT};
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT};
	case VK_IMAGE_LAYOUT_GENERAL:
		return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
	case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
		return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
	default:
		return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
	}
}
} // namespace

Device& GetDevice() { return g_device; }

void ReportError(const char* what, VkResult result)
{
	SetErrorCode(result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY
		? HRL_OUT_OF_MEMORY : HRL_INVALID_BACKEND_OPERATION,
		HRL_SEVERITY_ERROR, std::string("Vulkan: ") + what + " failed (" + ResultName(result) + ")");
}
void ReportError(const std::string& what)
{
	SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "Vulkan: " + what);
}
void ReportWarning(const std::string& what)
{
	SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_WARNING, "Vulkan: " + what);
}

// ---------------------------------------------------------------------------
// Instance / device
// ---------------------------------------------------------------------------
bool Device::CreateInstance()
{
	uint32_t apiVersion = VK_API_VERSION_1_0;
	vkEnumerateInstanceVersion(&apiVersion);
	if (apiVersion < VK_API_VERSION_1_3)
	{
		ReportError("the installed Vulkan loader does not support Vulkan 1.3; update the GPU driver");
		return false;
	}

	uint32_t count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> available(count);
	vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

	std::vector<const char*> extensions;
	for (const auto& e : instanceExtensions) extensions.push_back(e.c_str());
	debugUtils_ = HasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	if (debugUtils_) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	std::vector<const char*> layers;
	const char* validation = std::getenv("HRL_VULKAN_VALIDATION");
	const bool wantValidation = validation && validation[0] && validation[0] != '0';
	if (wantValidation)
	{
		uint32_t layerCount = 0;
		vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
		std::vector<VkLayerProperties> layerList(layerCount);
		vkEnumerateInstanceLayerProperties(&layerCount, layerList.data());
		bool found = false;
		for (const auto& l : layerList)
			if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) found = true;
		if (found) layers.push_back("VK_LAYER_KHRONOS_validation");
		else ReportWarning("HRL_VULKAN_VALIDATION is set but VK_LAYER_KHRONOS_validation is not installed");
	}

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "HRL application";
	app.pEngineName = "Horizon Rendering Library";
	app.apiVersion = VK_API_VERSION_1_3;

	VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ci.pApplicationInfo = &app;
	ci.enabledExtensionCount = (uint32_t)extensions.size();
	ci.ppEnabledExtensionNames = extensions.data();
	ci.enabledLayerCount = (uint32_t)layers.size();
	ci.ppEnabledLayerNames = layers.data();
	VkResult r = vkCreateInstance(&ci, nullptr, &instance);
	if (r != VK_SUCCESS) { ReportError("vkCreateInstance", r); return false; }

	if (debugUtils_ && !layers.empty())
	{
		VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
		mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		mi.pfnUserCallback = DebugCallback;
		auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
		if (create) create(instance, &mi, nullptr, &messenger_);
	}
	return true;
}

bool Device::PickPhysicalDevice()
{
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(instance, &count, devices.data());

	int bestScore = -1;
	std::string rejected;
	for (VkPhysicalDevice pd : devices)
	{
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(pd, &props);
		if (props.apiVersion < VK_API_VERSION_1_3)
		{
			rejected += std::string(props.deviceName) + " (Vulkan < 1.3); ";
			continue;
		}

		VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
		VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
		VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
		VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
		vkGetPhysicalDeviceFeatures2(pd, &f2);
		const bool required = f13.dynamicRendering && f13.synchronization2 &&
			f12.runtimeDescriptorArray && f12.descriptorBindingPartiallyBound &&
			f12.shaderSampledImageArrayNonUniformIndexing && f12.descriptorBindingSampledImageUpdateAfterBind &&
			f12.descriptorBindingUpdateUnusedWhilePending && f12.drawIndirectCount &&
			f11.shaderDrawParameters && f2.features.multiDrawIndirect && f2.features.drawIndirectFirstInstance &&
			f2.features.independentBlend;
		if (!required)
		{
			rejected += std::string(props.deviceName) + " (missing required features); ";
			continue;
		}

		uint32_t qCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(qCount);
		vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, families.data());
		int family = -1;
		for (uint32_t i = 0; i < qCount; ++i)
		{
			if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
				continue;
			if (surface != VK_NULL_HANDLE)
			{
				VkBool32 present = VK_FALSE;
				vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
				if (!present) continue;
			}
			family = (int)i;
			break;
		}
		if (family < 0)
		{
			rejected += std::string(props.deviceName) + " (no graphics+present queue); ";
			continue;
		}

		int score = 1;
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
		else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 100;
		if (score > bestScore)
		{
			bestScore = score;
			physical = pd;
			queueFamily = (uint32_t)family;
		}
	}
	if (physical == VK_NULL_HANDLE)
	{
		ReportError("no suitable GPU found. Rejected: " + (rejected.empty() ? std::string("none listed") : rejected));
		return false;
	}
	return true;
}

bool Device::CreateDevice()
{
	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> available(count);
	vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data());

	deviceExtensions_.clear();
	if (surface != VK_NULL_HANDLE) deviceExtensions_.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

	const char* noRt = std::getenv("HRL_VULKAN_NO_RAYTRACING");
	const bool rtAllowed = !(noRt && noRt[0] && noRt[0] != '0');
	bool rt = rtAllowed &&
		HasExtension(available, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
		HasExtension(available, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
		HasExtension(available, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

	VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
	VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rq};
	VkPhysicalDeviceVulkan13Features s13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	VkPhysicalDeviceVulkan12Features s12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &s13};
	VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &s12};
	if (rt) s13.pNext = &as;
	vkGetPhysicalDeviceFeatures2(physical, &supported);
	rt = rt && as.accelerationStructure && rq.rayQuery && s12.bufferDeviceAddress;

	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(physical, &props);
	VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
	VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
	vkGetPhysicalDeviceProperties2(physical, &p2);

	caps.deviceName = props.deviceName;
	caps.rayQuery = rt;
	caps.anisotropy = supported.features.samplerAnisotropy == VK_TRUE;
	caps.maxAnisotropy = caps.anisotropy ? props.limits.maxSamplerAnisotropy : 1.f;
	caps.maxBindlessTextures = std::min<uint32_t>(16384u,
		std::min(p12.maxPerStageDescriptorUpdateAfterBindSamplers, p12.maxPerStageDescriptorUpdateAfterBindSampledImages) / 2u);

	// Accurate memory usage / budget figures (HRL_Vulkan_GetMemoryUsage).
	const bool memoryBudget = HasExtension(available, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	if (memoryBudget) deviceExtensions_.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	if (rt)
	{
		deviceExtensions_.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
		deviceExtensions_.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
		deviceExtensions_.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
	}

	// Enabled features: only what the renderer uses.
	VkPhysicalDeviceRayQueryFeaturesKHR erq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
	erq.rayQuery = VK_TRUE;
	VkPhysicalDeviceAccelerationStructureFeaturesKHR eas{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &erq};
	eas.accelerationStructure = VK_TRUE;

	VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	e13.dynamicRendering = VK_TRUE;
	e13.synchronization2 = VK_TRUE;
	e13.maintenance4 = s13.maintenance4;
	if (rt) e13.pNext = &eas;
	VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13};
	e12.descriptorIndexing = VK_TRUE;
	e12.runtimeDescriptorArray = VK_TRUE;
	e12.descriptorBindingPartiallyBound = VK_TRUE;
	e12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	e12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
	e12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
	e12.drawIndirectCount = VK_TRUE; // GPU culling
	e12.bufferDeviceAddress = rt ? VK_TRUE : VK_FALSE;
	VkPhysicalDeviceVulkan11Features e11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &e12};
	e11.shaderDrawParameters = VK_TRUE;
	VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e11};
	enabled.features.multiDrawIndirect = VK_TRUE;
	enabled.features.drawIndirectFirstInstance = VK_TRUE;
	enabled.features.independentBlend = VK_TRUE; // colour blends, object ids do not
	enabled.features.samplerAnisotropy = supported.features.samplerAnisotropy;
	enabled.features.depthClamp = supported.features.depthClamp;
	enabled.features.depthBiasClamp = supported.features.depthBiasClamp;

	const float priority = 1.f;
	VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qi.queueFamilyIndex = queueFamily;
	qi.queueCount = 1;
	qi.pQueuePriorities = &priority;

	VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enabled};
	ci.queueCreateInfoCount = 1;
	ci.pQueueCreateInfos = &qi;
	ci.enabledExtensionCount = (uint32_t)deviceExtensions_.size();
	ci.ppEnabledExtensionNames = deviceExtensions_.data();
	VkResult r = vkCreateDevice(physical, &ci, nullptr, &device);
	if (r != VK_SUCCESS) { ReportError("vkCreateDevice", r); return false; }
	vkGetDeviceQueue(device, queueFamily, 0, &queue);

	VmaAllocatorCreateInfo ai{};
	ai.physicalDevice = physical;
	ai.device = device;
	ai.instance = instance;
	ai.vulkanApiVersion = VK_API_VERSION_1_3;
	if (rt) ai.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (memoryBudget) ai.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	r = vmaCreateAllocator(&ai, &allocator);
	if (r != VK_SUCCESS) { ReportError("vmaCreateAllocator", r); return false; }
	return true;
}

bool Device::CreateFrames()
{
	for (Frame& f : frames_)
	{
		VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pi.queueFamilyIndex = queueFamily;
		if (vkCreateCommandPool(device, &pi, nullptr, &f.pool) != VK_SUCCESS) return false;
		VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = f.pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(device, &ai, &f.cmd) != VK_SUCCESS) return false;

		VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if (vkCreateFence(device, &fi, nullptr, &f.fence) != VK_SUCCESS) return false;
		VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		if (vkCreateSemaphore(device, &si, nullptr, &f.acquired) != VK_SUCCESS) return false;

		const VkDescriptorPoolSize sizes[] = {
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048},
			{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
			{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 64},
		};
		VkDescriptorPoolCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		di.maxSets = 2048;
		di.poolSizeCount = caps.rayQuery ? 5u : 4u;
		di.pPoolSizes = sizes;
		if (vkCreateDescriptorPool(device, &di, nullptr, &f.descriptorPool) != VK_SUCCESS) return false;

		if (!CreateBuffer(kFrameLinearSize,
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, f.linear, "frame linear"))
			return false;
	}

	VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pi.queueFamilyIndex = queueFamily;
	pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	if (vkCreateCommandPool(device, &pi, nullptr, &uploadPool_) != VK_SUCCESS) return false;
	VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	return vkCreateFence(device, &fi, nullptr, &uploadFence_) == VK_SUCCESS;
}

bool Device::Init(uint32_t width, uint32_t height)
{
	if (IsInitialized()) return true;
	wantedWidth_ = width;
	wantedHeight_ = height;
	if (!CreateInstance()) { Shutdown(); return false; }
	if (surfaceCallback)
	{
		const int r = surfaceCallback(instance, &surface, surfaceUser);
		if (r != VK_SUCCESS || surface == VK_NULL_HANDLE)
		{
			ReportError("the surface callback failed", (VkResult)r);
			Shutdown();
			return false;
		}
	}
	if (!PickPhysicalDevice() || !CreateDevice() || !CreateFrames()) { Shutdown(); return false; }
	if (surface != VK_NULL_HANDLE && !CreateSwapchain(width, height)) { Shutdown(); return false; }
	return true;
}

void Device::RunDeletions(uint64_t completedFrame, bool all)
{
	// A destructor may defer further deletions: work on a detached list.
	std::vector<std::pair<uint64_t, std::function<void()>>> pending;
	pending.swap(deletions_);
	for (auto& entry : pending)
	{
		if (all || entry.first <= completedFrame) entry.second();
		else deletions_.push_back(std::move(entry));
	}
}

void Device::Shutdown()
{
	if (device != VK_NULL_HANDLE)
	{
		vkDeviceWaitIdle(device);
		for (auto& fn : uploadDeletions_) fn();
		uploadDeletions_.clear();
		while (!deletions_.empty()) RunDeletions(0, true);
		DestroySwapchain();
		for (Frame& f : frames_)
		{
			DestroyBuffer(f.linear, false);
			if (f.descriptorPool) vkDestroyDescriptorPool(device, f.descriptorPool, nullptr);
			if (f.acquired) vkDestroySemaphore(device, f.acquired, nullptr);
			if (f.fence) vkDestroyFence(device, f.fence, nullptr);
			if (f.pool) vkDestroyCommandPool(device, f.pool, nullptr);
			f = Frame{};
		}
		if (uploadFence_) vkDestroyFence(device, uploadFence_, nullptr);
		if (uploadPool_) vkDestroyCommandPool(device, uploadPool_, nullptr);
		uploadFence_ = VK_NULL_HANDLE;
		uploadPool_ = VK_NULL_HANDLE;
		uploadCmd_ = VK_NULL_HANDLE;
		if (allocator) vmaDestroyAllocator(allocator);
		allocator = VK_NULL_HANDLE;
		vkDestroyDevice(device, nullptr);
		device = VK_NULL_HANDLE;
	}
	if (instance != VK_NULL_HANDLE)
	{
		if (surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance, surface, nullptr);
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
	queue = VK_NULL_HANDLE;
	inFrame_ = false;
	frame_ = 0;
}

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------
bool Device::CreateSwapchain(uint32_t width, uint32_t height)
{
	VkSurfaceCapabilitiesKHR sc{};
	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &sc) != VK_SUCCESS) return false;

	uint32_t count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(count);
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data());
	if (formats.empty()) { ReportError("the surface reports no format"); return false; }
	VkSurfaceFormatKHR chosen = formats[0];
	for (const auto& f : formats)
		if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
			f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }

	VkExtent2D extent = sc.currentExtent;
	if (extent.width == UINT32_MAX)
	{
		extent.width = std::clamp(width, sc.minImageExtent.width, sc.maxImageExtent.width);
		extent.height = std::clamp(height, sc.minImageExtent.height, sc.maxImageExtent.height);
	}
	if (extent.width == 0 || extent.height == 0)
	{
		// Minimized window: keep rendering off, try again on the next resize.
		swapchainDirty_ = true;
		return true;
	}

	uint32_t imageCount = sc.minImageCount + 1;
	if (sc.maxImageCount > 0 && imageCount > sc.maxImageCount) imageCount = sc.maxImageCount;

	VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	ci.surface = surface;
	ci.minImageCount = imageCount;
	ci.imageFormat = chosen.format;
	ci.imageColorSpace = chosen.colorSpace;
	ci.imageExtent = extent;
	ci.imageArrayLayers = 1;
	// The scenes are copied to the swapchain with a blit.
	if (!(sc.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
	{
		ReportError("the surface does not support VK_IMAGE_USAGE_TRANSFER_DST_BIT");
		return false;
	}
	ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.preTransform = sc.currentTransform;
	ci.compositeAlpha = (sc.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
		? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	// FIFO is always available. Without vertical synchronisation, MAILBOX
	// (no tearing) is preferred to IMMEDIATE.
	ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	if (!vsync_)
	{
		uint32_t modeCount = 0;
		vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, nullptr);
		std::vector<VkPresentModeKHR> modes(modeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, modes.data());
		for (VkPresentModeKHR wanted : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
			if (std::find(modes.begin(), modes.end(), wanted) != modes.end()) { ci.presentMode = wanted; break; }
	}
	ci.clipped = VK_TRUE;
	ci.oldSwapchain = swapchain;
	VkSwapchainKHR created = VK_NULL_HANDLE;
	VkResult r = vkCreateSwapchainKHR(device, &ci, nullptr, &created);
	if (r != VK_SUCCESS) { ReportError("vkCreateSwapchainKHR", r); return false; }
	DestroySwapchain();
	swapchain = created;
	swapchainFormat = chosen.format;
	swapchainExtent = extent;

	vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
	std::vector<VkImage> images(count);
	vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
	swapchainImages_.resize(count);
	presentReady_.resize(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		Image& img = swapchainImages_[i];
		img = Image{};
		img.handle = images[i];
		img.format = chosen.format;
		img.extent = extent;
		img.view = CreateView(img, chosen.format, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1);
		VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		vkCreateSemaphore(device, &si, nullptr, &presentReady_[i]);
	}
	swapchainDirty_ = false;
	return true;
}

void Device::DestroySwapchain()
{
	if (device == VK_NULL_HANDLE) return;
	for (Image& img : swapchainImages_)
		if (img.view) vkDestroyImageView(device, img.view, nullptr);
	for (VkSemaphore s : presentReady_)
		if (s) vkDestroySemaphore(device, s, nullptr);
	swapchainImages_.clear();
	presentReady_.clear();
	if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
	swapchain = VK_NULL_HANDLE;
	backbufferValid_ = false;
}

void Device::SetVSync(bool enabled)
{
	if (vsync_ == enabled) return;
	vsync_ = enabled;
	swapchainDirty_ = true;
}

void Device::Resize(uint32_t width, uint32_t height)
{
	wantedWidth_ = width;
	wantedHeight_ = height;
	swapchainDirty_ = true;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------
bool Device::BeginFrame()
{
	if (!IsInitialized()) return false;
	if (inFrame_) return true;

	Frame& f = frames_[frame_];
	if (f.submitted)
	{
		vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
		f.submitted = false;
	}
	// Every frame up to frameNumber_ - kFramesInFlight has now completed.
	if (frameNumber_ >= kFramesInFlight) RunDeletions(frameNumber_ - kFramesInFlight, false);
	vkResetCommandPool(device, f.pool, 0);
	vkResetDescriptorPool(device, f.descriptorPool, 0);
	f.linearOffset = 0;

	backbufferValid_ = false;
	if (surface != VK_NULL_HANDLE)
	{
		if (swapchainDirty_ || swapchain == VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(device);
			CreateSwapchain(wantedWidth_, wantedHeight_);
		}
		if (swapchain != VK_NULL_HANDLE)
		{
			VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imageIndex_);
			if (r == VK_ERROR_OUT_OF_DATE_KHR)
			{
				swapchainDirty_ = true; // recreated at the next frame
			}
			else if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
			{
				backbufferValid_ = true;
				swapchainImages_[imageIndex_].layout = VK_IMAGE_LAYOUT_UNDEFINED;
				if (r == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true;
			}
			else ReportError("vkAcquireNextImageKHR", r);
		}
	}

	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(f.cmd, &bi);
	inFrame_ = true;
	return true;
}

VkCommandBuffer Device::TakeUploadCmd()
{
	VkCommandBuffer cmd = uploadCmd_;
	if (cmd != VK_NULL_HANDLE)
	{
		// Make every upload visible to whatever reads it in later command buffers.
		VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
		mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
		VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.memoryBarrierCount = 1;
		di.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2(cmd, &di);
		vkEndCommandBuffer(cmd);
	}
	uploadCmd_ = VK_NULL_HANDLE;
	pendingStagingBytes_ = 0;
	return cmd;
}

void Device::EndFrame()
{
	if (!inFrame_) return;
	Frame& f = frames_[frame_];

	Image* back = Backbuffer();
	if (back)
	{
		if (back->layout == VK_IMAGE_LAYOUT_UNDEFINED)
		{
			// Nothing was rendered on screen this frame: present black.
			Transition(f.cmd, *back, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			VkClearColorValue black{};
			VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdClearColorImage(f.cmd, back->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		}
		Transition(f.cmd, *back, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	}
	vkEndCommandBuffer(f.cmd);

	VkCommandBuffer upload = TakeUploadCmd();
	VkCommandBufferSubmitInfo cmds[2]{};
	uint32_t cmdCount = 0;
	if (upload)
	{
		cmds[cmdCount] = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
		cmds[cmdCount++].commandBuffer = upload;
		VkCommandPool pool = uploadPool_;
		VkDevice dev = device;
		deletions_.emplace_back(frameNumber_, [dev, pool, upload]() { vkFreeCommandBuffers(dev, pool, 1, &upload); });
		for (auto& fn : uploadDeletions_) deletions_.emplace_back(frameNumber_, std::move(fn));
		uploadDeletions_.clear();
	}
	cmds[cmdCount] = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
	cmds[cmdCount++].commandBuffer = f.cmd;

	VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	wait.semaphore = f.acquired;
	wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	signal.semaphore = back ? presentReady_[imageIndex_] : VK_NULL_HANDLE;
	signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

	VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	si.commandBufferInfoCount = cmdCount;
	si.pCommandBufferInfos = cmds;
	if (back)
	{
		si.waitSemaphoreInfoCount = 1;
		si.pWaitSemaphoreInfos = &wait;
		si.signalSemaphoreInfoCount = 1;
		si.pSignalSemaphoreInfos = &signal;
	}
	vkResetFences(device, 1, &f.fence);
	VkResult r = vkQueueSubmit2(queue, 1, &si, f.fence);
	if (r != VK_SUCCESS) ReportError("vkQueueSubmit2", r);
	else f.submitted = true;

	if (back && r == VK_SUCCESS)
	{
		VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &presentReady_[imageIndex_];
		pi.swapchainCount = 1;
		pi.pSwapchains = &swapchain;
		pi.pImageIndices = &imageIndex_;
		r = vkQueuePresentKHR(queue, &pi);
		if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true;
		else if (r != VK_SUCCESS) ReportError("vkQueuePresentKHR", r);
	}

	backbufferValid_ = false;
	inFrame_ = false;
	frame_ = (frame_ + 1) % kFramesInFlight;
	++frameNumber_;
}

FrameSlice Device::AllocFrame(VkDeviceSize size, VkDeviceSize alignment)
{
	Frame& f = frames_[frame_];
	if (size == 0) size = alignment;
	VkDeviceSize offset = (f.linearOffset + alignment - 1) / alignment * alignment;
	if (offset + size > f.linear.size)
	{
		// Grow: the previous buffer stays alive until this frame slot is reused.
		Buffer old = f.linear;
		Defer([this, old]() mutable { DestroyBuffer(old, false); });
		f.linear = Buffer{};
		const VkDeviceSize newSize = std::max(old.size * 2, size * 2);
		if (!CreateBuffer(newSize,
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, f.linear, "frame linear"))
			return {};
		offset = 0;
	}
	f.linearOffset = offset + size;
	return {f.linear.handle, offset, size, static_cast<char*>(f.linear.mapped) + offset};
}

VkCommandBuffer Device::UploadCmd()
{
	if (uploadCmd_ == VK_NULL_HANDLE)
	{
		VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = uploadPool_;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(device, &ai, &uploadCmd_) != VK_SUCCESS) return VK_NULL_HANDLE;
		VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(uploadCmd_, &bi);
		// Uploads write into buffers that earlier frames read (geometry pool):
		// order them after everything already submitted. Once per batch.
		VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		mb.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
		mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
		VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.memoryBarrierCount = 1;
		di.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2(uploadCmd_, &di);
	}
	return uploadCmd_;
}

void Device::FlushUploads(bool wait)
{
	VkCommandBuffer cmd = TakeUploadCmd();
	if (cmd == VK_NULL_HANDLE) return;
	VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
	ci.commandBuffer = cmd;
	VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	si.commandBufferInfoCount = 1;
	si.pCommandBufferInfos = &ci;
	vkResetFences(device, 1, &uploadFence_);
	VkResult r = vkQueueSubmit2(queue, 1, &si, uploadFence_);
	if (r != VK_SUCCESS) { ReportError("vkQueueSubmit2 (uploads)", r); return; }
	(void)wait; // the staging buffers are released right after, so always wait
	vkWaitForFences(device, 1, &uploadFence_, VK_TRUE, UINT64_MAX);
	vkFreeCommandBuffers(device, uploadPool_, 1, &cmd);
	for (auto& fn : uploadDeletions_) fn();
	uploadDeletions_.clear();
}

void Device::WaitIdle()
{
	if (!IsInitialized()) return;
	FlushUploads(true);
	vkQueueWaitIdle(queue);
}

void Device::Defer(std::function<void()> fn)
{
	// Outside a frame the resource can still be referenced by the pending
	// upload commands, which are submitted with frame frameNumber_.
	deletions_.emplace_back(frameNumber_, std::move(fn));
}

void Device::MemoryUsage(uint64_t& usedBytes, uint64_t& budgetBytes)
{
	usedBytes = budgetBytes = 0;
	if (!allocator) return;
	VkPhysicalDeviceMemoryProperties props{};
	vkGetPhysicalDeviceMemoryProperties(physical, &props);
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
	vmaSetCurrentFrameIndex(allocator, (uint32_t)frameNumber_);
	vmaGetHeapBudgets(allocator, budgets);
	// GPU memory: the device local heaps (on integrated GPUs, shared system memory).
	for (uint32_t i = 0; i < props.memoryHeapCount; ++i)
		if (props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
		{
			usedBytes += budgets[i].usage;
			budgetBytes += budgets[i].budget;
		}
}

void Device::DrainDeletions()
{
	if (!IsInitialized()) return;
	WaitIdle();
	vkDeviceWaitIdle(device);
	while (!deletions_.empty()) RunDeletions(0, true);
}

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------
bool Device::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, Buffer& out, const char* name)
{
	VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = std::max<VkDeviceSize>(size, 16);
	bi.usage = usage;
	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	if (hostVisible)
		ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo info{};
	VkResult r = vmaCreateBuffer(allocator, &bi, &ai, &out.handle, &out.alloc, &info);
	if (r != VK_SUCCESS) { ReportError("buffer allocation", r); out = Buffer{}; return false; }
	out.size = bi.size;
	out.mapped = info.pMappedData;
	out.address = 0;
	if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
	{
		VkBufferDeviceAddressInfo di{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
		di.buffer = out.handle;
		out.address = vkGetBufferDeviceAddress(device, &di);
	}
	if (name) SetName(VK_OBJECT_TYPE_BUFFER, (uint64_t)out.handle, name);
	return true;
}

void Device::DestroyBuffer(Buffer& buffer, bool deferred)
{
	if (!buffer.handle) return;
	VmaAllocator a = allocator;
	VkBuffer h = buffer.handle;
	VmaAllocation al = buffer.alloc;
	if (deferred) Defer([a, h, al]() { vmaDestroyBuffer(a, h, al); });
	else vmaDestroyBuffer(a, h, al);
	buffer = Buffer{};
}

bool Device::UploadToBuffer(const Buffer& dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size)
{
	if (size == 0) return true;
	Buffer staging;
	if (!CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging)) return false;
	std::memcpy(staging.mapped, data, (size_t)size);
	VkCommandBuffer cmd = UploadCmd();
	VkBufferCopy copy{0, dstOffset, size};
	vkCmdCopyBuffer(cmd, staging.handle, dst.handle, 1, &copy);
	VmaAllocator a = allocator;
	uploadDeletions_.push_back([a, staging]() { vmaDestroyBuffer(a, staging.handle, staging.alloc); });
	pendingStagingBytes_ += size;
	if (pendingStagingBytes_ > kMaxPendingStaging && !inFrame_) FlushUploads(true);
	return true;
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------
VkImageView Device::CreateView(const Image& image, VkFormat format, VkImageViewType type,
	uint32_t baseMip, uint32_t mipCount, uint32_t baseLayer, uint32_t layerCount)
{
	VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = image.handle;
	vi.viewType = type;
	vi.format = format;
	vi.subresourceRange = {image.aspect, baseMip, mipCount, baseLayer, layerCount};
	VkImageView view = VK_NULL_HANDLE;
	VkResult r = vkCreateImageView(device, &vi, nullptr, &view);
	if (r != VK_SUCCESS) ReportError("vkCreateImageView", r);
	return view;
}

bool Device::CreateImage(const ImageDesc& desc, Image& out, const char* name)
{
	const bool depth = desc.format == VK_FORMAT_D32_SFLOAT || desc.format == VK_FORMAT_D16_UNORM;
	VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = desc.format;
	ci.extent = {std::max(desc.width, 1u), std::max(desc.height, 1u), 1};
	ci.mipLevels = std::max(desc.mips, 1u);
	ci.arrayLayers = desc.cube ? 6u : std::max(desc.layers, 1u);
	ci.samples = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = desc.usage;
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (desc.cube) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	if (desc.mutableFormat) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	out = Image{};
	VkResult r = vmaCreateImage(allocator, &ci, &ai, &out.handle, &out.alloc, nullptr);
	if (r != VK_SUCCESS) { ReportError("image allocation", r); out = Image{}; return false; }
	out.format = desc.format;
	out.extent = {ci.extent.width, ci.extent.height};
	out.mips = ci.mipLevels;
	out.layers = ci.arrayLayers;
	out.aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageViewType type = desc.cube ? VK_IMAGE_VIEW_TYPE_CUBE
		: ((out.layers > 1 || desc.arrayView) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
	out.view = CreateView(out, desc.format, type, 0, out.mips, 0, out.layers);
	if (!out.view) { DestroyImage(out, false); return false; }
	if (name) SetName(VK_OBJECT_TYPE_IMAGE, (uint64_t)out.handle, name);
	return true;
}

void Device::DestroyImage(Image& image, bool deferred)
{
	if (!image.handle) return;
	VmaAllocator a = allocator;
	VkDevice d = device;
	VkImage h = image.handle;
	VkImageView v = image.view;
	VmaAllocation al = image.alloc;
	auto destroy = [a, d, h, v, al]()
	{
		if (v) vkDestroyImageView(d, v, nullptr);
		vmaDestroyImage(a, h, al);
	};
	if (deferred) Defer(destroy); else destroy();
	image = Image{};
}

void Device::Transition(VkCommandBuffer cmd, Image& image, VkImageLayout newLayout)
{
	if (image.layout == newLayout) return;
	const LayoutInfo src = InfoForLayout(image.layout);
	const LayoutInfo dst = InfoForLayout(newLayout);
	VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
	b.srcStageMask = src.stage;
	b.srcAccessMask = src.access;
	b.dstStageMask = dst.stage;
	b.dstAccessMask = dst.access;
	b.oldLayout = image.layout;
	b.newLayout = newLayout;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image.handle;
	b.subresourceRange = {image.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
	// A present-source image acquired from the swapchain has no pending access,
	// but the acquire semaphore is waited at ALL_COMMANDS: chain on it.
	if (image.layout == VK_IMAGE_LAYOUT_UNDEFINED || image.layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
		b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	if (newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
		b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.imageMemoryBarrierCount = 1;
	di.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cmd, &di);
	image.layout = newLayout;
}

bool Device::UploadToImage(Image& image, uint32_t layer, const void* pixels, VkDeviceSize size)
{
	Buffer staging;
	if (!CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging)) return false;
	std::memcpy(staging.mapped, pixels, (size_t)size);
	VkCommandBuffer cmd = UploadCmd();
	Transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy region{};
	region.imageSubresource = {image.aspect, 0, layer, 1};
	region.imageExtent = {image.extent.width, image.extent.height, 1};
	vkCmdCopyBufferToImage(cmd, staging.handle, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	VmaAllocator a = allocator;
	uploadDeletions_.push_back([a, staging]() { vmaDestroyBuffer(a, staging.handle, staging.alloc); });
	pendingStagingBytes_ += size;
	return true;
}

void Device::GenerateMips(VkCommandBuffer cmd, Image& image)
{
	// Expects every mip in TRANSFER_DST (state after UploadToImage).
	auto barrier = [&](uint32_t mip, VkImageLayout from, VkImageLayout to)
	{
		const LayoutInfo src = InfoForLayout(from);
		const LayoutInfo dst = InfoForLayout(to);
		VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		b.srcStageMask = src.stage; b.srcAccessMask = src.access;
		b.dstStageMask = dst.stage; b.dstAccessMask = dst.access;
		b.oldLayout = from; b.newLayout = to;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image.handle;
		b.subresourceRange = {image.aspect, mip, 1, 0, image.layers};
		VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.imageMemoryBarrierCount = 1;
		di.pImageMemoryBarriers = &b;
		vkCmdPipelineBarrier2(cmd, &di);
	};
	int32_t w = (int32_t)image.extent.width, h = (int32_t)image.extent.height;
	for (uint32_t mip = 1; mip < image.mips; ++mip)
	{
		barrier(mip - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		const int32_t nw = std::max(w / 2, 1), nh = std::max(h / 2, 1);
		VkImageBlit blit{};
		blit.srcSubresource = {image.aspect, mip - 1, 0, image.layers};
		blit.srcOffsets[1] = {w, h, 1};
		blit.dstSubresource = {image.aspect, mip, 0, image.layers};
		blit.dstOffsets[1] = {nw, nh, 1};
		vkCmdBlitImage(cmd, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
		barrier(mip - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		w = nw; h = nh;
	}
	barrier(image.mips - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// ---------------------------------------------------------------------------
// Debug names / labels (RenderDoc, validation messages)
// ---------------------------------------------------------------------------
void Device::SetName(VkObjectType type, uint64_t handle, const char* name)
{
	if (!debugUtils_ || !name) return;
	static PFN_vkSetDebugUtilsObjectNameEXT fn = nullptr;
	if (!fn) fn = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT");
	if (!fn) return;
	VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
	ni.objectType = type;
	ni.objectHandle = handle;
	ni.pObjectName = name;
	fn(device, &ni);
}

void Device::BeginLabel(VkCommandBuffer cmd, const char* name)
{
	if (!debugUtils_) return;
	static PFN_vkCmdBeginDebugUtilsLabelEXT fn = nullptr;
	if (!fn) fn = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT");
	if (!fn) return;
	VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
	label.pLabelName = name;
	fn(cmd, &label);
}

void Device::EndLabel(VkCommandBuffer cmd)
{
	if (!debugUtils_) return;
	static PFN_vkCmdEndDebugUtilsLabelEXT fn = nullptr;
	if (!fn) fn = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT");
	if (fn) fn(cmd);
}
} // namespace hrlvk
