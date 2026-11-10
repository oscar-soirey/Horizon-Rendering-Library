// HRL frontend - 2D voxel worlds.
#include "frontend_internal.h"

static HRL_VoxelWorld* FindVoxelWorld(HRL_id sceneid)
{
	auto it = ctx_.scenes.find(sceneid);
	if (it == ctx_.scenes.end() || !it->second)
		return nullptr;
	return it->second->voxel_world;
}

static bool VoxelWorldElementCount(int width, int height, size_t& outCount)
{
	if (width <= 0 || height <= 0)
		return false;
	const size_t w = static_cast<size_t>(width);
	const size_t h = static_cast<size_t>(height);
	if (h != 0 && w > std::numeric_limits<size_t>::max() / h)
		return false;
	outCount = w * h;
	return true;
}

static void EnsureVoxelWorld(HRL_id sceneid)
{
	auto it = ctx_.scenes.find(sceneid);
	if (it == ctx_.scenes.end() || !it->second)
		return;
	if (!it->second->voxel_world)
		it->second->voxel_world = new HRL_VoxelWorld();
}

static uint64_t VoxelChunkKey(int chunkX, int chunkY)
{
	return (static_cast<uint64_t>(static_cast<uint32_t>(chunkX)) << 32u) |
		static_cast<uint32_t>(chunkY);
}

static void MarkVoxelChunkDirty(HRL_VoxelWorld* world, int voxelX, int voxelY)
{
	if (!world || world->chunk_size_ <= 0 || voxelX < 0 || voxelY < 0)
		return;

	const int chunkX = voxelX / world->chunk_size_;
	const int chunkY = voxelY / world->chunk_size_;
	world->dirty_chunks_.insert(VoxelChunkKey(chunkX, chunkY));
	world->render_dirty_chunks_.insert(VoxelChunkKey(chunkX, chunkY));
	world->light_dirty_chunks_.insert(VoxelChunkKey(chunkX, chunkY));

	// En SMOOTH/BLOCKY, la forme d'un voxel depend de ses voisins (y compris
	// diagonaux) et un chunk dessine aussi la bordure de ses voisins : un voxel
	// edite sur le bord d'un chunk doit reconstruire le ou les chunks adjacents.
	if (world->render_mode_ != HRL_VOXEL_FLAT)
	{
		const int localX = voxelX - chunkX * world->chunk_size_;
		const int localY = voxelY - chunkY * world->chunk_size_;
		const int chunkW = std::min(world->chunk_size_, world->width_ - chunkX * world->chunk_size_);
		const int chunkH = std::min(world->chunk_size_, world->height_ - chunkY * world->chunk_size_);
		const int dxMin = localX == 0 ? -1 : 0;
		const int dxMax = localX == chunkW - 1 ? 1 : 0;
		const int dyMin = localY == 0 ? -1 : 0;
		const int dyMax = localY == chunkH - 1 ? 1 : 0;
		for (int dy = dyMin; dy <= dyMax; ++dy)
		{
			for (int dx = dxMin; dx <= dxMax; ++dx)
			{
				const int nx = chunkX + dx;
				const int ny = chunkY + dy;
				if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 ||
					nx * world->chunk_size_ >= world->width_ || ny * world->chunk_size_ >= world->height_)
					continue;
				world->render_dirty_chunks_.insert(VoxelChunkKey(nx, ny));
			}
		}
	}

	// Keep local voxel edits separate from the global geometry revision.
	// Otherwise changing one voxel makes every visible chunk look stale.
	world->voxel_edit_dirty_ = true;
	if (world->voxel_edit_depth_ == 0)
	{
		++world->voxel_revision_;
		world->voxel_edit_dirty_ = false;
	}
}

static constexpr uint32_t kVoxelWorldSaveMagic = 0x31564C48u; // "HLV1" on little-endian files.
// Version 2 : la taille physique d'un voxel n'est plus enregistree (c'est un
// reglage d'affichage de la scene, modifiable a tout moment avec
// HRL_SetVoxelPhysicalSize). Les fichiers version 1 restent lisibles ; leur
// taille enregistree est ignoree.
static constexpr uint16_t kVoxelWorldSaveVersion = 2u;
static constexpr uint16_t kVoxelWorldSaveVersionWithVoxelSize = 1u;
static constexpr size_t kVoxelWorldSaveHeaderSize = 24u;       // version 2
static constexpr size_t kVoxelWorldSaveHeaderSizeV1 = 28u;     // version 1 (avec taille de voxel)
static constexpr uint32_t kVoxelWorldDeltaMagic = 0x31504448u; // "HDP1" little-endian.
static constexpr uint16_t kVoxelWorldDeltaVersion = 1u;
static constexpr uint8_t kVoxelWorldDeltaDeleteEncoding = 0xffu;

enum class VoxelSaveEncoding : uint8_t
{
	Constant = 0,
	Palette = 1,
	Raw = 2,
	Rle = 3
};

class VoxelSaveWriter
{
public:
	void WriteU8(uint8_t v) { data_.push_back(v); }
	void WriteU16(uint16_t v)
	{
		data_.push_back(static_cast<uint8_t>(v & 0xffu));
		data_.push_back(static_cast<uint8_t>((v >> 8u) & 0xffu));
	}
	void WriteU32(uint32_t v)
	{
		for (unsigned i = 0; i < 4; ++i)
			data_.push_back(static_cast<uint8_t>((v >> (8u * i)) & 0xffu));
	}
	void WriteF32(float v)
	{
		uint32_t bits = 0;
		static_assert(sizeof(bits) == sizeof(v), "float must be 32-bit");
		std::memcpy(&bits, &v, sizeof(bits));
		WriteU32(bits);
	}
	void WriteBytes(const uint8_t* bytes, size_t count)
	{
		if (count == 0) return;
		data_.insert(data_.end(), bytes, bytes + count);
	}
	void WriteVarUInt(uint32_t value)
	{
		while (value >= 0x80u)
		{
			WriteU8(static_cast<uint8_t>(value) | 0x80u);
			value >>= 7u;
		}
		WriteU8(static_cast<uint8_t>(value));
	}
	const std::vector<uint8_t>& Data() const { return data_; }
private:
	std::vector<uint8_t> data_;
};

class VoxelSaveReader
{
public:
	VoxelSaveReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

	bool ReadU8(uint8_t& out)
	{
		if (pos_ >= size_) return false;
		out = data_[pos_++];
		return true;
	}
	bool ReadU16(uint16_t& out)
	{
		uint8_t b0 = 0, b1 = 0;
		if (!ReadU8(b0) || !ReadU8(b1)) return false;
		out = static_cast<uint16_t>(b0 | (static_cast<uint16_t>(b1) << 8u));
		return true;
	}
	bool ReadU32(uint32_t& out)
	{
		uint8_t b[4] = {};
		for (unsigned i = 0; i < 4; ++i)
			if (!ReadU8(b[i])) return false;
		out = static_cast<uint32_t>(b[0]) |
			(static_cast<uint32_t>(b[1]) << 8u) |
			(static_cast<uint32_t>(b[2]) << 16u) |
			(static_cast<uint32_t>(b[3]) << 24u);
		return true;
	}
	bool ReadF32(float& out)
	{
		uint32_t bits = 0;
		if (!ReadU32(bits)) return false;
		std::memcpy(&out, &bits, sizeof(out));
		return true;
	}
	bool ReadBytes(const uint8_t*& out, size_t count)
	{
		if (count > Remaining()) return false;
		out = data_ + pos_;
		pos_ += count;
		return true;
	}
	bool ReadVarUInt(uint32_t& out)
	{
		out = 0;
		unsigned shift = 0;
		for (unsigned i = 0; i < 5; ++i)
		{
			uint8_t b = 0;
			if (!ReadU8(b)) return false;
			if (i == 4 && (b & 0xF0u) != 0) return false;
			out |= static_cast<uint32_t>(b & 0x7Fu) << shift;
			if ((b & 0x80u) == 0) return true;
			shift += 7u;
		}
		return false;
	}
	size_t Position() const { return pos_; }
	size_t Remaining() const { return size_ - pos_; }
private:
	const uint8_t* data_ = nullptr;
	size_t size_ = 0;
	size_t pos_ = 0;
};

static unsigned VoxelSaveBitsForPalette(size_t count)
{
	unsigned bits = 0;
	size_t v = count > 1 ? count - 1 : 0;
	while (v != 0)
	{
		++bits;
		v >>= 1u;
	}
	return std::max(1u, bits);
}

static void VoxelSaveAppendPackedIndices(
	const HRL_VoxelWorld* world,
	int chunkX, int chunkY,
	const std::array<int16_t, 256>& paletteIndex,
	unsigned bits,
	std::vector<uint8_t>& out)
{
	const int chunkSize = world->chunk_size_;
	const int minX = chunkX * chunkSize;
	const int minY = chunkY * chunkSize;
	const int maxX = std::min(minX + chunkSize, world->width_);
	const int maxY = std::min(minY + chunkSize, world->height_);
	const int chunkW = maxX - minX;
	const HRL_VoxelWorld::VoxelChunk* chunk = world->FindVoxelChunk(chunkX, chunkY);
	if (!chunk || chunkW <= 0)
		return;

	uint32_t accumulator = 0;
	unsigned accumulatorBits = 0;
	for (int y = minY; y < maxY; ++y)
	{
		for (int x = minX; x < maxX; ++x)
		{
			const int localX = x - minX;
			const int localY = y - minY;
			const HRL_VoxelType type = (*chunk)[static_cast<size_t>(localY) * static_cast<size_t>(chunkW) +
				static_cast<size_t>(localX)].type;
			const int16_t palette = paletteIndex[type];
			if (palette < 0) return;
			accumulator |= static_cast<uint32_t>(palette) << accumulatorBits;
			accumulatorBits += bits;
			while (accumulatorBits >= 8u)
			{
				out.push_back(static_cast<uint8_t>(accumulator & 0xffu));
				accumulator >>= 8u;
				accumulatorBits -= 8u;
			}
		}
	}
	if (accumulatorBits != 0)
		out.push_back(static_cast<uint8_t>(accumulator & 0xffu));
}

