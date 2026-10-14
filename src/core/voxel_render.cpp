#include "voxel_render.h"
#include "utils_functions.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

// Sommet voxel compact : 28 octets au lieu de 40 (couche de texture incluse). Le monde voxel est dans le
// plan Z=0 avec une normale constante (0,0,1) : la position est envoyee en 2
// composantes (OpenGL complete z=0, w=1) et la normale passe par une valeur
// d'attribut generique (glVertexAttrib3f) au lieu d'etre dupliquee par sommet.
// Le shader voxel est inchange.

// Couche de texture d'un type (-1 : couleur seule).
float GetVoxelTypeTextureLayer(const HRL_VoxelWorld* world, uint32_t type)
{
	if (!world || type == 0 || type > HRL_VOXEL_TYPE_MAX)
		return -1.f;
	return static_cast<float>(world->type_texture_layers_[type]);
}

uint64_t MakeVoxelChunkKey(int chunkX, int chunkY)
{
	return (static_cast<uint64_t>(static_cast<uint32_t>(chunkX)) << 32u) |
		static_cast<uint32_t>(chunkY);
}

int VoxelChunkXFromKey(uint64_t key)
{
	return static_cast<int>(static_cast<uint32_t>(key >> 32u));
}

int VoxelChunkYFromKey(uint64_t key)
{
	return static_cast<int>(static_cast<uint32_t>(key & 0xffffffffu));
}

glm::mat4 CalculateVoxelViewportProjection(const HRL_Viewport* viewport)
{
	if (!viewport || !viewport->camera_)
		return glm::mat4(1.f);
	const float viewportWidth = std::max(1.f, static_cast<float>(GetWindowWidth()) * viewport->width_);
	const float viewportHeight = std::max(1.f, static_cast<float>(GetWindowHeight()) * viewport->height_);
	const float aspect = viewportWidth / viewportHeight;
	const HRL_Camera* camera = viewport->camera_;
	if (camera->type_ == HRL_PERSPECTIVE)
	{
		return glm::perspective(glm::radians(camera->value_), aspect, camera->near_plane_, camera->far_plane_);
	}
	const float halfHeight = camera->value_ * 0.5f;
	const float halfWidth = halfHeight * aspect;
	return glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight,
		camera->near_plane_, camera->far_plane_);
}

glm::mat4 CalculateVoxelViewportView(const HRL_Viewport* viewport)
{
	if (!viewport || !viewport->camera_)
		return glm::mat4(1.f);
	const HRL_Camera* camera = viewport->camera_;
	return glm::lookAt(
		camera->position_,
		camera->position_ + GetForwardVector(camera->rotation_),
		GetUpVector(camera->rotation_)
	);
}

// Rectangle (monde) du plan Z=0 vu par la camera d'un viewport.
bool ComputeVoxelViewWorldRect(const HRL_VoxelWorld* world, const HRL_Viewport* viewport,
	float& minWorldX, float& minWorldY, float& maxWorldX, float& maxWorldY)
{
	if (!world || world->width_ <= 0 || world->height_ <= 0 || world->chunk_size_ <= 0 ||
		world->voxel_size_ <= 0.f || !viewport || !viewport->camera_)
		return false;

	const glm::mat4 vp = CalculateVoxelViewportProjection(viewport) * CalculateVoxelViewportView(viewport);
	const glm::mat4 invVp = glm::inverse(vp);

	// The voxel world is the finite XY rectangle at Z=0. Intersect the camera
	// frustum edges with that plane instead of sampling screen points: this is
	// exact for the convex frustum and prevents loading unrelated chunks when
	// the camera sees the world at a steep angle.
	const float ndcX[2] = {-1.f, 1.f};
	const float ndcY[2] = {-1.f, 1.f};
	const float ndcZ[2] = {-1.f, 1.f};
	glm::vec3 corners[8];
	int cornerIndex = 0;
	for (float z : ndcZ)
	{
		for (float y : ndcY)
		{
			for (float x : ndcX)
			{
				glm::vec4 point = invVp * glm::vec4(x, y, z, 1.f);
				if (std::abs(point.w) < 1e-7f)
					return false;
				point /= point.w;
				corners[cornerIndex++] = glm::vec3(point);
			}
		}
	}

	minWorldX = std::numeric_limits<float>::max();
	minWorldY = std::numeric_limits<float>::max();
	maxWorldX = std::numeric_limits<float>::lowest();
	maxWorldY = std::numeric_limits<float>::lowest();
	bool foundIntersection = false;

	auto includePoint = [&](const glm::vec3& point)
	{
		if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
			return;
		if (std::abs(point.z) > 1e-4f)
			return;
		minWorldX = std::min(minWorldX, point.x);
		minWorldY = std::min(minWorldY, point.y);
		maxWorldX = std::max(maxWorldX, point.x);
		maxWorldY = std::max(maxWorldY, point.y);
		foundIntersection = true;
	};

	for (const glm::vec3& corner : corners)
		includePoint(corner);

	static const int edges[12][2] = {
		{0, 1}, {0, 2}, {1, 3}, {2, 3},
		{4, 5}, {4, 6}, {5, 7}, {6, 7},
		{0, 4}, {1, 5}, {2, 6}, {3, 7}
	};
	for (const auto& edge : edges)
	{
		const glm::vec3& a = corners[edge[0]];
		const glm::vec3& b = corners[edge[1]];
		const float dz = b.z - a.z;
		if (std::abs(dz) < 1e-7f)
			continue;
		const float t = -a.z / dz;
		if (t < -1e-5f || t > 1.f + 1e-5f)
			continue;
		includePoint(glm::mix(a, b, glm::clamp(t, 0.f, 1.f)));
	}

	if (!foundIntersection)
		return false;
	return true;
}

