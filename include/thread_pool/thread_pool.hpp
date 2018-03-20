#pragma once

#include <thread_pool/fixed_function.hpp>
#include <thread_pool/mpmc_bounded_queue.hpp>
#include <thread_pool/slotted_bag.hpp>
#include <thread_pool/thread_pool_options.hpp>
#include <thread_pool/worker.hpp>
#include <thread_pool/rouser.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <vector>
#include <chrono>

namespace tp
{

template <typename Task, template<typename> class Queue>
class GenericThreadPool;
using ThreadPool = GenericThreadPool<FixedFunction<void(), 128>, MPMCBoundedQueue>;

/**
 * @brief The ThreadPool class implements thread pool pattern.
 * It is highly scalable and fast.
 * It is header only.
 * It implements both work-stealing and work-distribution balancing
 * strategies.
 * It implements cooperative scheduling strategy for tasks.
 * Idle CPU utilization is constant with increased worker count.
 */
template <typename Task, template<typename> class Queue>
class GenericThreadPool final
{
    using WorkerVector = std::vector<std::unique_ptr<Worker<Task, Queue>>>;

public:
    /**
     * @brief ThreadPool Construct and start new thread pool.
     * @param options Creation options.
     */
    explicit GenericThreadPool(ThreadPoolOptions options = ThreadPoolOptions());

    /**
    * @brief Copy ctor implementation.
    */
    GenericThreadPool(GenericThreadPool const&) = delete;

    /**
    * @brief Copy assignment implementation.
    */
    GenericThreadPool& operator=(GenericThreadPool const& rhs) = delete;

    /**
    * @brief Move ctor implementation.
    * @note Be very careful when invoking this while the thread pool is 
    * active, or in an otherwise undefined state.
    */
    GenericThreadPool(GenericThreadPool&& rhs) noexcept;

    /**
    * @brief Move assignment implementaion.v
    * @note Be very careful when invoking this while the thread pool is 
    * active, or in an otherwise undefined state.
    */
    GenericThreadPool& operator=(GenericThreadPool&& rhs) noexcept;

    /**
     * @brief ~ThreadPool Stop all workers and destroy thread pool.
     */
    ~GenericThreadPool();

    /**
     * @brief post Try post job to thread pool.
     * @param handler Handler to be called from thread pool worker. It has
     * to be callable as 'handler()'.
     * @return 'true' on success, false otherwise.
     * @note All exceptions thrown by handler will be suppressed.
     */
    template <typename Handler>
    bool tryPost(Handler&& handler);

    /**
     * @brief post Post job to thread pool.
     * @param handler Handler to be called from thread pool worker. It has
     * to be callable as 'handler()'.
     * @throw std::overflow_error if worker's queue is full.
     * @note All exceptions thrown by handler will be suppressed.
     */
    template <typename Handler>
    void post(Handler&& handler);

private:
    /**
    * @brief getWorker Obtain a reference to the local thread's associated worker,
    * otherwise return the next worker in the round robin.
    */
    Worker<Task, Queue>& getWorker();

    SlottedBag<Queue> m_idle_workers;
    WorkerVector m_workers;
    Rouser m_rouser;
    std::atomic<size_t> m_next_worker;
    std::atomic<size_t> m_num_busy_waiters;
};


/// Implementation

template <typename Task, template<typename> class Queue>
inline GenericThreadPool<Task, Queue>::GenericThreadPool(ThreadPoolOptions options)
    : m_idle_workers(options.threadCount())
    , m_workers(options.threadCount())
    , m_rouser(options.rousePeriod())
    , m_next_worker(0)
    , m_num_busy_waiters(0)
{
    // Instatiate all workers.
    for (auto it = m_workers.begin(); it != m_workers.end(); ++it)
        it->reset(new Worker<Task, Queue>(options.busyWaitOptions(), options.queueSize()));

    // Initialize all worker threads.
    for (size_t i = 0; i < m_workers.size(); ++i)
        m_workers[i]->start(i, m_workers, m_idle_workers, m_num_busy_waiters);

    m_rouser.start(m_workers, m_idle_workers, m_num_busy_waiters);
}

template <typename Task, template<typename> class Queue>
inline GenericThreadPool<Task, Queue>::GenericThreadPool(GenericThreadPool&& rhs) noexcept
{
    *this = std::move(rhs);
}

template <typename Task, template<typename> class Queue>
inline GenericThreadPool<Task, Queue>& GenericThreadPool<Task, Queue>::operator=(GenericThreadPool&& rhs) noexcept
{
    if (this != &rhs)
    {
        m_idle_workers = std::move(rhs.m_idle_workers);
        m_workers = std::move(rhs.m_workers);
        m_rouser = std::move(rhs.m_rouser);
        m_next_worker = rhs.m_next_worker.load();
        m_num_busy_waiters = rhs.m_num_busy_waiters.load();
    }

    return *this;
}

template <typename Task, template<typename> class Queue>
inline GenericThreadPool<Task, Queue>::~GenericThreadPool()
{
    m_rouser.stop();

    for (auto& worker_ptr : m_workers)
        worker_ptr->stop();
}

template <typename Task, template<typename> class Queue>
template <typename Handler>
inline bool GenericThreadPool<Task, Queue>::tryPost(Handler&& handler)
{
    // This section of the code increases the probability that our thread pool
    // is fully utilized (num active workers = argmin(num tasks, num total workers)).
    // If there aren't busy waiters, let's see if we have any idling threads. 
    // These incur higher overhead to wake up than the busy waiters.
    if (m_num_busy_waiters.load(std::memory_order_acquire) == 0)
    {
        auto result = m_idle_workers.tryEmptyAny();
        if (result.first)
        {
            if (m_workers[result.second]->tryPost(std::forward<Handler>(handler)))
            {
                m_workers[result.second]->wake();
                return true;
            }

            // If post is unsuccessful, we need to re-add the worker to the idle worker bag.
            m_idle_workers.fill(result.second);
            return false;
        }
    }

    // No idle threads. Our threads are either active or busy waiting
    // Either way, submit the work item in a round robin fashion.
    if (!getWorker().tryPost(std::forward<Handler>(handler)))
        return false; // Worker's task queue is full.

    // The following section increases the probability that tasks will not be dropped.
    // This is a soft constraint, the strict task dropping bound is covered by the Rouser
    // thread's functionality. This code experimentally lowers task response time under
    // low thread pool utilization without incurring significant performance penalties at
    // high thread pool utilization.
    if (m_num_busy_waiters.load(std::memory_order_acquire) == 0)
    {
        auto result = m_idle_workers.tryEmptyAny();
        if (result.first)
            m_workers[result.second]->wake();
    }

    return true;
}

template <typename Task, template<typename> class Queue>
template <typename Handler>
inline void GenericThreadPool<Task, Queue>::post(Handler&& handler)
{
    const auto ok = tryPost(std::forward<Handler>(handler));
    if (!ok)
        throw std::runtime_error("Thread pool queue is full.");
}

template <typename Task, template<typename> class Queue>
inline Worker<Task, Queue>& GenericThreadPool<Task, Queue>::getWorker()
{
    auto id = Worker<Task, Queue>::getWorkerIdForCurrentThread();

    if (id > m_workers.size())
        id = m_next_worker.fetch_add(1, std::memory_order_relaxed) % m_workers.size();

    return *m_workers[id];
}

}