static void VoxelSaveBuildRle(
	const HRL_VoxelWorld* world,
	int chunkX, int chunkY,
	std::vector<uint8_t>& out)
{
	const int chunkSize = world->chunk_size_;
	const int minX = chunkX * chunkSize;
	const int minY = chunkY * chunkSize;
	const int maxX = std::min(minX + chunkSize, world->width_);
	const int maxY = std::min(minY + chunkSize, world->height_);
	const int chunkW = maxX - minX;
	const HRL_VoxelWorld::VoxelChunk* chunk = world->FindVoxelChunk(chunkX, chunkY);
	if (!chunk || chunkW <= 0)
		return;

	for (int y = minY; y < maxY; ++y)
	{
		int x = minX;
		while (x < maxX)
		{
			const int localY = y - minY;
			const int localX = x - minX;
			const HRL_VoxelType type = (*chunk)[static_cast<size_t>(localY) * static_cast<size_t>(chunkW) +
				static_cast<size_t>(localX)].type;
			int run = 1;
			while (x + run < maxX &&
				(*chunk)[static_cast<size_t>(localY) * static_cast<size_t>(chunkW) +
					static_cast<size_t>(localX + run)].type == type)
				++run;
			out.push_back(type);
			uint32_t remaining = static_cast<uint32_t>(run);
			while (remaining >= 0x80u)
			{
				out.push_back(static_cast<uint8_t>(remaining) | 0x80u);
				remaining >>= 7u;
			}
			out.push_back(static_cast<uint8_t>(remaining));
			x += run;
		}
	}
}

static bool VoxelSaveBuildChunk(
	const HRL_VoxelWorld* world,
	int chunkX, int chunkY,
	VoxelSaveEncoding& outEncoding,
	uint8_t& outBits,
	std::vector<uint8_t>& outPalette,
	std::vector<uint8_t>& outPayload)
{
	outEncoding = VoxelSaveEncoding::Raw;
	outBits = 0;
	outPalette.clear();
	outPayload.clear();

	std::array<bool, 256> seen{};
	std::vector<HRL_VoxelType> palette;
	palette.reserve(16);
	HRL_VoxelType first = 0;
	bool firstSet = false;
	bool uniform = true;

	const int chunkSize = world->chunk_size_;
	const int minX = chunkX * chunkSize;
	const int minY = chunkY * chunkSize;
	const int maxX = std::min(minX + chunkSize, world->width_);
	const int maxY = std::min(minY + chunkSize, world->height_);
	const int chunkW = maxX - minX;
	const HRL_VoxelWorld::VoxelChunk* chunk = world->FindVoxelChunk(chunkX, chunkY);
	if (!chunk || chunkW <= 0)
		return false;
	for (int y = minY; y < maxY; ++y)
	{
		for (int x = minX; x < maxX; ++x)
		{
			const int localX = x - minX;
			const int localY = y - minY;
			const HRL_VoxelType type = (*chunk)[static_cast<size_t>(localY) * static_cast<size_t>(chunkW) +
				static_cast<size_t>(localX)].type;
			if (!firstSet) { first = type; firstSet = true; }
			else if (type != first) uniform = false;
			if (!seen[type])
			{
				seen[type] = true;
				palette.push_back(type);
			}
		}
	}
	if (!firstSet) return false;
	if (uniform)
	{
		if (first == 0) return false;
		outEncoding = VoxelSaveEncoding::Constant;
		outPalette.push_back(first);
		return true;
	}

	std::array<int16_t, 256> paletteIndex{};
	paletteIndex.fill(-1);
	for (size_t i = 0; i < palette.size(); ++i)
		paletteIndex[palette[i]] = static_cast<int16_t>(i);

	std::vector<uint8_t> palettePayload;
	const unsigned bits = VoxelSaveBitsForPalette(palette.size());
	VoxelSaveAppendPackedIndices(world, chunkX, chunkY, paletteIndex, bits, palettePayload);

	std::vector<uint8_t> rawPayload;
	rawPayload.reserve(static_cast<size_t>(maxX - minX) * static_cast<size_t>(maxY - minY));
	for (int y = minY; y < maxY; ++y)
		for (int x = minX; x < maxX; ++x)
			rawPayload.push_back((*chunk)[static_cast<size_t>(y - minY) * static_cast<size_t>(chunkW) +
				static_cast<size_t>(x - minX)].type);

	std::vector<uint8_t> rlePayload;
	VoxelSaveBuildRle(world, chunkX, chunkY, rlePayload);

	const size_t paletteTotal = palette.size() + palettePayload.size();
	const size_t rawTotal = rawPayload.size();
	const size_t rleTotal = rlePayload.size();
	if (paletteTotal <= rawTotal && paletteTotal <= rleTotal)
	{
		outEncoding = VoxelSaveEncoding::Palette;
		outBits = static_cast<uint8_t>(bits);
		outPalette.assign(palette.begin(), palette.end());
		outPayload = std::move(palettePayload);
	}
	else if (rleTotal <= rawTotal)
	{
		outEncoding = VoxelSaveEncoding::Rle;
		outPayload = std::move(rlePayload);
	}
	else
	{
		outEncoding = VoxelSaveEncoding::Raw;
		outPayload = std::move(rawPayload);
	}
	return true;
}

static bool VoxelSaveBuildAll(const HRL_VoxelWorld* world, std::vector<uint8_t>& out)
{
	if (!world || world->width_ <= 0 || world->height_ <= 0 || world->chunk_size_ <= 0)
		return false;

	const uint64_t chunkColumns64 =
		(static_cast<uint64_t>(world->width_) + static_cast<uint64_t>(world->chunk_size_) - 1u) /
		static_cast<uint64_t>(world->chunk_size_);
	const uint64_t chunkRows64 =
		(static_cast<uint64_t>(world->height_) + static_cast<uint64_t>(world->chunk_size_) - 1u) /
		static_cast<uint64_t>(world->chunk_size_);
	if (chunkColumns64 == 0 || chunkRows64 == 0 ||
		chunkColumns64 * chunkRows64 > std::numeric_limits<uint32_t>::max())
		return false;

	VoxelSaveWriter writer;
	writer.WriteU32(kVoxelWorldSaveMagic);
	writer.WriteU16(kVoxelWorldSaveVersion);
	writer.WriteU16(0u);
	writer.WriteU32(static_cast<uint32_t>(world->width_));
	writer.WriteU32(static_cast<uint32_t>(world->height_));
	writer.WriteU32(static_cast<uint32_t>(world->chunk_size_));

	const size_t chunkCountOffset = 20u;
	writer.WriteU32(0u);

	uint32_t nonEmptyChunks = 0;

	// The world is sparse: only chunks that actually contain data exist.
	// Iterating the complete chunk grid was the main cost for huge empty worlds.
	for (const auto& [key, chunk] : world->voxel_chunks_)
	{
		(void)chunk;
		const int chunkX = static_cast<int32_t>(key >> 32u);
		const int chunkY = static_cast<int32_t>(key & 0xffffffffu);
		if (chunkX < 0 || chunkY < 0 ||
			static_cast<uint64_t>(chunkX) >= chunkColumns64 ||
			static_cast<uint64_t>(chunkY) >= chunkRows64)
			return false;

		VoxelSaveEncoding encoding = VoxelSaveEncoding::Raw;
		uint8_t bits = 0;
		std::vector<uint8_t> palette;
		std::vector<uint8_t> payload;
		if (!VoxelSaveBuildChunk(world, chunkX, chunkY, encoding, bits, palette, payload))
			continue;

		if (palette.size() > 255u || payload.size() > std::numeric_limits<uint32_t>::max())
			return false;

		writer.WriteU32(static_cast<uint32_t>(static_cast<uint64_t>(chunkY) * chunkColumns64 +
			static_cast<uint64_t>(chunkX)));
		writer.WriteU8(static_cast<uint8_t>(encoding));
		writer.WriteU8(bits);
		writer.WriteU8(static_cast<uint8_t>(palette.size()));
		writer.WriteU8(0u);
		writer.WriteU32(static_cast<uint32_t>(payload.size()));
		writer.WriteBytes(palette.data(), palette.size());
		writer.WriteBytes(payload.data(), payload.size());
		++nonEmptyChunks;
	}

	std::vector<uint8_t> result = writer.Data();
	if (result.size() < kVoxelWorldSaveHeaderSize)
		return false;

	const uint32_t count = nonEmptyChunks;
	result[chunkCountOffset + 0] = static_cast<uint8_t>(count & 0xffu);
	result[chunkCountOffset + 1] = static_cast<uint8_t>((count >> 8u) & 0xffu);
	result[chunkCountOffset + 2] = static_cast<uint8_t>((count >> 16u) & 0xffu);
	result[chunkCountOffset + 3] = static_cast<uint8_t>((count >> 24u) & 0xffu);
	out = std::move(result);
	return true;
}

