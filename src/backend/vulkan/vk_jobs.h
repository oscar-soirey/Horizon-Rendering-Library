// HRL native Vulkan backend - worker threads for the CPU side of a frame
// (culling, LOD selection).
#ifndef HRL_VK_JOBS_H
#define HRL_VK_JOBS_H

#include <cstddef>
#include <functional>

namespace hrlvk
{
// Calls fn(begin, end) on sub-ranges covering [0, count), on the worker
// threads and the calling thread, and returns once every range is done.
// Ranges hold at least `minChunk` items; small counts run on the caller only.
// fn must be safe to call concurrently on disjoint ranges.
// Thread count: hardware threads (at most 8), or HRL_VULKAN_THREADS.
void ParallelFor(size_t count, size_t minChunk, const std::function<void(size_t, size_t)>& fn);
// Number of threads ParallelFor uses (calling thread included).
unsigned JobThreadCount();
// Stops the worker threads (they restart on the next ParallelFor).
void JobsShutdown();
} // namespace hrlvk

#endif