// Portee (voxels) de la lumiere des voxels emissifs : 0 si elle est coupee.
float VoxelLightReachVoxels(const HRL_VoxelWorld* world)
{
	if (!world || !world->emissive_lighting_enabled_ || world->emissive_type_count_ == 0 ||
		world->emissive_light_intensity_ <= 0.f)
		return 0.f;
	return std::min(world->emissive_light_falloff_ * 3.5f, 1024.f);
}

// Chunks charges autour de la vue : au moins un, plus si la lumiere des
// emetteurs porte plus loin (ils doivent etre charges pour eclairer).
int VoxelStreamMarginChunks(const HRL_VoxelWorld* world)
{
	const float reach = VoxelLightReachVoxels(world);
	if (!world || world->chunk_size_ <= 0 || reach <= 0.f)
		return VOXEL_CHUNK_STREAM_MARGIN;
	const int chunks = static_cast<int>(std::ceil(reach / static_cast<float>(world->chunk_size_)));
	return std::clamp(chunks, VOXEL_CHUNK_STREAM_MARGIN, 8);
}

bool CollectVoxelVisibleChunksForViewport(const HRL_VoxelWorld* world,
	const HRL_Viewport* viewport, std::unordered_set<uint64_t>& desired)
{
	if (!world || world->width_ <= 0 || world->height_ <= 0 || world->chunk_size_ <= 0 ||
		world->voxel_size_ <= 0.f || !viewport || !viewport->camera_)
		return false;

	float minWorldX, minWorldY, maxWorldX, maxWorldY;
	if (!ComputeVoxelViewWorldRect(world, viewport, minWorldX, minWorldY, maxWorldX, maxWorldY))
		return false;

	const float worldWidth = static_cast<float>(world->width_) * world->voxel_size_;
	const float worldHeight = static_cast<float>(world->height_) * world->voxel_size_;
	if (maxWorldX < 0.f || maxWorldY < 0.f || minWorldX > worldWidth || minWorldY > worldHeight)
		return false;

	const int chunkCountX = (world->width_ + world->chunk_size_ - 1) / world->chunk_size_;
	const int chunkCountY = (world->height_ + world->chunk_size_ - 1) / world->chunk_size_;
	if (chunkCountX <= 0 || chunkCountY <= 0)
		return false;

	const float minVoxelX = std::max(0.f, minWorldX) / world->voxel_size_;
	const float minVoxelY = std::max(0.f, minWorldY) / world->voxel_size_;
	const float maxVoxelX = std::min(worldWidth, maxWorldX) / world->voxel_size_;
	const float maxVoxelY = std::min(worldHeight, maxWorldY) / world->voxel_size_;

	int minChunkX = static_cast<int>(std::floor(minVoxelX / static_cast<float>(world->chunk_size_)));
	int minChunkY = static_cast<int>(std::floor(minVoxelY / static_cast<float>(world->chunk_size_)));
	int maxChunkX = static_cast<int>(std::floor(maxVoxelX / static_cast<float>(world->chunk_size_)));
	int maxChunkY = static_cast<int>(std::floor(maxVoxelY / static_cast<float>(world->chunk_size_)));

	const int margin = VoxelStreamMarginChunks(world);
	minChunkX = std::max(0, minChunkX - margin);
	minChunkY = std::max(0, minChunkY - margin);
	maxChunkX = std::min(chunkCountX - 1, maxChunkX + margin);
	maxChunkY = std::min(chunkCountY - 1, maxChunkY + margin);
	if (minChunkX > maxChunkX || minChunkY > maxChunkY)
		return false;

	for (int chunkY = minChunkY; chunkY <= maxChunkY; ++chunkY)
		for (int chunkX = minChunkX; chunkX <= maxChunkX; ++chunkX)
			desired.insert(MakeVoxelChunkKey(chunkX, chunkY));
	return true;
}

glm::vec4 GetVoxelTypeColor(const HRL_VoxelWorld* world, uint32_t type)
{
    if (!world || type == 0)
        return glm::vec4(1.f);
    if (type > HRL_VOXEL_TYPE_MAX)
        return glm::vec4(1.f);
    const glm::vec4 color = world->type_colors_[type];
    return glm::vec4(
        glm::clamp(color.r, 0.f, 1.f),
        glm::clamp(color.g, 0.f, 1.f),
        glm::clamp(color.b, 0.f, 1.f),
        glm::clamp(color.a, 0.f, 1.f));
}