static bool VoxelSaveDecodeChunk(
	const uint8_t* payload, size_t payloadSize,
	VoxelSaveEncoding encoding, uint8_t bits,
	const std::vector<HRL_VoxelType>& palette,
	int chunkW, int chunkH,
	std::vector<HRL_VoxelType>& destination)
{
	const size_t voxelCount = static_cast<size_t>(chunkW) * static_cast<size_t>(chunkH);
	destination.assign(voxelCount, 0);
	if (encoding == VoxelSaveEncoding::Constant)
	{
		if (palette.size() != 1 || payloadSize != 0) return false;
		std::fill(destination.begin(), destination.end(), palette[0]);
		return true;
	}
	if (encoding == VoxelSaveEncoding::Raw)
	{
		if (!palette.empty() || payloadSize != voxelCount) return false;
		std::memcpy(destination.data(), payload, voxelCount);
		return true;
	}
	if (encoding == VoxelSaveEncoding::Palette)
	{
		if (palette.empty() || palette.size() > 255u || bits == 0 || bits > 8) return false;
		const size_t expectedBytes = (voxelCount * static_cast<size_t>(bits) + 7u) / 8u;
		if (payloadSize != expectedBytes) return false;
		uint32_t accumulator = 0;
		unsigned accumulatorBits = 0;
		size_t input = 0;
		for (size_t i = 0; i < voxelCount; ++i)
		{
			while (accumulatorBits < bits)
			{
				if (input >= payloadSize) return false;
				accumulator |= static_cast<uint32_t>(payload[input++]) << accumulatorBits;
				accumulatorBits += 8u;
			}
			const uint32_t index = accumulator & ((1u << bits) - 1u);
			accumulator >>= bits;
			accumulatorBits -= bits;
			if (index >= palette.size()) return false;
			destination[i] = palette[index];
		}
		return true;
	}
	if (encoding == VoxelSaveEncoding::Rle)
	{
		VoxelSaveReader reader(payload, payloadSize);
		size_t dst = 0;
		for (int y = 0; y < chunkH; ++y)
		{
			size_t row = 0;
			while (row < static_cast<size_t>(chunkW))
			{
				uint8_t type = 0;
				uint32_t run = 0;
				if (!reader.ReadU8(type) || !reader.ReadVarUInt(run) || run == 0)
					return false;
				if (run > static_cast<uint32_t>(chunkW) - row)
					return false;
				std::fill(destination.begin() + static_cast<std::ptrdiff_t>(dst),
					destination.begin() + static_cast<std::ptrdiff_t>(dst + run), type);
				row += run;
				dst += run;
			}
		}
		return reader.Remaining() == 0;
	}
	return false;
}

bool HRL_EnsureVoxelChunkLoaded(HRL_VoxelWorld* world, int chunkX, int chunkY)
{
	if (!world || chunkX < 0 || chunkY < 0)
		return false;
	if (world->FindVoxelChunk(chunkX, chunkY))
		return true;

	const uint64_t key = HRL_VoxelWorld::MakeVoxelChunkKey(chunkX, chunkY);
	auto it = world->serialized_chunks_.find(key);
	if (it == world->serialized_chunks_.end() || it->second.deleted)
		return false;
	const auto& rec = it->second;
	if (rec.payload_offset > world->serialized_source_.size() ||
		rec.payload_size > world->serialized_source_.size() - rec.payload_offset)
		return false;

	const int minX = chunkX * world->chunk_size_;
	const int minY = chunkY * world->chunk_size_;
	const int chunkW = std::min(world->chunk_size_, world->width_ - minX);
	const int chunkH = std::min(world->chunk_size_, world->height_ - minY);
	if (chunkW <= 0 || chunkH <= 0)
		return false;

	std::vector<HRL_VoxelType> decoded;
	if (!VoxelSaveDecodeChunk(world->serialized_source_.data() + rec.payload_offset,
		rec.payload_size, static_cast<VoxelSaveEncoding>(rec.encoding), rec.bits,
		rec.palette, chunkW, chunkH, decoded))
		return false;

	bool nonEmpty = false;
	for (HRL_VoxelType type : decoded)
	{
		if (type != 0) { nonEmpty = true; break; }
	}
	if (!nonEmpty)
		return false;

	auto& chunk = world->EnsureVoxelChunk(chunkX, chunkY);
	for (size_t i = 0; i < decoded.size(); ++i)
		chunk[i].type = decoded[i];
	return true;
}

void HRL_SetVoxelSize(HRL_id _sceneid, int _width, int _height)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelSize: invalid scene ID");
		return;
	}

	if (_width <= 0 || _height <= 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetVoxelSize: width and height must be positive");
		return;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	world->width_ = _width;
	world->height_ = _height;
	world->voxel_chunks_.clear();
	world->serialized_source_.clear();
	world->serialized_chunks_.clear();
	world->dirty_chunks_.clear();
	world->render_dirty_chunks_.clear();
	++world->geometry_revision_;
	++world->voxel_revision_;
}

int HRL_CreateVoxelWorld(HRL_id _sceneid, int _width, int _height)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateVoxelWorld: invalid scene ID");
		return HRL_FALSE;
	}

	if (_width <= 0 || _height <= 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_CreateVoxelWorld: width and height must be positive");
		return HRL_FALSE;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;

	world->width_ = _width;
	world->height_ = _height;
	world->voxel_chunks_.clear();
	world->serialized_source_.clear();
	world->serialized_chunks_.clear();
	world->dirty_chunks_.clear();
	world->render_dirty_chunks_.clear();
	++world->geometry_revision_;
	++world->voxel_revision_;

	return HRL_TRUE;
}

void HRL_SetVoxelPhysicalSize(HRL_id _sceneid, float _size)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelPhysicalSize: invalid scene ID");
		return;
	}
	if (!std::isfinite(_size) || _size <= 0.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVoxelPhysicalSize: size must be finite and greater than zero");
		return;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	if (world->voxel_size_ == _size)
		return;
	// Modifiable a tout moment : les chunks visibles sont reconstruits a la
	// prochaine frame, les emetteurs voxel et les conversions de coordonnees
	// suivent immediatement. Les donnees voxel ne changent pas.
	world->voxel_size_ = _size;
	++world->geometry_revision_;
	++world->voxel_revision_;
}

int HRL_WorldToVoxelCoordinates(
	HRL_id _sceneid,
	float _world_x, float _world_y,
	float* _voxel_x, float* _voxel_y)
{
	if (_voxel_x) *_voxel_x = 0.f;
	if (_voxel_y) *_voxel_y = 0.f;

	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_WorldToVoxelCoordinates: invalid scene ID");
		return HRL_FALSE;
	}

	const HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world || !std::isfinite(world->voxel_size_) || world->voxel_size_ <= 0.f)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING,
			"HRL_WorldToVoxelCoordinates: voxel physical size is not configured");
		return HRL_FALSE;
	}
	if (!_voxel_x || !_voxel_y)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_WorldToVoxelCoordinates: output pointer is null");
		return HRL_FALSE;
	}
	if (!std::isfinite(_world_x) || !std::isfinite(_world_y))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_WorldToVoxelCoordinates: input coordinates must be finite");
		return HRL_FALSE;
	}

	const float invVoxelSize = 1.f / world->voxel_size_;
	*_voxel_x = _world_x * invVoxelSize;
	*_voxel_y = _world_y * invVoxelSize;
	return HRL_TRUE;
}

int HRL_VoxelToWorldCoordinates(
	HRL_id _sceneid,
	float _voxel_x, float _voxel_y,
	float* _world_x, float* _world_y)
{
	if (_world_x) *_world_x = 0.f;
	if (_world_y) *_world_y = 0.f;

	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_VoxelToWorldCoordinates: invalid scene ID");
		return HRL_FALSE;
	}

	const HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world || !std::isfinite(world->voxel_size_) || world->voxel_size_ <= 0.f)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING,
			"HRL_VoxelToWorldCoordinates: voxel physical size is not configured");
		return HRL_FALSE;
	}
	if (!_world_x || !_world_y)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_VoxelToWorldCoordinates: output pointer is null");
		return HRL_FALSE;
	}
	if (!std::isfinite(_voxel_x) || !std::isfinite(_voxel_y))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_VoxelToWorldCoordinates: input coordinates must be finite");
		return HRL_FALSE;
	}

	*_world_x = _voxel_x * world->voxel_size_;
	*_world_y = _voxel_y * world->voxel_size_;
	return HRL_TRUE;
}

