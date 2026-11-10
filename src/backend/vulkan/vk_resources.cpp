// HRL native Vulkan backend - textures (bindless), geometry pool, bottom level
// acceleration structures.
#include "vk_renderer.h"
#include "vk_renderer_internal.h"

namespace hrlvk
{
// ---------------------------------------------------------------------------
// Samplers / textures
// ---------------------------------------------------------------------------
VkSampler GetTextureSampler(HRL_EFilterType minFilter, HRL_EFilterType magFilter)
{
	const uint64_t key = (uint64_t(minFilter) << 32) | uint64_t(magFilter);
	auto it = R.samplers.find(key);
	if (it != R.samplers.end()) return it->second;
	VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	ci.magFilter = magFilter == HRL_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	ci.minFilter = minFilter == HRL_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	ci.mipmapMode = (minFilter == HRL_FILTER_TRILINEAR || minFilter == HRL_FILTER_ANISOTROPIC)
		? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	// NEAREST and LINEAR keep the historical "no mip" behaviour.
	ci.maxLod = (minFilter == HRL_FILTER_NEAREST || minFilter == HRL_FILTER_LINEAR) ? 0.f : VK_LOD_CLAMP_NONE;
	if (minFilter == HRL_FILTER_ANISOTROPIC && Dev().caps.anisotropy)
	{
		ci.anisotropyEnable = VK_TRUE;
		ci.maxAnisotropy = std::min(8.f, Dev().caps.maxAnisotropy);
	}
	VkSampler s = VK_NULL_HANDLE;
	vkCreateSampler(Dev().device, &ci, nullptr, &s);
	R.samplers.emplace(key, s);
	return s;
}

void WriteBindless(const Texture& t)
{
	VkSampler sampler = GetTextureSampler(t.minFilter, t.magFilter);
	VkDescriptorImageInfo infos[2] = {
		{sampler, t.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		{sampler, t.srgbView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
	VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = R.bindlessSet;
	w.dstBinding = 0;
	w.dstArrayElement = t.slot * 2u;
	w.descriptorCount = 2;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = infos;
	vkUpdateDescriptorSets(Dev().device, 1, &w, 0, nullptr);
}

bool CreateTextureInternal(Texture& t, const unsigned char* rgba, int width, int height)
{
	Device& dev = Dev();
	if (R.freeSlots.empty() && R.nextSlot >= dev.caps.maxBindlessTextures)
	{
		ReportError("texture limit reached (" + std::to_string(dev.caps.maxBindlessTextures) + ")");
		return false;
	}
	ImageDesc d;
	d.format = VK_FORMAT_R8G8B8A8_UNORM;
	d.width = (uint32_t)width;
	d.height = (uint32_t)height;
	d.mips = (uint32_t)std::floor(std::log2((float)std::max(width, height))) + 1u;
	d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	d.mutableFormat = true;
	if (!dev.CreateImage(d, t.image, "texture")) return false;
	t.srgbView = dev.CreateView(t.image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_VIEW_TYPE_2D, 0, t.image.mips, 0, 1);
	if (!t.srgbView || !dev.UploadToImage(t.image, 0, rgba, VkDeviceSize(width) * VkDeviceSize(height) * 4u))
	{
		if (t.srgbView) vkDestroyImageView(dev.device, t.srgbView, nullptr);
		dev.DestroyImage(t.image, false);
		return false;
	}
	dev.GenerateMips(dev.UploadCmd(), t.image);
	if (!R.freeSlots.empty()) { t.slot = R.freeSlots.back(); R.freeSlots.pop_back(); }
	else t.slot = R.nextSlot++;
	WriteBindless(t);
	return true;
}

void DestroyTextureInternal(Texture& t, bool releaseSlot)
{
	Device& dev = Dev();
	VkDevice d = dev.device;
	VkImageView v = t.srgbView;
	const uint32_t slot = t.slot;
	dev.Defer([d, v, slot, releaseSlot]()
	{
		if (v) vkDestroyImageView(d, v, nullptr);
		// The slot is reused only once no in-flight frame can sample it.
		if (releaseSlot) R.freeSlots.push_back(slot);
	});
	dev.DestroyImage(t.image);
	t = Texture{};
}

uint32_t TextureSlot(const HRL_Material* m, const char* key, uint32_t fallback)
{
	auto it = m->textureParams_.find(key);
	if (it == m->textureParams_.end()) return fallback;
	auto t = R.textures.find(it->second);
	return t == R.textures.end() ? fallback : t->second.slot;
}

// ---------------------------------------------------------------------------
// Geometry pool
// ---------------------------------------------------------------------------
namespace
{
VkBufferUsageFlags GeometryUsage(bool index)
{
	VkBufferUsageFlags u = (index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	if (Dev().caps.rayQuery)
		u |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	return u;
}

} // namespace

// Makes sure the pool buffer holds `needed` bytes; existing data is preserved.
bool EnsurePoolCapacity(Buffer& buffer, VkDeviceSize needed, bool index)
{
	if (buffer && buffer.size >= needed) return true;
	Device& dev = Dev();
	Buffer grown;
	const VkDeviceSize size = std::max<VkDeviceSize>(needed, buffer ? buffer.size * 2 : (index ? (8ull << 20) : (32ull << 20)));
	if (!dev.CreateBuffer(size, GeometryUsage(index), false, grown, index ? "index pool" : "vertex pool")) return false;
	if (buffer)
	{
		VkBufferCopy copy{0, 0, buffer.size};
		vkCmdCopyBuffer(dev.UploadCmd(), buffer.handle, grown.handle, 1, &copy);
		dev.DestroyBuffer(buffer);
	}
	buffer = grown;
	return true;
}

namespace
{
void FreeLOD(MeshLOD& lod)
{
	if (lod.vertexAlloc) vmaVirtualFree(R.vertexBlock, lod.vertexAlloc);
	if (lod.indexAlloc) vmaVirtualFree(R.indexBlock, lod.indexAlloc);
	lod = MeshLOD{};
}

} // namespace

void DestroyBlas(Blas& blas)
{
	if (!blas.handle) return;
	VkDevice d = Dev().device;
	VkAccelerationStructureKHR h = blas.handle;
	Dev().Defer([d, h]() { R.destroyAS(d, h, nullptr); });
	Dev().DestroyBuffer(blas.storage);
	blas = Blas{};
}

namespace
{
void BuildBlas(MeshGPU& mesh)
{
	Device& dev = Dev();
	const MeshLOD& lod = mesh.lods[0];
	VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
	geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	auto& tri = geo.geometry.triangles;
	tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	tri.vertexData.deviceAddress = R.vertexBuffer.address + VkDeviceSize(lod.vertexOffset) * sizeof(HRL_Vertex3D);
	tri.vertexStride = sizeof(HRL_Vertex3D);
	tri.maxVertex = lod.vertexCount - 1;
	tri.indexType = VK_INDEX_TYPE_UINT32;
	tri.indexData.deviceAddress = R.indexBuffer.address + VkDeviceSize(lod.firstIndex) * sizeof(uint32_t);

	VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geo;
	const uint32_t primitives = lod.indexCount / 3;
	VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
	R.buildSizesAS(dev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);

	Buffer scratch;
	if (!dev.CreateBuffer(sizes.accelerationStructureSize,
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false, mesh.blas.storage, "blas") ||
		!dev.CreateBuffer(sizes.buildScratchSize + R.scratchAlignment,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false, scratch, "blas scratch"))
	{
		dev.DestroyBuffer(mesh.blas.storage, false);
		return;
	}
	VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
	ci.buffer = mesh.blas.storage.handle;
	ci.size = sizes.accelerationStructureSize;
	ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	if (R.createAS(dev.device, &ci, nullptr, &mesh.blas.handle) != VK_SUCCESS)
	{
		dev.DestroyBuffer(mesh.blas.storage, false);
		dev.DestroyBuffer(scratch, false);
		return;
	}
	info.dstAccelerationStructure = mesh.blas.handle;
	info.scratchData.deviceAddress = AlignUp(scratch.address, R.scratchAlignment);

	VkCommandBuffer cmd = dev.UploadCmd();
	// The geometry was copied into the pool by this same command buffer.
	VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
	mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
	mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
	VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.memoryBarrierCount = 1;
	di.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2(cmd, &di);
	VkAccelerationStructureBuildRangeInfoKHR range{primitives, 0, 0, 0};
	const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
	R.buildAS(cmd, 1, &info, &ranges);
	dev.DestroyBuffer(scratch);

	VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
	ai.accelerationStructure = mesh.blas.handle;
	mesh.blas.address = R.addressAS(dev.device, &ai);
}
} // namespace

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------
int CreateMesh(HRL_id id, HRL_uint level, const HRL_Vertex3D* vertices, size_t vertexCount, const HRL_uint* indices, size_t indexCount)
{
	if (!R.initialized || !vertices || vertexCount == 0) return HRL_FALSE;
	Device& dev = Dev();
	if (R.meshes.find(id) == R.meshes.end()) ++R.meshTableRevision;
	MeshGPU& mesh = R.meshes[id];
	if (mesh.lods.size() <= level) mesh.lods.resize(level + 1);
	MeshLOD& lod = mesh.lods[level];
	FreeLOD(lod);

	std::vector<HRL_uint> generated;
	if (!indices || indexCount == 0)
	{
		generated.resize(vertexCount);
		for (size_t i = 0; i < vertexCount; ++i) generated[i] = (HRL_uint)i;
		indices = generated.data();
		indexCount = vertexCount;
	}

	VmaVirtualAllocationCreateInfo ai{};
	ai.size = vertexCount;
	VkDeviceSize vOffset = 0, iOffset = 0;
	if (vmaVirtualAllocate(R.vertexBlock, &ai, &lod.vertexAlloc, &vOffset) != VK_SUCCESS) return HRL_FALSE;
	ai.size = indexCount;
	if (vmaVirtualAllocate(R.indexBlock, &ai, &lod.indexAlloc, &iOffset) != VK_SUCCESS) { FreeLOD(lod); return HRL_FALSE; }

	const VkDeviceSize vBytes = vertexCount * sizeof(HRL_Vertex3D), iBytes = indexCount * sizeof(uint32_t);
	if (!EnsurePoolCapacity(R.vertexBuffer, vOffset * sizeof(HRL_Vertex3D) + vBytes, false) ||
		!EnsurePoolCapacity(R.indexBuffer, iOffset * sizeof(uint32_t) + iBytes, true) ||
		!dev.UploadToBuffer(R.vertexBuffer, vOffset * sizeof(HRL_Vertex3D), vertices, vBytes) ||
		!dev.UploadToBuffer(R.indexBuffer, iOffset * sizeof(uint32_t), indices, iBytes))
	{
		FreeLOD(lod);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "Vulkan: mesh upload failed");
		return HRL_FALSE;
	}
	lod.vertexOffset = (uint32_t)vOffset;
	lod.vertexCount = (uint32_t)vertexCount;
	lod.firstIndex = (uint32_t)iOffset;
	lod.indexCount = (uint32_t)indexCount;
	lod.valid = true;

	if (level == 0 && dev.caps.rayQuery)
	{
		DestroyBlas(mesh.blas);
		BuildBlas(mesh);
	}
	return HRL_TRUE;
}

void DeleteMeshLODs(HRL_id id)
{
	auto it = R.meshes.find(id);
	if (it == R.meshes.end()) return;
	for (size_t i = 1; i < it->second.lods.size(); ++i) FreeLOD(it->second.lods[i]);
	it->second.lods.resize(std::min<size_t>(it->second.lods.size(), 1));
}

void DeleteMesh(HRL_id id)
{
	auto it = R.meshes.find(id);
	if (it == R.meshes.end()) return;
	// The pool ranges may be reused at once: a frame that still draws the old
	// mesh was recorded before the new data is uploaded (uploads go first in
	// the next submission), so defer the release by the in-flight frames.
	std::vector<MeshLOD> lods = it->second.lods;
	Dev().Defer([lods]() mutable { for (MeshLOD& l : lods) FreeLOD(l); });
	DestroyBlas(it->second.blas);
	R.meshes.erase(it);
	++R.meshTableRevision;
}

HRL_id CreateTexture(HRL_id id, const unsigned char* rgba, int width, int height)
{
	if (!R.initialized || !rgba || width <= 0 || height <= 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "Vulkan: invalid texture data");
		return HRL_INVALID_ID;
	}
	if (id == HRL_INVALID_ID) id = GenerateHRL_ID();
	auto existing = R.textures.find(id);
	if (existing != R.textures.end()) { DestroyTextureInternal(existing->second, true); R.textures.erase(existing); }
	Texture t;
	if (!CreateTextureInternal(t, rgba, width, height)) return HRL_INVALID_ID;
	R.textures.emplace(id, t);
	++R.textureRevision;
	return id;
}

void DeleteTexture(HRL_id id)
{
	auto it = R.textures.find(id);
	if (it == R.textures.end()) return;
	DestroyTextureInternal(it->second, true);
	R.textures.erase(it);
	++R.textureRevision;
}

bool IsValidTexture(HRL_id id) { return R.textures.count(id) != 0; }

void GetTextureSize(HRL_id id, int* width, int* height)
{
	auto it = R.textures.find(id);
	if (width) *width = it == R.textures.end() ? 0 : (int)it->second.image.extent.width;
	if (height) *height = it == R.textures.end() ? 0 : (int)it->second.image.extent.height;
}

void SetTextureFilter(HRL_id id, HRL_EFilterType filter, bool minFilter)
{
	auto it = R.textures.find(id);
	if (it == R.textures.end()) return;
	(minFilter ? it->second.minFilter : it->second.magFilter) = filter;
	WriteBindless(it->second);
}
} // namespace hrlvk