glm::vec3 GetVoxelTypeEmissiveColor(const HRL_VoxelWorld* world, uint32_t type)
{
    if (!world || type == 0)
        return glm::vec3(0.f);
    if (type > HRL_VOXEL_TYPE_MAX)
        return glm::vec3(0.f);
    return glm::max(world->type_emissive_colors_[type], glm::vec3(0.f));
}

static void AppendVoxelQuad(std::vector<HRL_VoxelMeshVertex>& vertices,
    std::vector<HRL_uint>& indices, float x0, float x1, float y0, float y1,
    const glm::vec4& color, float layer)
{
    const HRL_uint base = static_cast<HRL_uint>(vertices.size());
    const float rgba[4] = {color.r, color.g, color.b, color.a};
    const float positions[4][3] = {
        {x0, y0, 0.f}, {x1, y0, 0.f}, {x1, y1, 0.f}, {x0, y1, 0.f}
    };
    for (const auto& position : positions)
    {
        HRL_VoxelMeshVertex vertex{};
        vertex.position[0] = position[0];
        vertex.position[1] = position[1];
        std::copy(std::begin(rgba), std::end(rgba), std::begin(vertex.color));
        vertex.layer = layer;
        vertices.push_back(vertex);
    }
    indices.insert(indices.end(), {base, base + 1u, base + 2u, base + 2u, base + 3u, base});
}

// ---------------------------------------------------------------------------
// Modes de surface SMOOTH / BLOCKY (HRL_SetVoxelRenderMode)
// ---------------------------------------------------------------------------
// Les deux modes ont besoin des voxels voisins, y compris ceux des chunks
// adjacents : on copie les types dans une grille locale entouree d'une bordure
// d'un voxel. Hors du monde, on recopie le voxel de bord le plus proche : les
// bords du monde restent droits (pas de biseau ni de pente contre le cadre).

struct VoxelPoint { float x; float y; };

struct VoxelPaddedGrid
{
    int originX = 0; // coordonnee globale de la colonne 0 de la grille
    int originY = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> types;

    uint8_t At(int gx, int gy) const
    {
        return types[static_cast<size_t>(gy - originY) * static_cast<size_t>(width) +
            static_cast<size_t>(gx - originX)];
    }
};

static void FillVoxelPaddedGrid(const HRL_VoxelWorld* world, int x0, int y0, int x1, int y1, VoxelPaddedGrid& grid)
{
    // Grille couvrant les voxels globaux [x0, x1) x [y0, y1).
    grid.originX = x0;
    grid.originY = y0;
    grid.width = x1 - x0;
    grid.height = y1 - y0;
    grid.types.assign(static_cast<size_t>(grid.width) * static_cast<size_t>(grid.height), 0u);

    const int cs = world->chunk_size_;
    for (int gy = std::max(0, y0); gy < std::min(world->height_, y1); ++gy)
    {
        int cachedChunkX = std::numeric_limits<int>::min();
        const HRL_VoxelWorld::VoxelChunk* chunk = nullptr;
        const int cy = gy / cs;
        const int ly = gy - cy * cs;
        int chunkW = 0;
        for (int gx = std::max(0, x0); gx < std::min(world->width_, x1); ++gx)
        {
            const int cx = gx / cs;
            if (cx != cachedChunkX)
            {
                // Au plus 3 chunks par ligne : une recherche par changement de chunk.
                cachedChunkX = cx;
                chunk = world->FindVoxelChunk(cx, cy);
                chunkW = std::min(cs, world->width_ - cx * cs);
            }
            if (!chunk)
                continue;
            const size_t index = static_cast<size_t>(ly) * static_cast<size_t>(chunkW) +
                static_cast<size_t>(gx - cx * cs);
            if (index < chunk->size())
                grid.types[static_cast<size_t>(gy - y0) * static_cast<size_t>(grid.width) +
                    static_cast<size_t>(gx - x0)] = (*chunk)[index].type;
        }
    }

    // Echantillons hors du monde : copie du voxel de bord (coordonnees bornees).
    // La position bornee est toujours dans la grille (bordure d'un voxel).
    for (int gy = y0; gy < y1; ++gy)
    {
        for (int gx = x0; gx < x1; ++gx)
        {
            if (gx >= 0 && gy >= 0 && gx < world->width_ && gy < world->height_)
                continue;
            const int cx = std::clamp(gx, std::max(0, x0), std::min(world->width_, x1) - 1);
            const int cy = std::clamp(gy, std::max(0, y0), std::min(world->height_, y1) - 1);
            grid.types[static_cast<size_t>(gy - y0) * static_cast<size_t>(grid.width) + static_cast<size_t>(gx - x0)] =
                grid.types[static_cast<size_t>(cy - y0) * static_cast<size_t>(grid.width) + static_cast<size_t>(cx - x0)];
        }
    }
}