void HRL_SetVoxelChunkSize(HRL_id _sceneid, int _chunkSize)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelChunkSize: invalid scene ID");
		return;
	}
	if (_chunkSize <= 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVoxelChunkSize: chunk size must be greater than zero");
		return;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	if (world->chunk_size_ == _chunkSize)
		return;
	// Chunks of a lazily loaded world still compressed : decoded first, or
	// they would be lost with serialized_chunks_ below.
	HRL_InternalMaterializeVoxelChunks(world);
	const int oldChunkSize = world->chunk_size_;
	auto oldChunks = std::move(world->voxel_chunks_);
	world->voxel_chunks_.clear();
	world->serialized_source_.clear();
	world->serialized_chunks_.clear();
	world->chunk_size_ = _chunkSize;

	// Re-bucket the sparse storage when the chunk size changes. This is an
	// explicit configuration operation, so paying the cost once here is much
	// better than keeping a dense world just to make this case cheap.
	const int oldChunkColumns = (world->width_ + oldChunkSize - 1) / oldChunkSize;
	for (const auto& [key, oldChunk] : oldChunks)
	{
		const int oldChunkX = static_cast<int32_t>(key >> 32u);
		const int oldChunkY = static_cast<int32_t>(key & 0xffffffffu);
		const int oldMinX = oldChunkX * oldChunkSize;
		const int oldMinY = oldChunkY * oldChunkSize;
		const int oldChunkW = std::min(oldChunkSize, world->width_ - oldMinX);
		const int oldChunkH = std::min(oldChunkSize, world->height_ - oldMinY);
		if (oldChunkX < 0 || oldChunkY < 0 || oldChunkX >= oldChunkColumns ||
			oldChunkW <= 0 || oldChunkH <= 0)
			continue;

		for (int y = 0; y < oldChunkH; ++y)
		{
			for (int x = 0; x < oldChunkW; ++x)
			{
				const HRL_VoxelType type =
					oldChunk[static_cast<size_t>(y) * static_cast<size_t>(oldChunkW) +
					static_cast<size_t>(x)].type;
				if (type == 0)
					continue;
				*world->GetVoxelTypeMutable(oldMinX + x, oldMinY + y) = type;
			}
		}
	}
	world->dirty_chunks_.clear();
	world->render_dirty_chunks_.clear();
	++world->geometry_revision_;
	++world->voxel_revision_;
}

// Decodes every chunk still compressed in serialized_chunks_ (lazy load).
void HRL_InternalMaterializeVoxelChunks(HRL_VoxelWorld* world)
{
	if (!world)
		return;
	for (const auto& [key, record] : world->serialized_chunks_)
	{
		if (record.deleted)
			continue;
		const int chunkX = static_cast<int32_t>(key >> 32u);
		const int chunkY = static_cast<int32_t>(key & 0xffffffffu);
		HRL_EnsureVoxelChunkLoaded(world, chunkX, chunkY);
	}
}

static bool VoxelWorldSizeIsEncodable(int width, int height, int chunkSize)
{
	if (width <= 0 || height <= 0 || chunkSize <= 0)
		return false;
	const uint64_t columns = (static_cast<uint64_t>(width) + chunkSize - 1u) / static_cast<uint64_t>(chunkSize);
	const uint64_t rows = (static_cast<uint64_t>(height) + chunkSize - 1u) / static_cast<uint64_t>(chunkSize);
	return columns * rows <= std::numeric_limits<uint32_t>::max();
}

int HRL_GetVoxelSize(HRL_id _sceneid, int* _width, int* _height)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second || !it->second->voxel_world)
	{
		if (_width) *_width = 0;
		if (_height) *_height = 0;
		return HRL_FALSE;
	}
	if (_width) *_width = it->second->voxel_world->width_;
	if (_height) *_height = it->second->voxel_world->height_;
	return HRL_TRUE;
}

int HRL_GetVoxelChunkSize(HRL_id _sceneid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second || !it->second->voxel_world)
		return 0;
	return it->second->voxel_world->chunk_size_;
}

int HRL_IsVoxelSizeEncodable(HRL_id _sceneid, int _width, int _height)
{
	auto it = ctx_.scenes.find(_sceneid);
	const int chunkSize = (it != ctx_.scenes.end() && it->second && it->second->voxel_world)
		? it->second->voxel_world->chunk_size_ : 32;
	return VoxelWorldSizeIsEncodable(_width, _height, chunkSize) ? HRL_TRUE : HRL_FALSE;
}

uint64_t HRL_CountVoxelsOutside(HRL_id _sceneid, int _width, int _height)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second || !it->second->voxel_world)
		return 0;
	HRL_VoxelWorld* world = it->second->voxel_world;
	HRL_InternalMaterializeVoxelChunks(world);

	uint64_t count = 0;
	const int cs = world->chunk_size_;
	for (const auto& [key, chunk] : world->voxel_chunks_)
	{
		const int chunkX = static_cast<int32_t>(key >> 32u);
		const int chunkY = static_cast<int32_t>(key & 0xffffffffu);
		const int minX = chunkX * cs;
		const int minY = chunkY * cs;
		const int chunkW = std::min(cs, world->width_ - minX);
		const int chunkH = std::min(cs, world->height_ - minY);
		if (chunkW <= 0 || chunkH <= 0)
			continue;
		// Whole chunk inside : nothing to count.
		if (minX + chunkW <= _width && minY + chunkH <= _height)
			continue;
		for (int y = 0; y < chunkH; ++y)
			for (int x = 0; x < chunkW; ++x)
			{
				if (minX + x < _width && minY + y < _height)
					continue;
				if (chunk[static_cast<size_t>(y) * static_cast<size_t>(chunkW) + static_cast<size_t>(x)].type != 0)
					++count;
			}
	}
	return count;
}

int HRL_ResizeVoxelWorld(HRL_id _sceneid, int _width, int _height)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_ResizeVoxelWorld: invalid scene ID");
		return HRL_FALSE;
	}
	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;

	if (!VoxelWorldSizeIsEncodable(_width, _height, world->chunk_size_))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_ResizeVoxelWorld: size must be positive and fit the .hrlv encoding (chunk count <= 2^32)");
		return HRL_FALSE;
	}
	if (world->width_ == _width && world->height_ == _height)
		return HRL_TRUE;

	// Every chunk decoded, then re-bucketed : the edge chunks change size
	// (a chunk stores min(chunk_size, width - minX) columns).
	HRL_InternalMaterializeVoxelChunks(world);
	const int oldWidth = world->width_;
	const int oldHeight = world->height_;
	const int cs = world->chunk_size_;
	auto oldChunks = std::move(world->voxel_chunks_);
	world->voxel_chunks_.clear();
	world->serialized_source_.clear();
	world->serialized_chunks_.clear();
	world->width_ = _width;
	world->height_ = _height;

	for (auto& [key, oldChunk] : oldChunks)
	{
		const int chunkX = static_cast<int32_t>(key >> 32u);
		const int chunkY = static_cast<int32_t>(key & 0xffffffffu);
		const int minX = chunkX * cs;
		const int minY = chunkY * cs;
		const int oldW = std::min(cs, oldWidth - minX);
		const int oldH = std::min(cs, oldHeight - minY);
		if (oldW <= 0 || oldH <= 0 || minX >= _width || minY >= _height)
			continue;

		const int newW = std::min(cs, _width - minX);
		const int newH = std::min(cs, _height - minY);
		if (newW == oldW && newH == oldH)
		{
			// Same shape : the chunk is moved as is.
			world->voxel_chunks_.emplace(key, std::move(oldChunk));
			continue;
		}

		bool any = false;
		const int w = std::min(oldW, newW);
		const int h = std::min(oldH, newH);
		for (int y = 0; y < h && !any; ++y)
			for (int x = 0; x < w; ++x)
				if (oldChunk[static_cast<size_t>(y) * oldW + x].type != 0) { any = true; break; }
		if (!any)
			continue;

		auto& chunk = world->EnsureVoxelChunk(chunkX, chunkY);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				chunk[static_cast<size_t>(y) * newW + x] = oldChunk[static_cast<size_t>(y) * oldW + x];
	}

	world->dirty_chunks_.clear();
	world->render_dirty_chunks_.clear();
	world->light_dirty_chunks_.clear();
	++world->geometry_revision_;
	++world->voxel_revision_;
	return HRL_TRUE;
}

void HRL_SetVoxelTypeColor(HRL_id _sceneid, uint32_t _type, float _r, float _g, float _b, float _a)
{
	if (_type > HRL_VOXEL_TYPE_MAX)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_SetVoxelTypeColor: voxel type must be in range 0..255");
		return;
	}
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelTypeColor: invalid scene ID");
		return;
	}
	if (!std::isfinite(_r) || !std::isfinite(_g) || !std::isfinite(_b) || !std::isfinite(_a))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVoxelTypeColor: color contains NaN or infinity");
		return;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	// Base color is display color only. Values above 1 are intentionally ignored
	// instead of implicitly turning the voxel into an emitter. Emission is
	// configured independently with HRL_SetVoxelTypeEmissiveColor().
	const glm::vec4 color(
		glm::clamp(_r, 0.0f, 1.0f),
		glm::clamp(_g, 0.0f, 1.0f),
		glm::clamp(_b, 0.0f, 1.0f),
		glm::clamp(_a, 0.0f, 1.0f)
	);
	// Un changement de couleur reconstruit tous les chunks visibles : ne rien
	// invalider si la couleur est deja celle-ci.
	if (world->type_colors_[_type] == color)
		return;
	world->type_colors_[_type] = color;
	++world->color_revision_;
}

