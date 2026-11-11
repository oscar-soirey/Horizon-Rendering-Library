// Internal header of the HRL frontend (src/frontend/*.cpp). Not part of the public API.
#ifndef HRL_FRONTEND_INTERNAL_H
#define HRL_FRONTEND_INTERNAL_H

#include "../hrl.h"
#include "../hrl_gl.h"
#include "../hrl_vulkan.h"

#include "../core/backend_vtable.h"
#include "../core/object_types.h"
#include "../core/utils_functions.h"
#include "../core/widgets.h"

// Backends. HRL_DISABLE_OPENGL33 leaves the desktop OpenGL backend out (mobile
// builds); HRL_ENABLE_GLES3 adds the OpenGL ES 3.0 one (HRL_OPENGL_ES_30).
#ifndef HRL_DISABLE_OPENGL33
#include "../backend/opengl33/gl33_backend.h"
#endif
#ifdef HRL_ENABLE_GLES3
#include "../backend/gles3/gles3_backend.h"
#endif
// HRL_ENABLE_VULKAN (CMake option COMPILE_VULKAN) adds the Vulkan backend.
#ifdef HRL_ENABLE_VULKAN
#include "../backend/vulkan/vulkan_backend.h"
#endif

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>
#include <array>
#include <vector>
#include <cmath>
#include <new>
#include <cstdio>
#include <cstdarg>
#include <limits>
#include <set>
#include <tuple>
#include <cstring>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

// Internal FBX decoding dependency. Add ufbx to the build; HRL only consumes
// its public header here and converts the decoded data to HRL_Vertex3D.
#include <ufbx/ufbx.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

//pour le texte
#include <stb/stb_truetype.h>
#include <stb/stb_image.h>


// ---- Asynchronous resource loading -------------------------------------------
struct AsyncResourceResult
{
    enum class Type { Texture, Shader };
    Type type = Type::Texture;
    HRL_id id = HRL_INVALID_ID;
    BitmapResult bitmap{};
    std::string vert;
    std::string frag;
    std::string error;
    bool success = false;
};

struct AsyncResourceTask
{
    AsyncResourceResult::Type type = AsyncResourceResult::Type::Texture;
    HRL_id id = HRL_INVALID_ID;
    std::vector<unsigned char> bytes;
    std::string vert;
    std::string frag;
};

// A browser page without shared memory has no threads to give (Emscripten
// without -pthread): resources are then decoded on the calling thread.
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define HRL_NO_THREADS 1
#endif

class AsyncResourceLoader
{
public:
    void Start()
    {
#ifdef HRL_NO_THREADS
        return; // tasks run inside Enqueue()
#else
        std::lock_guard<std::mutex> lock(mutex_);
        if (!workers_.empty()) return;
        stop_ = false;
        unsigned int hw = std::thread::hardware_concurrency();
        unsigned int count = hw > 1 ? hw - 1 : 1;
        count = std::max(1u, std::min(count, 4u));
        workers_.reserve(count);
        for (unsigned int i = 0; i < count; ++i)
            workers_.emplace_back([this]{ WorkerLoop(); });
#endif
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_)
            if (worker.joinable()) worker.join();
        workers_.clear();
    }

    void Enqueue(AsyncResourceTask task)
    {
#ifdef HRL_NO_THREADS
        // No worker threads (a browser page): do the work now. The result is
        // still picked up by the main loop, like one coming from a worker.
        AsyncResourceResult result = Process(task);
        std::lock_guard<std::mutex> lock(result_mutex_);
        results_.push(std::move(result));
        return;
#endif
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(task));
        }
        condition_.notify_one();
    }

    bool TryPopResult(AsyncResourceResult& out)
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        if (results_.empty()) return false;
        out = std::move(results_.front());
        results_.pop();
        return true;
    }

    bool HasPendingWork() const
    {
        bool pending = false;
        { std::lock_guard<std::mutex> lock(mutex_); pending = !queue_.empty() || active_workers_ != 0; }
        if (pending) return true;
        { std::lock_guard<std::mutex> lock(result_mutex_); return !results_.empty(); }
    }