static int ClipConvexToBox(const VoxelPoint* in, int count, float minX, float minY, float maxX, float maxY, VoxelPoint* out);

static void AppendVoxelPolygon(std::vector<HRL_VoxelMeshVertex>& vertices, std::vector<HRL_uint>& indices,
    const VoxelPoint* points, int count, const glm::vec4& color, float layer)
{
    if (count < 3)
        return;
    const HRL_uint base = static_cast<HRL_uint>(vertices.size());
    for (int i = 0; i < count; ++i)
    {
        HRL_VoxelMeshVertex vertex{};
        vertex.position[0] = points[i].x;
        vertex.position[1] = points[i].y;
        vertex.color[0] = color.r;
        vertex.color[1] = color.g;
        vertex.color[2] = color.b;
        vertex.color[3] = color.a;
        vertex.layer = layer;
        vertices.push_back(vertex);
    }
    // Polygones convexes : triangulation en eventail.
    for (int i = 1; i + 1 < count; ++i)
        indices.insert(indices.end(), {base, base + static_cast<HRL_uint>(i), base + static_cast<HRL_uint>(i + 1)});
}

// Ecrit un polygone (coordonnees monde) dans le maillage de base et, si le type
// est emissif, dans le maillage emissif (memes regles que le mode FLAT).
struct VoxelMeshSink
{
    const HRL_VoxelWorld* world;
    std::vector<HRL_VoxelMeshVertex>& vertices;
    std::vector<HRL_uint>& indices;
    std::vector<HRL_VoxelMeshVertex>& emissiveVertices;
    std::vector<HRL_uint>& emissiveIndices;
    // Decoupe au rectangle du monde (cellules SMOOTH a cheval sur le bord).
    bool clipToWorld = false;
    float worldMaxX = 0.f;
    float worldMaxY = 0.f;

    void Emit(uint32_t type, const VoxelPoint* inPoints, int inCount)
    {
        VoxelPoint clipped[16];
        const VoxelPoint* points = inPoints;
        int count = inCount;
        if (clipToWorld)
        {
            count = ClipConvexToBox(inPoints, inCount, 0.f, 0.f, worldMaxX, worldMaxY, clipped);
            points = clipped;
            if (count < 3)
                return;
        }
        const glm::vec4 baseColor = GetVoxelTypeColor(world, type);
        const float layer = GetVoxelTypeTextureLayer(world, type);
        AppendVoxelPolygon(vertices, indices, points, count, baseColor, layer);
        const glm::vec3 emissive = GetVoxelTypeEmissiveColor(world, type);
        if (baseColor.a > 0.001f && std::max(emissive.r, std::max(emissive.g, emissive.b)) > 0.f)
            AppendVoxelPolygon(emissiveVertices, emissiveIndices, points, count, glm::vec4(emissive, baseColor.a), layer);
    }

    void EmitRect(uint32_t type, float x0, float x1, float y0, float y1)
    {
        const VoxelPoint quad[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        Emit(type, quad, 4);
    }
};

// Fusion gloutonne de cellules pleines de meme type en rectangles (comme le
// mode FLAT) : l'interieur du terrain reste a 2 triangles par rectangle.
static void EmitMergedRects(const std::vector<uint8_t>& mergeTypes, int w, int h,
    float originX, float originY, float cellSize, VoxelMeshSink& sink)
{
    static std::vector<uint8_t> used;
    used.assign(mergeTypes.size(), 0u);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const size_t start = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            const uint8_t type = mergeTypes[start];
            if (type == 0 || used[start])
                continue;
            int rectW = 1;
            while (x + rectW < w)
            {
                const size_t idx = start + static_cast<size_t>(rectW);
                if (used[idx] || mergeTypes[idx] != type)
                    break;
                ++rectW;
            }
            int rectH = 1;
            while (y + rectH < h)
            {
                bool canExtend = true;
                for (int xx = x; xx < x + rectW; ++xx)
                {
                    const size_t idx = static_cast<size_t>(y + rectH) * static_cast<size_t>(w) + static_cast<size_t>(xx);
                    if (used[idx] || mergeTypes[idx] != type)
                    {
                        canExtend = false;
                        break;
                    }
                }
                if (!canExtend)
                    break;
                ++rectH;
            }
            for (int yy = y; yy < y + rectH; ++yy)
                for (int xx = x; xx < x + rectW; ++xx)
                    used[static_cast<size_t>(yy) * static_cast<size_t>(w) + static_cast<size_t>(xx)] = 1u;
            sink.EmitRect(type,
                originX + static_cast<float>(x) * cellSize, originX + static_cast<float>(x + rectW) * cellSize,
                originY + static_cast<float>(y) * cellSize, originY + static_cast<float>(y + rectH) * cellSize);
        }
    }
}

