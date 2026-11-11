// HRL native Vulkan backend - worker threads implementation.
#include "vk_jobs.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace hrlvk
{
namespace
{
struct Pool
{
	std::mutex mutex;
	std::condition_variable wake, done;
	std::vector<std::thread> workers;
	bool started = false;
	bool stop = false;

	// Current job. `generation` changes for each ParallelFor call.
	uint64_t generation = 0;
	const std::function<void(size_t, size_t)>* fn = nullptr;
	size_t count = 0, chunk = 1;
	std::atomic<size_t> next{0};
	unsigned active = 0; // workers still inside the current job

	void RunChunks()
	{
		for (;;)
		{
			const size_t begin = next.fetch_add(chunk);
			if (begin >= count) return;
			(*fn)(begin, std::min(begin + chunk, count));
		}
	}

	void WorkerLoop()
	{
		uint64_t seen = 0;
		std::unique_lock<std::mutex> lock(mutex);
		for (;;)
		{
			wake.wait(lock, [&] { return stop || generation != seen; });
			if (stop) return;
			seen = generation;
			++active;
			lock.unlock();
			RunChunks();
			lock.lock();
			if (--active == 0) done.notify_all();
		}
	}
};
// Never destroyed: if the application exits without HRL_Shutdown(), the parked
// workers must not outlive the object they wait on.
Pool& g_pool = *new Pool;

unsigned WantedThreads()
{
	unsigned n = std::thread::hardware_concurrency();
	if (const char* env = std::getenv("HRL_VULKAN_THREADS"))
		if (env[0]) n = (unsigned)std::max(1, std::atoi(env));
	return std::clamp(n, 1u, 8u);
}
} // namespace

unsigned JobThreadCount() { return WantedThreads(); }

void ParallelFor(size_t count, size_t minChunk, const std::function<void(size_t, size_t)>& fn)
{
	if (count == 0) return;
	const unsigned threads = WantedThreads();
	minChunk = std::max<size_t>(minChunk, 1);
	if (threads <= 1 || count < minChunk * 2)
	{
		fn(0, count);
		return;
	}

	std::unique_lock<std::mutex> lock(g_pool.mutex);
	if (!g_pool.started)
	{
		g_pool.stop = false;
		for (unsigned i = 1; i < threads; ++i)
			g_pool.workers.emplace_back([] { g_pool.WorkerLoop(); });
		g_pool.started = true;
	}
	// A worker that woke up after the previous job ended may still be reading
	// the (empty) job description: let it leave before writing a new one.
	g_pool.done.wait(lock, [] { return g_pool.active == 0; });
	g_pool.fn = &fn;
	g_pool.count = count;
	// About four chunks per thread: balances uneven work without contention.
	g_pool.chunk = std::max(minChunk, count / (size_t(threads) * 4));
	g_pool.next.store(0);
	++g_pool.generation;
	lock.unlock();
	g_pool.wake.notify_all();

	g_pool.RunChunks();

	// Wait for the workers still inside the job.
	lock.lock();
	g_pool.done.wait(lock, [] { return g_pool.active == 0; });
	g_pool.count = 0; // late wakers see an empty job
	g_pool.fn = nullptr;
}

void JobsShutdown()
{
	std::unique_lock<std::mutex> lock(g_pool.mutex);
	if (!g_pool.started) return;
	g_pool.stop = true;
	lock.unlock();
	g_pool.wake.notify_all();
	for (std::thread& t : g_pool.workers) t.join();
	g_pool.workers.clear();
	g_pool.started = false;
}
} // namespace hrlvk