private:
    static bool DecodeTexture(const std::vector<unsigned char>& bytes, BitmapResult& bmp, std::string& error)
    {
        if (bytes.empty() || bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        { error = "Async texture load: invalid image buffer"; return false; }
        int w = 0, h = 0, channels = 0;
        stbi_uc* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, STBI_rgb_alpha);
        if (!data || w <= 0 || h <= 0)
        { error = "Async texture load: image decode failed"; stbi_image_free(data); return false; }
        bmp.width = w;
        bmp.height = h;
        bmp.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
        std::memcpy(bmp.pixels.data(), data, bmp.pixels.size());
        stbi_image_free(data);
        return true;
    }

    static AsyncResourceResult Process(AsyncResourceTask& task)
    {
        AsyncResourceResult result;
        result.type = task.type;
        result.id = task.id;
        if (task.type == AsyncResourceResult::Type::Texture)
        {
            result.success = DecodeTexture(task.bytes, result.bitmap, result.error);
        }
        else
        {
            result.vert = std::move(task.vert);
            result.frag = std::move(task.frag);
            result.success = !result.vert.empty() && !result.frag.empty();
            if (!result.success) result.error = "Async shader load: empty vertex or fragment source";
        }
        return result;
    }

    void WorkerLoop()
    {
        for (;;)
        {
            AsyncResourceTask task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]{ return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop();
                ++active_workers_;
            }

            AsyncResourceResult result = Process(task);

            {
                std::lock_guard<std::mutex> lock(result_mutex_);
                results_.push(std::move(result));
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --active_workers_;
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<AsyncResourceTask> queue_;
    std::vector<std::thread> workers_;
    unsigned int active_workers_ = 0;
    bool stop_ = false;
    mutable std::mutex result_mutex_;
    std::queue<AsyncResourceResult> results_;
};

extern AsyncResourceLoader g_AsyncResourceLoader;
enum class AsyncResourceState : int { Pending = 0, Ready = 1, Failed = 2, Cancelled = 3 };

// ---- Frontend state --------------------------------------------------------------
// Every object created through the public API.
extern HRL_Context ctx_;
// Backend selected by HRL_Init(); never modified afterwards.
extern HRL_vtable g_Backend;


/**
 * Owns the ufbx scene kept alive for the duration of the public FBX resource API.
 * HRL_FBXTextureInfo/HRL_FBXMaterialInfo intentionally expose borrowed strings/data
 * from this scene, so callers must keep this object alive while using those pointers.
 */
struct HRL_FBXResources
{
	ufbx_scene* scene = nullptr;
	std::vector<HRL_FBXTextureInfo> textures;
	std::vector<HRL_FBXMaterialInfo> materials;
	std::vector<const ufbx_material*> material_sources;
	std::unordered_map<const ufbx_texture*, HRL_uint> texture_indices;
};


// Ecrit un parametre de materiau. Retourne false si la valeur est inchangee :
// beaucoup d'applications envoient leurs uniforms (temps, couleur...) a chaque
// frame, ce qui reinitialisait jusqu'ici tout le cache de sondes GI.
template <typename Map, typename Value>
static bool AssignMaterialParam(Map& params, const char* name, const Value& value)
{
	if (!name)
		return false;
	auto [it, inserted] = params.try_emplace(name, value);
	if (inserted)
		return true;
	if (it->second == value)
		return false;
	it->second = value;
	return true;
}

// ---- Helpers shared by several frontend files -------------------------------------
glm::vec3 HRLFBX_NormalizeOrFallback(const glm::vec3& value, const glm::vec3& fallback);
void HRLFBX_BuildFallbackTangentSpace( const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec2& uv0, const glm::vec2& uv1, const glm::vec2& uv2, const glm::vec3& normal, glm::vec3& tangent, glm::vec3& bitangent);
void MarkAllScenesGILightingDirty();
void MarkSceneGIGeometryDirty(hrl_scene_t* scene);
// Regenerates the LODs 1.. of a mesh from LOD 0 (CPU data only).
bool GenerateMeshLODs(HRL_Mesh* mesh, HRL_uint levels);
// World position / normal of a landscape at a world (x, z); false outside.
bool SampleLandscape(const HRL_Landscape* landscape, float worldX, float worldZ, glm::vec3* position, glm::vec3* normal);
// Renderer caches (see hrl_scene_t::mesh_revision / moved_meshes).
void MarkMeshMoved(HRL_id id, HRL_Mesh* mesh);
void MarkSceneMeshesChanged(HRL_id scene);
void MarkSceneGILightingDirty(hrl_scene_t* scene);
void MarkScenesUsingMaterialGILightingDirty(HRL_id materialId);
void ProcessAsyncResourceUploads();
void ProcessGizmoInput();
void SimulateVFXSystem(HRL_VFXSystem* system, float dt);
void UpdateMaterialIntFlags(HRL_Material* material, const char* name, int value);
void UpdateSceneLights(HRL_id _scene);
// 2D overlays of a scene for the backends that implement RHI_RenderSceneFrame (overlay.cpp).
void BuildSceneOverlays(const hrl_scene_t* scene, std::vector<HRL_ViewportOverlay>& out);
void EndOverlayFrame();
void ReleaseOverlays();

#endif