// BLOCKY : chaque coin dont l'un des deux cotes est expose (voisin vide) est
// coupe a 45 degres sur 25 % de la largeur du voxel. Un cote expose est donc
// plat sur ses 50 % centraux et en pente sur 25 % a chaque extremite.
// Entre deux types pleins, la regle est symetrique : une frontiere droite reste
// droite, et seul un coin "en escalier" (ses deux voisins de cote, autour de ce
// coin, sont d'un autre type) est chanfreine ; le triangle retire est alors
// rempli par le type voisin, il n'y a donc jamais de trou entre deux types.
static void BuildVoxelChunkBlocky(const HRL_VoxelWorld* world, const VoxelPaddedGrid& grid,
    int startX, int startY, int localW, int localH, VoxelMeshSink& sink)
{
    const float vs = world->voxel_size_;
    const float cut = 0.25f * vs;
    static std::vector<uint8_t> interior;
    interior.assign(static_cast<size_t>(localW) * static_cast<size_t>(localH), 0u);

    // Decide si le coin d'un voxel de type t, dont les voisins de cote autour
    // de ce coin sont 'vertical' et 'horizontal', est coupe et a qui revient
    // le triangle retire (0 = vide).
    struct CornerCut { bool cut; uint8_t owner; };
    auto corner = [](uint8_t t, uint8_t vertical, uint8_t horizontal) -> CornerCut {
        if (vertical == 0 || horizontal == 0)
            return {true, 0u};
        if (vertical != t && horizontal != t)
            return {true, vertical}; // voisins de types differents : priorite au vertical
        return {false, 0u};
    };

    for (int ly = 0; ly < localH; ++ly)
    {
        for (int lx = 0; lx < localW; ++lx)
        {
            const int gx = startX + lx;
            const int gy = startY + ly;
            const uint8_t type = grid.At(gx, gy);
            if (type == 0)
                continue;
            const uint8_t left = grid.At(gx - 1, gy);
            const uint8_t right = grid.At(gx + 1, gy);
            const uint8_t bottom = grid.At(gx, gy - 1);
            const uint8_t top = grid.At(gx, gy + 1);
            const CornerCut bl = corner(type, bottom, left);
            const CornerCut br = corner(type, bottom, right);
            const CornerCut tr = corner(type, top, right);
            const CornerCut tl = corner(type, top, left);
            if (!bl.cut && !br.cut && !tr.cut && !tl.cut)
            {
                // Aucun coin coupe : carre plein, fusionne avec ses voisins.
                interior[static_cast<size_t>(ly) * static_cast<size_t>(localW) + static_cast<size_t>(lx)] = type;
                continue;
            }

            const float x0 = static_cast<float>(gx) * vs;
            const float y0 = static_cast<float>(gy) * vs;
            const float x1 = x0 + vs;
            const float y1 = y0 + vs;
            VoxelPoint points[8];
            int n = 0;
            // Parcours CCW : bas-gauche, bas-droite, haut-droite, haut-gauche.
            if (bl.cut) { points[n++] = {x0, y0 + cut}; points[n++] = {x0 + cut, y0}; }
            else points[n++] = {x0, y0};
            if (br.cut) { points[n++] = {x1 - cut, y0}; points[n++] = {x1, y0 + cut}; }
            else points[n++] = {x1, y0};
            if (tr.cut) { points[n++] = {x1, y1 - cut}; points[n++] = {x1 - cut, y1}; }
            else points[n++] = {x1, y1};
            if (tl.cut) { points[n++] = {x0 + cut, y1}; points[n++] = {x0, y1 - cut}; }
            else points[n++] = {x0, y1};
            sink.Emit(type, points, n);

            // Triangles des coins coupes contre un autre type : remplis par lui.
            if (bl.owner) { const VoxelPoint t3[3] = {{x0, y0}, {x0 + cut, y0}, {x0, y0 + cut}}; sink.Emit(bl.owner, t3, 3); }
            if (br.owner) { const VoxelPoint t3[3] = {{x1, y0}, {x1, y0 + cut}, {x1 - cut, y0}}; sink.Emit(br.owner, t3, 3); }
            if (tr.owner) { const VoxelPoint t3[3] = {{x1, y1}, {x1 - cut, y1}, {x1, y1 - cut}}; sink.Emit(tr.owner, t3, 3); }
            if (tl.owner) { const VoxelPoint t3[3] = {{x0, y1}, {x0, y1 - cut}, {x0 + cut, y1}}; sink.Emit(tl.owner, t3, 3); }
        }
    }

    EmitMergedRects(interior, localW, localH,
        static_cast<float>(startX) * vs, static_cast<float>(startY) * vs, vs, sink);
}

