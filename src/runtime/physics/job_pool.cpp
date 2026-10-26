#include "job_pool.h"

#include <Jolt/Core/FPException.h>

#include <algorithm>
#include <string>

#if defined(__linux__)
#include <pthread.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace projv::runtime {
    JobPool::JobPool(JPH::uint maxJobs, JPH::uint maxBarriers, int threads) {
        JobSystemWithBarrier::Init(maxBarriers);
        jobs.Init(maxJobs, maxJobs);
        ring.resize(maxJobs, nullptr);
        threads = std::max(threads, 1);
        workers.reserve(static_cast<size_t>(threads));
        for (int i = 0; i < threads; i++) workers.emplace_back([this, i] { work(i); });
    }

    JobPool::~JobPool() {
        {
            std::lock_guard guard(lock);
            quit = true;
        }
        ready.notify_all();
        for (std::thread& t : workers) t.join();
        // Anything still queued runs here, as Jolt's pool does: a job's dependants and its barrier
        // are waiting on it.
        for (; count > 0; count--) {
            Job* job = ring[head];
            head = (head + 1) % ring.size();
            job->Execute();
            job->Release();
        }
    }

    JPH::JobHandle JobPool::CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                                      JPH::uint32 dependencies) {
        JPH::uint32 index = jobs.ConstructObject(name, color, this, function, dependencies);
        // Out of jobs means maxJobs is too small for the step; Jolt's pool asserts and retries too.
        while (index == JPH::FixedSizeFreeList<Job>::cInvalidObjectIndex) {
            JPH_ASSERT(false, "No jobs available!");
            std::this_thread::yield();
            index = jobs.ConstructObject(name, color, this, function, dependencies);
        }
        Job* job = &jobs.Get(index);
        JobHandle handle(job);
        if (dependencies == 0) QueueJob(job);
        return handle;
    }

    void JobPool::QueueJob(Job* job) { QueueJobs(&job, 1); }

    void JobPool::QueueJobs(Job** queued, JPH::uint n) {
        {
            std::lock_guard guard(lock);
            for (JPH::uint i = 0; i < n; i++) {
                queued[i]->AddRef();   // the queue's reference, dropped once a worker has run it
                ring[(head + count) % ring.size()] = queued[i];
                count++;
            }
        }
        if (n == 1) ready.notify_one();
        else ready.notify_all();
    }

    void JobPool::FreeJob(Job* job) { jobs.DestructObject(job); }

    void JobPool::work(int index) {
#if defined(__linux__)
        std::string name = "physics " + std::to_string(index + 1);
        pthread_setname_np(pthread_self(), name.c_str());
#elif defined(_WIN32)
        const std::wstring name = L"physics " + std::to_wstring(index + 1);
        SetThreadDescription(GetCurrentThread(), name.c_str());
#else
        (void)index;
#endif
        JPH::FPExceptionsEnable exceptions;   // as Jolt's own workers have them (debug builds only)
        (void)exceptions;
        for (;;) {
            Job* job;
            {
                std::unique_lock guard(lock);
                ready.wait(guard, [this] { return quit || count > 0; });
                if (count == 0) return;   // quitting, and nothing left
                job = ring[head];
                head = (head + 1) % ring.size();
                count--;
            }
            // A job a barrier has already run (the waiting thread takes them too) does nothing here.
            job->Execute();
            job->Release();
        }
    }
}