void HRL_SetVoxelRenderMode(HRL_id _sceneid, HRL_EVoxelRenderMode _mode)
{
	if (_mode != HRL_VOXEL_FLAT && _mode != HRL_VOXEL_SMOOTH && _mode != HRL_VOXEL_BLOCKY)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVoxelRenderMode: invalid render mode");
		return;
	}
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelRenderMode: invalid scene ID");
		return;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	if (world->render_mode_ == _mode)
		return;
	world->render_mode_ = _mode;
	// Toute la geometrie affichee change : reconstruction des chunks visibles.
	++world->geometry_revision_;
}

HRL_EVoxelRenderMode HRL_GetVoxelRenderMode(HRL_id _sceneid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetVoxelRenderMode: invalid scene ID");
		return HRL_VOXEL_FLAT;
	}
	const HRL_VoxelWorld* world = it->second->voxel_world;
	return world ? world->render_mode_ : HRL_VOXEL_FLAT;
}

void HRL_SetVoxelTypeEmissiveColor(HRL_id _sceneid, uint32_t _type, float _r, float _g, float _b)
{
	if (_type > HRL_VOXEL_TYPE_MAX)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_SetVoxelTypeEmissiveColor: voxel type must be in range 0..255");
		return;
	}
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelTypeEmissiveColor: invalid scene ID");
		return;
	}
	if (!std::isfinite(_r) || !std::isfinite(_g) || !std::isfinite(_b))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVoxelTypeEmissiveColor: color contains NaN or infinity");
		return;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	const glm::vec3 emissive(
		std::max(_r, 0.0f),
		std::max(_g, 0.0f),
		std::max(_b, 0.0f)
	);

	const bool wasEmissive = std::max(world->type_emissive_colors_[_type].r,
		std::max(world->type_emissive_colors_[_type].g, world->type_emissive_colors_[_type].b)) > 0.0f;
	const bool isEmissive = std::max(emissive.r, std::max(emissive.g, emissive.b)) > 0.0f;
	const glm::vec3 stored = isEmissive ? emissive : glm::vec3(0.0f);
	if (world->type_emissive_colors_[_type] == stored)
		return; // inchange : pas de reconstruction des chunks ni des emetteurs
	world->type_emissive_colors_[_type] = stored;
	if (isEmissive && !wasEmissive)
		++world->emissive_type_count_;
	else if (!isEmissive && wasEmissive && world->emissive_type_count_ > 0)
		--world->emissive_type_count_;

	// Emissive geometry and global emitter lists are rebuilt with the same
	// revision used by the baked voxel colors.
	++world->color_revision_;
}

void HRL_SetVoxelTypeTexture(HRL_id _sceneid, uint32_t _type, HRL_id _texture, float _tileVoxels)
{
	if (_type == 0 || _type > HRL_VOXEL_TYPE_MAX)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_SetVoxelTypeTexture: voxel type must be in range 1..255");
		return;
	}
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelTypeTexture: invalid scene ID");
		return;
	}
	if (!std::isfinite(_tileVoxels) || _tileVoxels <= 0.f)
		_tileVoxels = 1.f;

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	if (world->type_textures_[_type] == _texture && world->type_texture_tiles_[_type] == _tileVoxels)
		return;

	const bool layoutChanged = world->type_textures_[_type] != _texture;
	world->type_textures_[_type] = _texture;
	world->type_texture_tiles_[_type] = _tileVoxels;

	if (layoutChanged)
	{
		// Couches attribuees dans l'ordre croissant des types textures.
		int16_t layer = 0;
		world->textured_type_count_ = 0;
		for (size_t t = 0; t < world->type_textures_.size(); ++t)
		{
			if (t != 0 && world->type_textures_[t] != HRL_INVALID_ID)
			{
				world->type_texture_layers_[t] = layer++;
				++world->textured_type_count_;
			}
			else
				world->type_texture_layers_[t] = static_cast<int16_t>(-1);
		}
		// La couche est dans les sommets des chunks : reconstruction.
		++world->color_revision_;
	}
	++world->texture_revision_;
}

void HRL_SetVoxelEmissiveLighting(HRL_id _sceneid, int _enabled, float _intensity, float _falloff)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelEmissiveLighting: invalid scene ID");
		return;
	}
	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	world->emissive_lighting_enabled_ = _enabled != HRL_FALSE;
	world->emissive_light_intensity_ = std::isfinite(_intensity) ? std::max(_intensity, 0.f) : 1.f;
	world->emissive_light_falloff_ = (std::isfinite(_falloff) && _falloff > 0.f) ? std::min(_falloff, 4096.f) : 8.f;
}

int HRL_LoadVoxelWorld(HRL_id _sceneid, const HRL_Voxel* _voxels, size_t _count)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorld: invalid scene ID");
		return HRL_FALSE;
	}

	HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world || world->width_ <= 0 || world->height_ <= 0)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_LoadVoxelWorld: call HRL_SetVoxelSize before loading voxel data");
		return HRL_FALSE;
	}

	size_t expectedCount = 0;
	if (!VoxelWorldElementCount(world->width_, world->height_, expectedCount) || expectedCount != _count)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_LoadVoxelWorld: count must equal voxel world width * height");
		return HRL_FALSE;
	}
	if (expectedCount > 0 && !_voxels)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_LoadVoxelWorld: voxel array is null");
		return HRL_FALSE;
	}

	world->voxel_chunks_.clear();
	world->serialized_source_.clear();
	world->serialized_chunks_.clear();

	const int chunkSize = world->chunk_size_;
	const int chunkColumns = (world->width_ + chunkSize - 1) / chunkSize;
	const int chunkRows = (world->height_ + chunkSize - 1) / chunkSize;

	// Convert the caller's dense array into the sparse chunk representation.
	for (int chunkY = 0; chunkY < chunkRows; ++chunkY)
	{
		for (int chunkX = 0; chunkX < chunkColumns; ++chunkX)
		{
			const int minX = chunkX * chunkSize;
			const int minY = chunkY * chunkSize;
			const int chunkW = std::min(chunkSize, world->width_ - minX);
			const int chunkH = std::min(chunkSize, world->height_ - minY);

			bool nonEmpty = false;
			for (int y = 0; y < chunkH && !nonEmpty; ++y)
			{
				const size_t row = static_cast<size_t>(minY + y) * static_cast<size_t>(world->width_);
				for (int x = 0; x < chunkW; ++x)
				{
					if (_voxels[row + static_cast<size_t>(minX + x)].type != 0)
					{
						nonEmpty = true;
						break;
					}
				}
			}
			if (!nonEmpty)
				continue;

			auto& chunk = world->EnsureVoxelChunk(chunkX, chunkY);
			for (int y = 0; y < chunkH; ++y)
			{
				const size_t src = static_cast<size_t>(minY + y) * static_cast<size_t>(world->width_) +
					static_cast<size_t>(minX);
				const size_t dst = static_cast<size_t>(y) * static_cast<size_t>(chunkW);
				std::memcpy(chunk.data() + dst, _voxels + src, static_cast<size_t>(chunkW) * sizeof(HRL_Voxel));
			}
		}
	}

	world->dirty_chunks_.clear();
	world->render_dirty_chunks_.clear();
	++world->geometry_revision_;
	++world->voxel_revision_;
	return HRL_TRUE;
}

size_t HRL_GetVoxelWorldSaveAllSize(HRL_id _sceneid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetVoxelWorldSaveAllSize: invalid scene ID");
		return 0;
	}
	const HRL_VoxelWorld* world = it->second->voxel_world;
	std::vector<uint8_t> data;
	if (!VoxelSaveBuildAll(world, data))
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING,
			"HRL_GetVoxelWorldSaveAllSize: voxel world is not valid for serialization");
		return 0;
	}
	return data.size();
}

size_t HRL_SaveVoxelWorldAll(HRL_id _sceneid, void* _buffer, size_t _capacity)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SaveVoxelWorldAll: invalid scene ID");
		return 0;
	}
	const HRL_VoxelWorld* world = it->second->voxel_world;
	std::vector<uint8_t> data;
	if (!VoxelSaveBuildAll(world, data))
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING,
			"HRL_SaveVoxelWorldAll: voxel world is not valid for serialization");
		return 0;
	}
	if (!_buffer || _capacity < data.size())
		return data.size();
	std::memcpy(_buffer, data.data(), data.size());
	return data.size();
}