// Decoupe un polygone convexe par une boite alignee (Sutherland-Hodgman).
static int ClipConvexToBox(const VoxelPoint* in, int count, float minX, float minY, float maxX, float maxY, VoxelPoint* out)
{
    VoxelPoint bufferA[16];
    VoxelPoint bufferB[16];
    int n = std::min(count, 16);
    for (int i = 0; i < n; ++i)
        bufferA[i] = in[i];
    VoxelPoint* src = bufferA;
    VoxelPoint* dst = bufferB;
    for (int plane = 0; plane < 4 && n > 0; ++plane)
    {
        auto inside = [&](const VoxelPoint& p) {
            switch (plane)
            {
            case 0: return p.x >= minX;
            case 1: return p.x <= maxX;
            case 2: return p.y >= minY;
            default: return p.y <= maxY;
            }
        };
        auto intersect = [&](const VoxelPoint& a, const VoxelPoint& b) {
            float t = 0.f;
            switch (plane)
            {
            case 0: t = (minX - a.x) / (b.x - a.x); break;
            case 1: t = (maxX - a.x) / (b.x - a.x); break;
            case 2: t = (minY - a.y) / (b.y - a.y); break;
            default: t = (maxY - a.y) / (b.y - a.y); break;
            }
            return VoxelPoint{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
        };
        int m = 0;
        for (int i = 0; i < n && m < 15; ++i)
        {
            const VoxelPoint& cur = src[i];
            const VoxelPoint& prev = src[(i + n - 1) % n];
            const bool curIn = inside(cur);
            const bool prevIn = inside(prev);
            if (curIn)
            {
                if (!prevIn) dst[m++] = intersect(prev, cur);
                dst[m++] = cur;
            }
            else if (prevIn)
                dst[m++] = intersect(prev, cur);
        }
        std::swap(src, dst);
        n = m;
    }
    for (int i = 0; i < n; ++i)
        out[i] = src[i];
    return n;
}

// SMOOTH : marching squares sur une grille dont les echantillons sont les
// centres des voxels. Le contour passe au milieu de deux centres, c'est-a-dire
// exactement sur la frontiere des voxels : les surfaces planes ne bougent pas,
// les escaliers deviennent des pentes a 45 degres.
static void BuildVoxelChunkSmooth(const HRL_VoxelWorld* world, const VoxelPaddedGrid& grid,
    int startX, int startY, int endX, int endY, VoxelMeshSink& sink)
{
    const float vs = world->voxel_size_;
    // Une cellule (i, j) a pour coins les centres des voxels (i..i+1, j..j+1).
    // Ce chunk possede les cellules i dans [startX - 1, endX - 1), plus la
    // derniere colonne (endX - 1) s'il touche le bord droit du monde ; idem en Y.
    // L'union sur tous les chunks couvre le monde sans recouvrement.
    const int cellX0 = startX - 1;
    const int cellY0 = startY - 1;
    const int cellX1 = endX - 1 + (endX >= world->width_ ? 1 : 0);
    const int cellY1 = endY - 1 + (endY >= world->height_ ? 1 : 0);
    const int cellsW = cellX1 - cellX0;
    const int cellsH = cellY1 - cellY0;
    if (cellsW <= 0 || cellsH <= 0)
        return;

    static std::vector<uint8_t> uniform;
    uniform.assign(static_cast<size_t>(cellsW) * static_cast<size_t>(cellsH), 0u);

    for (int j = cellY0; j < cellY1; ++j)
    {
        for (int i = cellX0; i < cellX1; ++i)
        {
            // Coins dans l'ordre CCW : bas-gauche, bas-droite, haut-droite, haut-gauche.
            const uint8_t t[4] = { grid.At(i, j), grid.At(i + 1, j), grid.At(i + 1, j + 1), grid.At(i, j + 1) };
            const int mask = (t[0] ? 1 : 0) | (t[1] ? 2 : 0) | (t[2] ? 4 : 0) | (t[3] ? 8 : 0);
            if (mask == 0)
                continue;
            if (mask == 15 && t[0] == t[1] && t[1] == t[2] && t[2] == t[3])
            {
                uniform[static_cast<size_t>(j - cellY0) * static_cast<size_t>(cellsW) +
                    static_cast<size_t>(i - cellX0)] = t[0];
                continue;
            }

            const float ox = (static_cast<float>(i) + 0.5f) * vs;
            const float oy = (static_cast<float>(j) + 0.5f) * vs;
            const float h = 0.5f * vs;
            const VoxelPoint corner[4] = {{ox, oy}, {ox + vs, oy}, {ox + vs, oy + vs}, {ox, oy + vs}};
            const VoxelPoint mid[4] = {{ox + h, oy}, {ox + vs, oy + h}, {ox + h, oy + vs}, {ox, oy + h}}; // bas, droite, haut, gauche

            // Polygones (convexes) de la partie pleine de la cellule.
            VoxelPoint polys[2][8];
            int polyCount[2] = {0, 0};
            int numPolys = 0;
            if (mask == 5 || mask == 10)
            {
                // Selle : deux coins opposes pleins. On garde deux triangles
                // separes (des voxels en diagonale ne se touchent que par un coin).
                for (int k = 0; k < 4; ++k)
                {
                    if (!t[k])
                        continue;
                    VoxelPoint* p = polys[numPolys];
                    p[0] = corner[k];
                    p[1] = mid[k];               // arete sortante du coin k
                    p[2] = mid[(k + 3) % 4];     // arete entrante du coin k
                    polyCount[numPolys++] = 3;
                }
            }
            else
            {
                // Parcours CCW du bord de la cellule : coin plein -> sommet,
                // changement plein/vide sur une arete -> milieu de l'arete.
                VoxelPoint* p = polys[0];
                int n = 0;
                for (int k = 0; k < 4; ++k)
                {
                    if (t[k])
                        p[n++] = corner[k];
                    if ((t[k] != 0) != (t[(k + 1) % 4] != 0))
                        p[n++] = mid[k];
                }
                polyCount[0] = n;
                numPolys = 1;
            }

            // Un seul type parmi les coins pleins : polygones emis tels quels.
            uint8_t singleType = 0;
            bool mixed = false;
            for (int k = 0; k < 4; ++k)
            {
                if (!t[k]) continue;
                if (singleType == 0) singleType = t[k];
                else if (singleType != t[k]) mixed = true;
            }
            if (!mixed)
            {
                for (int k = 0; k < numPolys; ++k)
                    sink.Emit(singleType, polys[k], polyCount[k]);
                continue;
            }

            // Plusieurs types : la frontiere entre deux types suit le meme
            // contour que la frontiere avec le vide (diagonales a 45 degres).
            // Chaque quart de cellule (le quart du voxel q le plus proche du
            // coin commun aux 4 voxels) est coupe par sa diagonale en :
            //  - un triangle "interieur" (cote centre du voxel) -> type de q ;
            //  - un triangle "exterieur" (cote coin commun), attribue comme le
            //    ferait le marching squares binaire, generalise aux types :
            //    q le garde s'il a un voisin de cote du meme type ; sinon, si
            //    ses deux voisins de cote sont du meme type, ce type le prend
            //    (le coin isole de q est chanfreine) ; sinon q le garde s'il est
            //    plein et touche de la matiere ; un quart vide entre deux
            //    voisins pleins est comble (priorite au voisin vertical).
            // Avec un seul type, cette regle redonne exactement le contour du
            // marching squares ; la silhouette exterieure ne change donc pas.
            const VoxelPoint center{ox + h, oy + h};
            for (int q = 0; q < 4; ++q)
            {
                const uint8_t tq = t[q];
                const uint8_t vertical = t[q ^ 3];
                const uint8_t horizontal = t[q ^ 1];
                uint8_t outer = 0;
                if (tq && (vertical == tq || horizontal == tq))
                    outer = tq;
                else if (vertical && vertical == horizontal)
                    outer = vertical;
                else if (tq && (vertical || horizontal))
                    outer = tq;
                else if (!tq && vertical && horizontal)
                    outer = vertical;

                const VoxelPoint& c = corner[q];
                const VoxelPoint& m0 = mid[q];
                const VoxelPoint& m1 = mid[(q + 3) % 4];
                if (tq && outer == tq)
                {
                    const VoxelPoint quad[4] = {c, m0, center, m1};
                    sink.Emit(tq, quad, 4);
                    continue;
                }
                if (tq)
                {
                    const VoxelPoint inner[3] = {c, m0, m1};
                    sink.Emit(tq, inner, 3);
                }
                if (outer)
                {
                    const VoxelPoint outerTri[3] = {m0, center, m1};
                    sink.Emit(outer, outerTri, 3);
                }
            }
        }
    }

    EmitMergedRects(uniform, cellsW, cellsH,
        (static_cast<float>(cellX0) + 0.5f) * vs, (static_cast<float>(cellY0) + 0.5f) * vs, vs, sink);
}

static bool BuildVoxelChunkGeometryShaped(const HRL_VoxelWorld* world, int chunkX, int chunkY,
    std::vector<HRL_VoxelMeshVertex>& vertices, std::vector<HRL_uint>& indices,
    std::vector<HRL_VoxelMeshVertex>& emissiveVertices, std::vector<HRL_uint>& emissiveIndices)
{
    const int startX = chunkX * world->chunk_size_;
    const int startY = chunkY * world->chunk_size_;
    if (startX < 0 || startY < 0 || startX >= world->width_ || startY >= world->height_)
        return false;
    const int endX = std::min(startX + world->chunk_size_, world->width_);
    const int endY = std::min(startY + world->chunk_size_, world->height_);

    static VoxelPaddedGrid grid;
    FillVoxelPaddedGrid(world, startX - 1, startY - 1, endX + 1, endY + 1, grid);

    VoxelMeshSink sink{world, vertices, indices, emissiveVertices, emissiveIndices};
    // Seules les cellules SMOOTH des chunks au bord du monde peuvent deborder.
    sink.clipToWorld = world->render_mode_ == HRL_VOXEL_SMOOTH &&
        (startX == 0 || startY == 0 || endX >= world->width_ || endY >= world->height_);
    sink.worldMaxX = static_cast<float>(world->width_) * world->voxel_size_;
    sink.worldMaxY = static_cast<float>(world->height_) * world->voxel_size_;
    if (world->render_mode_ == HRL_VOXEL_BLOCKY)
        BuildVoxelChunkBlocky(world, grid, startX, startY, endX - startX, endY - startY, sink);
    else
        BuildVoxelChunkSmooth(world, grid, startX, startY, endX, endY, sink);
    return true;
}

bool BuildVoxelChunkGeometry(const HRL_VoxelWorld* world, int chunkX, int chunkY,
    std::vector<HRL_VoxelMeshVertex>& vertices, std::vector<HRL_uint>& indices,
    std::vector<HRL_VoxelMeshVertex>& emissiveVertices, std::vector<HRL_uint>& emissiveIndices)
{
    vertices.clear();
    indices.clear();
    emissiveVertices.clear();
    emissiveIndices.clear();
    if (!world || world->chunk_size_ <= 0 || world->width_ <= 0 || world->height_ <= 0)
        return false;

    if (world->render_mode_ != HRL_VOXEL_FLAT)
        return BuildVoxelChunkGeometryShaped(world, chunkX, chunkY,
            vertices, indices, emissiveVertices, emissiveIndices);

    // Missing sparse chunks are completely empty.
    if (!world->FindVoxelChunk(chunkX, chunkY))
        return true;

    const int startX = chunkX * world->chunk_size_;
    const int startY = chunkY * world->chunk_size_;
    if (startX < 0 || startY < 0 || startX >= world->width_ || startY >= world->height_)
        return false;
    const int localWidth = std::min(world->chunk_size_, world->width_ - startX);
    const int localHeight = std::min(world->chunk_size_, world->height_ - startY);
    if (localWidth <= 0 || localHeight <= 0)
        return true;

    const size_t cellCount = static_cast<size_t>(localWidth) * static_cast<size_t>(localHeight);
    // Tampon reutilise entre les reconstructions (thread GL unique).
    static std::vector<uint8_t> used;
    used.assign(cellCount, 0u);
    const size_t reserveCells = std::min(cellCount, static_cast<size_t>(1024));
    vertices.reserve(reserveCells * 2u + 4u);
    indices.reserve(reserveCells * 3u + 6u);

    auto localIndex = [localWidth](int x, int y) -> size_t {
        return static_cast<size_t>(y) * static_cast<size_t>(localWidth) + static_cast<size_t>(x);
    };
    const HRL_VoxelWorld::VoxelChunk* chunk = world->FindVoxelChunk(chunkX, chunkY);
    if (!chunk)
        return true;

    if (chunk->size() < cellCount)
        return false;
    // Toutes les lectures restent dans ce chunk : indexation locale directe
    // (l'ancienne version refaisait deux divisions par voxel lu).
    const HRL_Voxel* cells = chunk->data();
    auto voxelAt = [cells, startX, startY, localWidth](int x, int y) -> uint32_t {
        return cells[static_cast<size_t>(y - startY) * static_cast<size_t>(localWidth) +
            static_cast<size_t>(x - startX)].type;
    };

    for (int y = 0; y < localHeight; ++y)
    {
        for (int x = 0; x < localWidth; ++x)
        {
            const size_t startIndex = localIndex(x, y);
            if (used[startIndex])
                continue;
            const uint32_t type = voxelAt(startX + x, startY + y);
            if (type == 0)
            {
                used[startIndex] = 1u;
                continue;
            }

            int rectWidth = 1;
            while (x + rectWidth < localWidth)
            {
                const size_t idx = localIndex(x + rectWidth, y);
                if (used[idx] || voxelAt(startX + x + rectWidth, startY + y) != type)
                    break;
                ++rectWidth;
            }

            int rectHeight = 1;
            while (y + rectHeight < localHeight)
            {
                bool canExtend = true;
                for (int xx = x; xx < x + rectWidth; ++xx)
                {
                    const size_t idx = localIndex(xx, y + rectHeight);
                    if (used[idx] || voxelAt(startX + xx, startY + y + rectHeight) != type)
                    {
                        canExtend = false;
                        break;
                    }
                }
                if (!canExtend)
                    break;
                ++rectHeight;
            }

            for (int yy = y; yy < y + rectHeight; ++yy)
                for (int xx = x; xx < x + rectWidth; ++xx)
                    used[localIndex(xx, yy)] = 1u;

            const float x0 = static_cast<float>(startX + x) * world->voxel_size_;
            const float x1 = static_cast<float>(startX + x + rectWidth) * world->voxel_size_;
            const float y0 = static_cast<float>(startY + y) * world->voxel_size_;
            const float y1 = static_cast<float>(startY + y + rectHeight) * world->voxel_size_;
            const glm::vec4 baseColor = GetVoxelTypeColor(world, type);
            const float layer = GetVoxelTypeTextureLayer(world, type);
            AppendVoxelQuad(vertices, indices, x0, x1, y0, y1, baseColor, layer);

            const glm::vec3 emissive = GetVoxelTypeEmissiveColor(world, type);
            if (baseColor.a > 0.001f && std::max(emissive.r, std::max(emissive.g, emissive.b)) > 0.f)
                AppendVoxelQuad(emissiveVertices, emissiveIndices,
                    x0, x1, y0, y1, glm::vec4(emissive, baseColor.a), layer);
        }
    }
    return true;
}

