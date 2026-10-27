#pragma once

// The worker threads PhysicsWorld steps Jolt with: a JPH::JobSystem of our own, in place of Jolt's
// JobSystemThreadPool. Private to src/runtime/physics/ -- it names Jolt types.
//
// Why not Jolt's. Its pool queues jobs in a ring that each worker walks with a head of its own, and
// a slot is only free again once *every* head has passed it. Workers sleep on one shared semaphore,
// and a wake-up goes to whichever thread asks first: a worker already running takes it again and
// again, while one asleep on an idle core misses it and never moves its head. A thousand jobs later
// the ring reads as full, and the step that queues the next job spins -- waking everyone, sleeping
// 100 us, trying again -- until that worker happens to run. On a 24-core hybrid CPU with 7 workers
// that was a stall of 50-700 ms every few seconds, in a scene with nothing awake; with 2 or 3
// workers it was not seen. Here there is one queue under one mutex, so a worker that wakes always
// takes a real job and none can fall behind; and a condition variable checked under that mutex
// cannot lose a wake-up. Physics queues a few dozen jobs a step, so the lock costs nothing.
//
// Which thread runs a job does not change what it computes: Jolt is deterministic whatever the job
// system, so the thread count stays out of everything that feeds a step.

#include <Jolt/Jolt.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace projv::runtime {
    class JobPool final : public JPH::JobSystemWithBarrier {
    public:
        // `threads` workers (at least 1); the thread that waits on a barrier runs jobs too.
        JobPool(JPH::uint maxJobs, JPH::uint maxBarriers, int threads);
        ~JobPool() override;

        int GetMaxConcurrency() const override { return static_cast<int>(workers.size()) + 1; }
        JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                            JPH::uint32 dependencies = 0) override;

    protected:
        void QueueJob(Job* job) override;
        void QueueJobs(Job** jobs, JPH::uint count) override;
        void FreeJob(Job* job) override;

    private:
        void work(int index);
        // Takes the oldest queued job, runs it and lets go of the queue's reference, as a worker does; false
        // when the queue is empty. What CreateJob does when it is out of slots.
        bool runQueuedJob();

        JPH::FixedSizeFreeList<Job> jobs;
        // A ring as long as the most jobs that can exist at once: each job is queued at most once,
        // so it never fills.
        std::vector<Job*> ring;
        size_t head = 0, count = 0;
        bool quit = false;
        std::mutex lock;
        std::condition_variable ready;
        std::vector<std::thread> workers;
    };
}