int HRL_LoadVoxelWorldBuffer(HRL_id _sceneid, const void* _buffer, size_t _size)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid scene ID");
		return HRL_FALSE;
	}
	if (!_buffer || _size < kVoxelWorldSaveHeaderSize)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid or empty buffer");
		return HRL_FALSE;
	}

	const auto* bytes = static_cast<const uint8_t*>(_buffer);
	VoxelSaveReader reader(bytes, _size);
	uint32_t magic = 0, width = 0, height = 0, chunkSize = 0, chunkCount = 0;
	uint16_t version = 0, flags = 0;
	if (!reader.ReadU32(magic) || !reader.ReadU16(version) || !reader.ReadU16(flags) ||
		!reader.ReadU32(width) || !reader.ReadU32(height))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: truncated header");
		return HRL_FALSE;
	}
	if (magic != kVoxelWorldSaveMagic ||
		(version != kVoxelWorldSaveVersion && version != kVoxelWorldSaveVersionWithVoxelSize) || flags != 0u)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: unsupported voxel world format");
		return HRL_FALSE;
	}
	if (version == kVoxelWorldSaveVersionWithVoxelSize)
	{
		// Ancien format : la taille de voxel enregistree est lue puis ignoree ;
		// le monde charge garde la taille actuelle de la scene.
		float ignoredVoxelSize = 0.f;
		if (_size < kVoxelWorldSaveHeaderSizeV1 || !reader.ReadF32(ignoredVoxelSize))
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: truncated header");
			return HRL_FALSE;
		}
	}
	if (!reader.ReadU32(chunkSize) || !reader.ReadU32(chunkCount))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: truncated header");
		return HRL_FALSE;
	}
	if (width == 0 || height == 0 || chunkSize == 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid world dimensions");
		return HRL_FALSE;
	}
	if (width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
		height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
		chunkSize > static_cast<uint32_t>(std::numeric_limits<int>::max()))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: world dimensions exceed HRL limits");
		return HRL_FALSE;
	}

	const uint64_t chunkColumns = (static_cast<uint64_t>(width) + chunkSize - 1u) / chunkSize;
	const uint64_t chunkRows = (static_cast<uint64_t>(height) + chunkSize - 1u) / chunkSize;
	const uint64_t totalChunks = chunkColumns * chunkRows;
	if (totalChunks == 0 || totalChunks > std::numeric_limits<uint32_t>::max() || chunkCount > totalChunks)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid chunk count");
		return HRL_FALSE;
	}

	// Keep the compressed source around.  The initial load only parses record
	// metadata; voxel payloads are decoded lazily by the renderer when a chunk
	// enters the visible streaming set.
	HRL_VoxelWorld* newWorld = new HRL_VoxelWorld();
	newWorld->width_ = static_cast<int>(width);
	newWorld->height_ = static_cast<int>(height);
	newWorld->chunk_size_ = static_cast<int>(chunkSize);
	newWorld->serialized_source_.assign(bytes, bytes + _size);

	auto readRecord = [&](VoxelSaveReader& r, bool allowDelete, bool& ok) -> uint64_t
	{
		ok = false;
		uint32_t chunkIndex = 0;
		uint8_t encodingByte = 0, bits = 0, paletteCount = 0, reserved = 0;
		uint32_t payloadSize = 0;
		if (!r.ReadU32(chunkIndex) || !r.ReadU8(encodingByte) || !r.ReadU8(bits) ||
			!r.ReadU8(paletteCount) || !r.ReadU8(reserved) || !r.ReadU32(payloadSize))
			return 0;
		if (reserved != 0u || chunkIndex >= totalChunks)
			return 0;
		if (encodingByte == kVoxelWorldDeltaDeleteEncoding)
		{
			if (!allowDelete || bits != 0 || paletteCount != 0 || payloadSize != 0)
				return 0;
		}
		else if (encodingByte > static_cast<uint8_t>(VoxelSaveEncoding::Rle))
			return 0;

		std::vector<uint8_t> palette(paletteCount);
		for (uint8_t& value : palette)
			if (!r.ReadU8(value)) return 0;
		const size_t payloadOffset = r.Position();
		const uint8_t* payload = nullptr;
		if (!r.ReadBytes(payload, payloadSize)) return 0;
		(void)payload;

		const uint32_t chunkX = chunkIndex % static_cast<uint32_t>(chunkColumns);
		const uint32_t chunkY = chunkIndex / static_cast<uint32_t>(chunkColumns);
		const int chunkW = std::min(static_cast<int>(chunkSize), static_cast<int>(width) - static_cast<int>(chunkX * chunkSize));
		const int chunkH = std::min(static_cast<int>(chunkSize), static_cast<int>(height) - static_cast<int>(chunkY * chunkSize));
		if (chunkW <= 0 || chunkH <= 0) return 0;
		if (encodingByte == static_cast<uint8_t>(VoxelSaveEncoding::Constant) &&
			(palette.size() != 1 || payloadSize != 0)) return 0;
		if (encodingByte == static_cast<uint8_t>(VoxelSaveEncoding::Raw) &&
			(!palette.empty() || payloadSize != static_cast<size_t>(chunkW) * static_cast<size_t>(chunkH))) return 0;
		if (encodingByte == static_cast<uint8_t>(VoxelSaveEncoding::Palette) &&
			(palette.empty() || bits == 0 || bits > 8 ||
			 payloadSize != (static_cast<size_t>(chunkW) * static_cast<size_t>(chunkH) * bits + 7u) / 8u)) return 0;

		const uint64_t key = HRL_VoxelWorld::MakeVoxelChunkKey(static_cast<int>(chunkX), static_cast<int>(chunkY));
		HRL_VoxelWorld::SerializedChunkRecord rec;
		rec.encoding = encodingByte;
		rec.bits = bits;
		rec.palette = std::move(palette);
		rec.payload_offset = payloadOffset;
		rec.payload_size = payloadSize;
		rec.deleted = encodingByte == kVoxelWorldDeltaDeleteEncoding;
		newWorld->serialized_chunks_[key] = std::move(rec);
		ok = true;
		return key;
	};

	for (uint32_t record = 0; record < chunkCount; ++record)
	{
		bool ok = false;
		readRecord(reader, false, ok);
		if (!ok)
		{
			delete newWorld;
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid chunk record");
			return HRL_FALSE;
		}
	}

	// A save to an existing world file is append-only: each delta contains only
	// chunks changed since the previous save. The last record for a chunk wins.
	while (reader.Remaining() != 0)
	{
		uint32_t deltaMagic = 0, deltaCount = 0;
		uint16_t deltaVersion = 0, deltaFlags = 0;
		if (!reader.ReadU32(deltaMagic) || !reader.ReadU16(deltaVersion) || !reader.ReadU16(deltaFlags) ||
			!reader.ReadU32(deltaCount) || deltaMagic != kVoxelWorldDeltaMagic ||
			deltaVersion != kVoxelWorldDeltaVersion || deltaFlags != 0u || deltaCount > totalChunks)
		{
			delete newWorld;
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid voxel delta section");
			return HRL_FALSE;
		}
		for (uint32_t record = 0; record < deltaCount; ++record)
		{
			bool ok = false;
			readRecord(reader, true, ok);
			if (!ok)
			{
				delete newWorld;
				SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_LoadVoxelWorldBuffer: invalid voxel delta record");
				return HRL_FALSE;
			}
		}
	}

	HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world)
		world = new HRL_VoxelWorld();
	const auto colors = std::move(world->type_colors_);
	const auto collisionFlags = std::move(world->type_collision_flags_);
	const auto emissiveColors = std::move(world->type_emissive_colors_);
	const uint32_t emissiveTypeCount = world->emissive_type_count_;
	const HRL_EVoxelRenderMode renderMode = world->render_mode_;
	// La taille physique n'est pas dans le fichier : on garde celle de la scene.
	const float voxelSize = world->voxel_size_;
	const uint64_t geometryRevision = world->geometry_revision_;
	const uint64_t voxelRevision = world->voxel_revision_;
	const uint64_t colorRevision = world->color_revision_;
	newWorld->type_colors_ = colors;
	newWorld->type_collision_flags_ = collisionFlags;
	newWorld->type_emissive_colors_ = emissiveColors;
	newWorld->emissive_type_count_ = emissiveTypeCount;
	newWorld->render_mode_ = renderMode;
	newWorld->voxel_size_ = voxelSize;
	newWorld->geometry_revision_ = geometryRevision + 1;
	newWorld->voxel_revision_ = voxelRevision + 1;
	newWorld->color_revision_ = colorRevision;
	// Textures of the types and emissive lighting are scene settings too :
	// a reload (ex : editor Stop restoring the level) must keep them.
	newWorld->type_textures_ = world->type_textures_;
	newWorld->type_texture_tiles_ = world->type_texture_tiles_;
	newWorld->type_texture_layers_ = world->type_texture_layers_;
	newWorld->textured_type_count_ = world->textured_type_count_;
	newWorld->texture_revision_ = world->texture_revision_ + 1;
	newWorld->emissive_lighting_enabled_ = world->emissive_lighting_enabled_;
	newWorld->emissive_light_intensity_ = world->emissive_light_intensity_;
	newWorld->emissive_light_falloff_ = world->emissive_light_falloff_;
	newWorld->dirty_chunks_.clear();
	newWorld->render_dirty_chunks_.clear();
	it->second->voxel_world = newWorld;
	delete world;
	return HRL_TRUE;
}


