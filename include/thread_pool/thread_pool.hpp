#pragma once

#include <thread_pool/fixed_function.hpp>
#include <thread_pool/mpmc_bounded_queue.hpp>
#include <thread_pool/slotted_bag.hpp>
#include <thread_pool/thread_pool_options.hpp>
#include <thread_pool/worker.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <vector>

namespace tp
{

template <typename Task, template<typename> class Queue>
class ThreadPoolImpl;
using ThreadPool = ThreadPoolImpl<FixedFunction<void(), 128>,
                                    MPMCBoundedQueue>;

/**
 * @brief The ThreadPool class implements thread pool pattern.
 * It is highly scalable and fast.
 * It is header only.
 * It implements both work-stealing and work-distribution balancing
 * startegies.
 * It implements cooperative scheduling strategy for tasks.
 */
template <typename Task, template<typename> class Queue>
class ThreadPoolImpl {

    using WorkerVector = std::vector<std::unique_ptr<Worker<Task, Queue>>>;

public:
    /**
     * @brief ThreadPool Construct and start new thread pool.
     * @param options Creation options.
     */
    explicit ThreadPoolImpl(ThreadPoolOptions&& options = ThreadPoolOptions());

    /**
     * @brief Move ctor implementation.
     */
    ThreadPoolImpl(ThreadPoolImpl&& rhs) noexcept;

    /**
     * @brief ~ThreadPool Stop all workers and destroy thread pool.
     */
    ~ThreadPoolImpl();

    /**
     * @brief Move assignment implementaion.
     */
    ThreadPoolImpl& operator=(ThreadPoolImpl&& rhs) noexcept;

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
s    */
    Worker<Task, Queue>& getWorker();

    SlottedBag<Queue> m_idle_workers;
    WorkerVector m_workers;
    std::atomic<size_t> m_next_worker;
    std::atomic<size_t> m_num_busy_waiters;
};


/// Implementation

template <typename Task, template<typename> class Queue>
inline ThreadPoolImpl<Task, Queue>::ThreadPoolImpl(ThreadPoolOptions&& options)
    : m_idle_workers(options.threadCount())
    , m_workers(options.threadCount())
    , m_next_worker(0)
    , m_num_busy_waiters(0)
{
    // Instatiate all workers.
    for (auto it = m_workers.begin(); it != m_workers.end(); ++it)
        it->reset(new Worker<Task, Queue>(options.busyWaitOptions(), options.queueSize()));

    // Initialize all worker threads.
    for (size_t i = 0; i < m_workers.size(); ++i)
        m_workers[i]->start(i, &m_workers, &m_idle_workers, &m_num_busy_waiters);
}

template <typename Task, template<typename> class Queue>
inline ThreadPoolImpl<Task, Queue>::ThreadPoolImpl(ThreadPoolImpl<Task, Queue>&& rhs) noexcept
{
    *this = rhs;
}

template <typename Task, template<typename> class Queue>
inline ThreadPoolImpl<Task, Queue>::~ThreadPoolImpl()
{
    for (auto& worker_ptr : m_workers)
    {
        worker_ptr->stop();
    }
}

template <typename Task, template<typename> class Queue>
inline ThreadPoolImpl<Task, Queue>&
ThreadPoolImpl<Task, Queue>::operator=(ThreadPoolImpl<Task, Queue>&& rhs) noexcept
{
    if (this != &rhs)
    {
        m_workers = std::move(rhs.m_workers);
        m_next_worker = rhs.m_next_worker.load();
    }
    return *this;
}

template <typename Task, template<typename> class Queue>
template <typename Handler>
inline bool ThreadPoolImpl<Task, Queue>::tryPost(Handler&& handler)
{
    size_t idle_worker_id;

    // If there aren't busy waiters, let's see if we have any idling threads. 
    // These incur higher overhead to wake up than the busy waiters.
    if (m_num_busy_waiters.load(std::memory_order_acquire) == 0 && m_idle_workers.tryEmptyAny(idle_worker_id))
    {
        auto success = m_workers[idle_worker_id]->tryPost(std::forward<Handler>(handler));
        m_workers[idle_worker_id]->wake();

        return success;
    }

    // No idle threads. Our threads are either active or busy waiting
    // Either way, submit the work item in a round robin fashion.
    if (!getWorker().tryPost(std::forward<Handler>(handler)))
        return false; // Worker's task queue is full.

    // We have to ensure that at least one thread is active after our submission.
    // Threads could have transitioned into idling under our feet. We need to account for this.
    if (m_num_busy_waiters.load(std::memory_order_acquire) == 0)
        if (m_idle_workers.tryEmptyAny(idle_worker_id))
            m_workers[idle_worker_id]->wake();

    return true;
}

template <typename Task, template<typename> class Queue>
template <typename Handler>
inline void ThreadPoolImpl<Task, Queue>::post(Handler&& handler)
{
    const auto ok = tryPost(std::forward<Handler>(handler));
    if (!ok)
    {
        throw std::runtime_error("Thread pool queue is full.");
    }
}

template <typename Task, template<typename> class Queue>
inline Worker<Task, Queue>& ThreadPoolImpl<Task, Queue>::getWorker()
{
    auto id = Worker<Task, Queue>::getWorkerIdForCurrentThread();

    if (id > m_workers.size())
    {
        id = m_next_worker.fetch_add(1, std::memory_order_relaxed) %
             m_workers.size();
    }

    return *m_workers[id];
}

}