int HRL_SaveVoxelWorldAllFile(HRL_id _sceneid, const char* _path)
{
	if (!_path || _path[0] == '\0')
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SaveVoxelWorldAllFile: file path is null or empty");
		return HRL_FALSE;
	}

	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SaveVoxelWorldAllFile: invalid scene ID");
		return HRL_FALSE;
	}

	HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world || world->width_ <= 0 || world->height_ <= 0 || world->chunk_size_ <= 0)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING,
			"HRL_SaveVoxelWorldAllFile: voxel world is not valid for serialization");
		return HRL_FALSE;
	}

	/*
	 * Always write a complete HLV1 snapshot.
	 *
	 * The previous implementation appended HDP1 delta records when the
	 * destination already contained a matching world. That makes the file
	 * depend on the complete history of lazy chunks and dirty-chunk state.
	 * For a full-world save this is unnecessary and, more importantly, makes
	 * it very easy for a lazy chunk to be missing from the resulting snapshot.
	 *
	 * Before building the snapshot, materialize every chunk that still exists
	 * only in serialized_chunks_. This guarantees that VoxelSaveBuildAll sees
	 * the complete current world.
	 */
	for (const auto& [key, record] : world->serialized_chunks_)
	{
		if (record.deleted)
			continue;

		const int chunkX = static_cast<int32_t>(key >> 32u);
		const int chunkY = static_cast<int32_t>(key & 0xffffffffu);

		HRL_EnsureVoxelChunkLoaded(world, chunkX, chunkY);
	}

	std::vector<uint8_t> data;
	if (!VoxelSaveBuildAll(world, data))
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING,
			"HRL_SaveVoxelWorldAllFile: voxel world is not valid for serialization");
		return HRL_FALSE;
	}

	FILE* file = std::fopen(_path, "wb");
	if (!file)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_SaveVoxelWorldAllFile: could not open destination file");
		return HRL_FALSE;
	}

	const bool wroteAll =
		data.empty() || std::fwrite(data.data(), 1, data.size(), file) == data.size();

	const bool closed = std::fclose(file) == 0;

	if (!wroteAll || !closed)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_SaveVoxelWorldAllFile: file write failed");
		return HRL_FALSE;
	}

	world->dirty_chunks_.clear();
	return HRL_TRUE;
}

void HRL_BeginVoxelEdit(HRL_id _sceneid)
{
	HRL_VoxelWorld* world = FindVoxelWorld(_sceneid);
	if (!world)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_BeginVoxelEdit: invalid scene or voxel world");
		return;
	}
	++world->voxel_edit_depth_;
}

void HRL_EndVoxelEdit(HRL_id _sceneid)
{
	HRL_VoxelWorld* world = FindVoxelWorld(_sceneid);
	if (!world)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_EndVoxelEdit: invalid scene or voxel world");
		return;
	}
	if (world->voxel_edit_depth_ == 0)
		return;

	--world->voxel_edit_depth_;
	if (world->voxel_edit_depth_ == 0 && world->voxel_edit_dirty_)
	{
		++world->voxel_revision_;
		world->voxel_edit_dirty_ = false;
	}
}

void HRL_SetVoxelType(HRL_id _sceneid, int _pos_x, int _pos_y, uint32_t _type)
{
	if (_type > HRL_VOXEL_TYPE_MAX)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_SetVoxelType: voxel type must be in range 0..255");
		return;
	}

	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVoxelType: invalid scene ID");
		return;
	}

	HRL_VoxelWorld* world = FindVoxelWorld(_sceneid);
	if (!world || world->width_ <= 0 || world->height_ <= 0)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_SetVoxelType: voxel world is not loaded");
		return;
	}

	if (_pos_x < 0 || _pos_y < 0 || _pos_x >= world->width_ || _pos_y >= world->height_)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_SetVoxelType: position is outside the voxel world");
		return;
	}

	const HRL_VoxelType requested = static_cast<HRL_VoxelType>(_type);

	const int chunkX = _pos_x / world->chunk_size_;
	const int chunkY = _pos_y / world->chunk_size_;
	const uint64_t key = HRL_VoxelWorld::MakeVoxelChunkKey(chunkX, chunkY);

	// A loaded world keeps off-screen chunks in serialized_chunks_. Before any
	// write, materialize the target chunk so editing it never replaces an
	// existing lazy chunk with a newly-created empty one.
	if (!world->FindVoxelChunk(chunkX, chunkY))
		HRL_EnsureVoxelChunkLoaded(world, chunkX, chunkY);

	const HRL_VoxelType old = world->GetVoxelType(_pos_x, _pos_y);
	if (old == requested)
		return;

	// Always edit the materialized chunk directly, including when setting a
	// voxel to 0. This changes only the requested voxel.
	HRL_VoxelType* voxel = world->GetVoxelTypeMutable(_pos_x, _pos_y);
	if (!voxel)
		return;

	*voxel = requested;

	if (requested == 0)
	{
		auto* chunk = world->FindVoxelChunk(chunkX, chunkY);
		if (chunk)
		{
			bool anyNonEmpty = false;
			for (const HRL_Voxel& current : *chunk)
			{
				if (current.type != 0)
				{
					anyNonEmpty = true;
					break;
				}
			}

			if (!anyNonEmpty)
			{
				// Keep the empty chunk in voxel_chunks_. This is important:
				// removing it would make GetVoxelType/EnsureVoxelChunkLoaded
				// able to fall back to the old serialized chunk.
				auto serializedIt = world->serialized_chunks_.find(key);
				if (serializedIt != world->serialized_chunks_.end())
					serializedIt->second.deleted = true;
			}
		}
	}
	else
	{
		// The current in-memory chunk is authoritative after an edit.
		// Its old serialized record must not be used as a fallback.
		auto serializedIt = world->serialized_chunks_.find(key);
		if (serializedIt != world->serialized_chunks_.end())
			serializedIt->second.deleted = true;
	}

	MarkVoxelChunkDirty(world, _pos_x, _pos_y);
}

uint32_t HRL_GetVoxelType(HRL_id _sceneid, int _pos_x, int _pos_y)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetVoxelType: invalid scene ID");
		return 0;
	}

	HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world || world->width_ <= 0 || world->height_ <= 0)
		return 0;

	if (_pos_x < 0 || _pos_y < 0 || _pos_x >= world->width_ || _pos_y >= world->height_)
		return 0;

	HRL_EnsureVoxelChunkLoaded(world, _pos_x / world->chunk_size_, _pos_y / world->chunk_size_);
	return world->GetVoxelType(_pos_x, _pos_y);
}

void HRL_SetVoxelTypeCollisionFlags(HRL_id _sceneid, uint32_t _type, uint32_t _flags)
{
	if (_type > HRL_VOXEL_TYPE_MAX)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_SetVoxelTypeCollisionFlags: voxel type must be in range 0..255");
		return;
	}
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVoxelTypeCollisionFlags: invalid scene ID");
		return;
	}

	EnsureVoxelWorld(_sceneid);
	HRL_VoxelWorld* world = it->second->voxel_world;
	world->type_collision_flags_[_type] = _flags;
}

uint32_t HRL_GetVoxelTypeCollisionFlags(HRL_id _sceneid, uint32_t _type)
{
	if (_type > HRL_VOXEL_TYPE_MAX)
		return 0u;
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_GetVoxelTypeCollisionFlags: invalid scene ID");
		return 0u;
	}

	const HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world)
		return 0u;

	return world->type_collision_flags_[_type];
}

	static uint32_t HRL_GetVoxelCollisionFlags(const HRL_VoxelWorld* _world, HRL_VoxelType _type)
	{
		if (!_world || _type == 0)
			return 0u;
		return _world->type_collision_flags_[_type];
	}

	static void HRL_RecordVoxelCollisionType(
		uint32_t& _slot, float& _bestMetric, uint32_t _type, float _metric)
	{
		if (_type == 0)
			return;
		if (_slot == 0 || _metric < _bestMetric)
		{
			_slot = _type;
			_bestMetric = _metric;
		}
	}

int HRL_VoxelCheckCollision(
	HRL_id _sceneid,
	float _x, float _y,
	float _width, float _height,
	uint32_t _mask,
	HRL_VoxelCollision* _out_collision)
{
	if (_out_collision)
	{
		*_out_collision = HRL_VoxelCollision{};
	}

	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_VoxelCheckCollision: invalid scene ID");
		return HRL_FALSE;
	}

	const HRL_VoxelWorld* world = it->second->voxel_world;
	if (!world || world->width_ <= 0 || world->height_ <= 0)
		return HRL_FALSE;
	if (!std::isfinite(_x) || !std::isfinite(_y) ||
		!std::isfinite(_width) || !std::isfinite(_height) ||
		_width <= 0.f || _height <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_VoxelCheckCollision: position and size must be finite; size must be greater than zero");
		return HRL_FALSE;
	}
	if (!_out_collision)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_WARNING,
			"HRL_VoxelCheckCollision: output collision pointer is null");
		return HRL_FALSE;
	}

	// Coordinates are expressed in voxel-space units, independent of the
	// physical render size of a voxel. The shape is centered on (_x, _y).
	const float halfWidth = _width * 0.5f;
	const float halfHeight = _height * 0.5f;
	const float minX = _x - halfWidth;
	const float maxX = _x + halfWidth;
	const float minY = _y - halfHeight;
	const float maxY = _y + halfHeight;

	constexpr float kEpsilon = 1e-5f;

	// Expand the integer search range by one epsilon so exact face contact is
	// reported as a collision rather than being lost to floor/ceil rounding.
	int minVX = static_cast<int>(std::floor(minX - kEpsilon));
	int maxVX = static_cast<int>(std::floor(maxX + kEpsilon));
	int minVY = static_cast<int>(std::floor(minY - kEpsilon));
	int maxVY = static_cast<int>(std::floor(maxY + kEpsilon));

	minVX = std::max(0, minVX);
	minVY = std::max(0, minVY);
	maxVX = std::min(world->width_ - 1, maxVX);
	maxVY = std::min(world->height_ - 1, maxVY);
	if (minVX > maxVX || minVY > maxVY)
		return HRL_FALSE;

	float bestLeft = std::numeric_limits<float>::max();
	float bestRight = std::numeric_limits<float>::max();
	float bestTop = std::numeric_limits<float>::max();
	float bestBottom = std::numeric_limits<float>::max();
	float bestInside = std::numeric_limits<float>::max();

	for (int vy = minVY; vy <= maxVY; ++vy)
	{
		for (int vx = minVX; vx <= maxVX; ++vx)
		{
			const HRL_VoxelType type = world->GetVoxelType(vx, vy);
			const uint32_t voxelFlags = HRL_GetVoxelCollisionFlags(world, type);
			if (_mask == 0u || (voxelFlags & _mask) == 0u)
				continue;

			const float voxelMinX = static_cast<float>(vx);
			const float voxelMaxX = voxelMinX + 1.f;
			const float voxelMinY = static_cast<float>(vy);
			const float voxelMaxY = voxelMinY + 1.f;

			const float overlapX = std::min(maxX, voxelMaxX) - std::max(minX, voxelMinX);
			const float overlapY = std::min(maxY, voxelMaxY) - std::max(minY, voxelMinY);
			if (overlapX < -kEpsilon || overlapY < -kEpsilon)
				continue;

			_out_collision->flags |= HRL_VOXEL_COLLISION_NONE;

			const bool containsShape =
				voxelMinX <= minX + kEpsilon && voxelMaxX >= maxX - kEpsilon &&
				voxelMinY <= minY + kEpsilon && voxelMaxY >= maxY - kEpsilon;
			if (containsShape)
			{
				_out_collision->flags |= HRL_VOXEL_COLLISION_INSIDE;
				HRL_RecordVoxelCollisionType(_out_collision->inside_type, bestInside, type,
					std::max(0.f, std::min(overlapX, overlapY)));
				continue;
			}

			// Exact corner contact: report both axes. For real penetration, use
			// the smaller overlap as the most plausible contact axis.
			if (overlapX <= kEpsilon && overlapY <= kEpsilon)
			{
				const float centerX = voxelMinX + 0.5f;
				const float centerY = voxelMinY + 0.5f;
				if (centerX < _x)
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_LEFT;
					HRL_RecordVoxelCollisionType(_out_collision->left_type, bestLeft, type, 0.f);
				}
				else if (centerX > _x)
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_RIGHT;
					HRL_RecordVoxelCollisionType(_out_collision->right_type, bestRight, type, 0.f);
				}
				if (centerY < _y)
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_BOTTOM;
					HRL_RecordVoxelCollisionType(_out_collision->bottom_type, bestBottom, type, 0.f);
				}
				else if (centerY > _y)
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_TOP;
					HRL_RecordVoxelCollisionType(_out_collision->top_type, bestTop, type, 0.f);
				}
				continue;
			}

			const float centerX = voxelMinX + 0.5f;
			const float centerY = voxelMinY + 0.5f;
			if (overlapX <= overlapY)
			{
				if (centerX < _x)
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_LEFT;
					HRL_RecordVoxelCollisionType(_out_collision->left_type, bestLeft, type, overlapX);
				}
				else
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_RIGHT;
					HRL_RecordVoxelCollisionType(_out_collision->right_type, bestRight, type, overlapX);
				}
			}
			else
			{
				if (centerY < _y)
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_BOTTOM;
					HRL_RecordVoxelCollisionType(_out_collision->bottom_type, bestBottom, type, overlapY);
				}
				else
				{
					_out_collision->flags |= HRL_VOXEL_COLLISION_TOP;
					HRL_RecordVoxelCollisionType(_out_collision->top_type, bestTop, type, overlapY);
				}
			}
		}
	}

	return _out_collision->flags != HRL_VOXEL_COLLISION_NONE ? HRL_TRUE : HRL_FALSE;
}


static bool HRL_GetVoxelScreenRay(const HRL_Viewport* viewport, float mouseX, float mouseY,
    glm::vec3& origin, glm::vec3& direction)
{
    if (!viewport || !viewport->camera_)
        return false;

    const float winW = static_cast<float>(GetWindowWidth());
    const float winH = static_cast<float>(GetWindowHeight());
    if (winW <= 0.f || winH <= 0.f)
        return false;

    // HRL mouse coordinates are top-left origin. OpenGL viewports use a
    // bottom-left origin, so convert Y before testing the viewport and building
    // NDC coordinates.
    const float framebufferY = winH - mouseY;
    const float viewportX = viewport->x_ * winW;
    const float viewportY = viewport->y_ * winH;
    const float viewportW = std::max(1.f, viewport->width_ * winW);
    const float viewportH = std::max(1.f, viewport->height_ * winH);

    if (mouseX < viewportX || mouseX >= viewportX + viewportW ||
        framebufferY < viewportY || framebufferY >= viewportY + viewportH)
        return false;

    const float u = (mouseX - viewportX) / viewportW;
    const float v = (framebufferY - viewportY) / viewportH;
    const float ndcX = u * 2.f - 1.f;
    const float ndcY = v * 2.f - 1.f;

    const HRL_Camera* camera = viewport->camera_;
    const float aspect = viewportW / viewportH;

    glm::mat4 projection(1.f);
    if (camera->type_ == HRL_PERSPECTIVE)
    {
        projection = glm::perspective(
            glm::radians(camera->value_), aspect, camera->near_plane_, camera->far_plane_);
    }
    else
    {
        const float halfHeight = camera->value_ * 0.5f;
        const float halfWidth = halfHeight * aspect;
        projection = glm::ortho(
            -halfWidth, halfWidth, -halfHeight, halfHeight,
            camera->near_plane_, camera->far_plane_);
    }

    const glm::mat4 view = glm::lookAt(
        camera->position_,
        camera->position_ + GetForwardVector(camera->rotation_),
        GetUpVector(camera->rotation_));

    const glm::mat4 inverseVP = glm::inverse(projection * view);
    glm::vec4 nearPoint = inverseVP * glm::vec4(ndcX, ndcY, -1.f, 1.f);
    glm::vec4 farPoint  = inverseVP * glm::vec4(ndcX, ndcY,  1.f, 1.f);

    if (std::abs(nearPoint.w) < 1e-6f || std::abs(farPoint.w) < 1e-6f)
        return false;

    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;

    origin = glm::vec3(nearPoint);
    direction = glm::vec3(farPoint - nearPoint);
    const float directionLength = glm::length(direction);
    if (directionLength <= 1e-6f || !std::isfinite(directionLength))
        return false;
    direction /= directionLength;

    return true;
}


int HRL_GetVoxelAtScreenPosition(HRL_id _sceneid, int _loc_x, int _loc_y, int* _vx, int* _vy)
{
    if (_vx) *_vx = -1;
    if (_vy) *_vy = -1;

    auto sceneIt = ctx_.scenes.find(_sceneid);
    if (sceneIt == ctx_.scenes.end() || !sceneIt->second)
    {
        SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
            "HRL_GetVoxelAtScreenPosition: invalid scene ID");
        return HRL_FALSE;
    }

    hrl_scene_t* scene = sceneIt->second;
    HRL_VoxelWorld* world = scene->voxel_world;
    if (!world || world->width_ <= 0 || world->height_ <= 0 ||
        world->voxel_size_ <= 0.f)
        return HRL_FALSE;

    // A scene can have several viewports. Select the one containing the mouse.
    const HRL_Viewport* viewport = nullptr;
    const float winW = static_cast<float>(GetWindowWidth());
    const float winH = static_cast<float>(GetWindowHeight());
    for (const auto& [viewportId, candidate] : scene->viewports)
    {
        (void)viewportId;
        if (!candidate || !candidate->camera_)
            continue;

        const float viewportX = candidate->x_ * winW;
        const float viewportYTop = winH - (candidate->y_ + candidate->height_) * winH;
        const float viewportW = candidate->width_ * winW;
        const float viewportH = candidate->height_ * winH;
        if (_loc_x >= viewportX && _loc_x < viewportX + viewportW &&
            _loc_y >= viewportYTop && _loc_y < viewportYTop + viewportH)
        {
            viewport = candidate;
            break;
        }
    }

    if (!viewport)
        return HRL_FALSE;

    glm::vec3 rayOrigin(0.f);
    glm::vec3 rayDirection(0.f);
    if (!HRL_GetVoxelScreenRay(viewport, static_cast<float>(_loc_x), static_cast<float>(_loc_y),
        rayOrigin, rayDirection))
        return HRL_FALSE;

    // The voxel world is a finite rectangle in the XY plane at Z = 0.
    if (std::abs(rayDirection.z) <= 1e-6f)
        return HRL_FALSE;

    const float t = -rayOrigin.z / rayDirection.z;
    if (t < 0.f || !std::isfinite(t))
        return HRL_FALSE;

    const glm::vec3 hit = rayOrigin + rayDirection * t;
    if (!std::isfinite(hit.x) || !std::isfinite(hit.y))
        return HRL_FALSE;

    const float worldWidth = static_cast<float>(world->width_) * world->voxel_size_;
    const float worldHeight = static_cast<float>(world->height_) * world->voxel_size_;
    if (hit.x < 0.f || hit.y < 0.f || hit.x >= worldWidth || hit.y >= worldHeight)
        return HRL_FALSE;

    const int vx = static_cast<int>(std::floor(hit.x / world->voxel_size_));
    const int vy = static_cast<int>(std::floor(hit.y / world->voxel_size_));
    if (vx < 0 || vy < 0 || vx >= world->width_ || vy >= world->height_)
        return HRL_FALSE;

    if (_vx) *_vx = vx;
    if (_vy) *_vy = vy;
    return HRL_TRUE;
}
