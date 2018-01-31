#pragma once

#include <thread_pool/bounded_random_access_bag.hpp>

#include <atomic>
#include <thread>
#include <limits>
#include <mutex>
#include <condition_variable>

namespace tp
{

/**
* @brief The Worker class owns task queue and executing thread.
* In thread it tries to pop task from queue. If queue is empty then it tries
* to steal task from the sibling worker. If steal was unsuccessful then spins
* with one millisecond delay.
* @details State Machine:
*
*             +---------------+---------------+
*             |               |               |
*             v               |               |
*       +--------+       +----------+      +------+
*    +--| Active | ----> | BusyWait | ---> | Idle |
*    |  +--------+       +----------+      +------+
*    |      ^
*    |      |
*    +------+
*
*/
template <typename Task, template<typename> class Queue>
class Worker
{
    using WorkerVector = std::vector<std::unique_ptr<Worker<Task, Queue>>>;
    
    

    class WorkerStoppedException final : public std::exception
    {
    };

public:
    

    /**
    * @brief Worker Constructor.
    * @param queue_size Length of undelaying task queue.
    * @param num_busy_wait_iterations The number of exponential backoff sleep iterations
    * to perform during the busy wait state.
    */
    explicit Worker(size_t queue_size, size_t num_busy_wait_iterations);

    /**
    * @brief Move ctor implementation.
    */
    Worker(Worker&& rhs) noexcept;

    /**
    * @brief Move assignment implementaion.
    */
    Worker& operator=(Worker&& rhs) noexcept;

    /**
    * @brief start Create the executing thread and start tasks execution.
    * @param id Worker ID.
    * @param workers Sibling workers for performing round robin work stealing.
    */
    void start(size_t id, WorkerVector* workers, BoundedRandomAccessBag* idle_workers, std::atomic<size_t>* num_busy_waiters);

    /**
    * @brief stop Stop all worker's thread and stealing activity.
    * Waits until the executing thread became finished.
    */
    void stop();

    /**
    * @brief tryPost Post task to queue.
    * @param handler Handler to be executed in executing thread.
    * @return true on success.
    */
    template <typename Handler>
    bool tryPost(Handler&& handler);

    /**
    * @brief tryGetLocalTask Get one task from this worker queue.
    * @param task Place for the obtained task to be stored.
    * @param is_strong false if we wish to allow spurious failures 
    * to occur in interest of performance benefits. Defaults to true.
    * @return true on success.
    */
    bool tryGetLocalTask(Task& task, bool is_strong = true);

    /**
    * @brief getWorkerIdForCurrentThread Return worker ID associated with
    * current thread if exists.
    * @return Worker ID.
    */
    static size_t getWorkerIdForCurrentThread();

    /**
    * @brief wake Awake the worker if it was previously asleep.
    */
    void wake();

private:

    /**
    * @brief tryRoundRobinSteal Try stealing a thread from sibling workers in a round-robin fashion.
    * @param task Place for the obtained task to be stored.
    * @param workers Sibling workers for performing round robin work stealing.
    * @param is_strong false if we wish to allow spurious failures 
    * to occur in interest of performance benefits. Defaults to true.
    * @return true upon success, false otherwise.
    */
    bool tryRoundRobinSteal(Task& task, WorkerVector* workers, bool is_strong = true);

    /**
    * @brief tryHandleTask Try to obtain a work item and process it.
    * @details This entails attempting to pop an item from the local queue, and if not successful,
    * the worker will attempt to perform a round robin steal.
    * @param task Place for the obtained task to be stored.
    * @param workers Sibling workers for performing round robin work stealing.
    * @param is_strong false if we wish to allow spurious failures 
    * to occur in interest of performance benefits. Defaults to true.
    * @return true upon success, false otherwise.
    */
    bool tryHandleTask(Task& task, WorkerVector* workers, bool is_strong = true);

    /**
    * @brief threadFunc Executing thread function.
    * @param id Worker ID to be associated with this thread.
    * @param workers Sibling workers for performing round robin work stealing.
    */
    void threadFunc(size_t id, WorkerVector* workers, BoundedRandomAccessBag* idle_workers, std::atomic<size_t>* num_busy_waiters);

    
    size_t m_num_busy_wait_iterations;
    Queue<Task> m_queue;
    std::atomic<bool> m_running_flag;
    std::thread m_thread;
    size_t m_next_donor;
    
    std::mutex m_idle_mutex;
    std::condition_variable m_idle_cv;

    bool m_is_idle;
    bool m_abort_idle;
    
};


/// Implementation

namespace detail
{
    inline size_t* thread_id()
    {
        static thread_local size_t tss_id = std::numeric_limits<size_t>::max();
        return &tss_id;
    }
}

template <typename Task, template<typename> class Queue>
inline Worker<Task, Queue>::Worker(size_t queue_size, size_t num_busy_wait_iterations)
    : m_num_busy_wait_iterations(num_busy_wait_iterations)
    , m_queue(queue_size)
    , m_running_flag(true)
    , m_next_donor(0) // Initialized in threadFunc.
    , m_is_idle(false)
    , m_abort_idle(false)
{
}

template <typename Task, template<typename> class Queue>
inline Worker<Task, Queue>::Worker(Worker&& rhs) noexcept
{
    *this = rhs;
}

template <typename Task, template<typename> class Queue>
inline Worker<Task, Queue>& Worker<Task, Queue>::operator=(Worker&& rhs) noexcept
{
    if (this != &rhs)
    {
        m_num_busy_wait_iterations = rhs.m_num_busy_wait_iterations;
        m_queue = std::move(rhs.m_queue);
        m_running_flag = rhs.m_running_flag.load();
        m_thread = std::move(rhs.m_thread);
        m_next_donor = rhs.m_next_donor;
        m_is_idle = rhs.m_is_idle;
        m_abort_idle = rhs.m_is_idle;
        m_idle_mutex = std::move(rhs.m_idle_mutex);
        m_idle_cv = std::move(rhs.m_idle_cv);
    }
    return *this;
}

template <typename Task, template<typename> class Queue>
inline void Worker<Task, Queue>::stop()
{
    m_running_flag.store(false);
    wake();
    m_thread.join();
}

template <typename Task, template<typename> class Queue>
inline void Worker<Task, Queue>::start(size_t id, WorkerVector* workers, BoundedRandomAccessBag* idle_workers, std::atomic<size_t>* num_busy_waiters)
{
    m_thread = std::thread(&Worker<Task, Queue>::threadFunc, this, id, workers, idle_workers, num_busy_waiters);
}

template <typename Task, template<typename> class Queue>
inline size_t Worker<Task, Queue>::getWorkerIdForCurrentThread()
{
    return *detail::thread_id();
}

template <typename Task, template<typename> class Queue>
inline void Worker<Task, Queue>::wake()
{
    bool notify;
    {
        std::unique_lock<std::mutex> lock(m_idle_mutex);

        m_abort_idle = true;
        notify = m_is_idle;
    }
    // Notify outside of lock.
    if (notify)
        m_idle_cv.notify_one();
}

template <typename Task, template<typename> class Queue>
template <typename Handler>
inline bool Worker<Task, Queue>::tryPost(Handler&& handler)
{
    return m_queue.pushStrong(std::forward<Handler>(handler));
}

template <typename Task, template<typename> class Queue>
inline bool Worker<Task, Queue>::tryGetLocalTask(Task& task, bool is_strong)
{
    return is_strong ? m_queue.popStrong(task) : m_queue.popWeak(task);
}

template <typename Task, template<typename> class Queue>
inline bool Worker<Task, Queue>::tryRoundRobinSteal(Task& task, WorkerVector* workers, bool is_strong)
{
    auto starting_index = m_next_donor;

    // Iterate once through the worker ring, checking for queued work items on each thread.
    do
    {
        // Don't steal from local queue.
        if (m_next_donor != *detail::thread_id() && workers->at(m_next_donor)->tryGetLocalTask(task, is_strong))
        {
            // Increment before returning so that m_next_donor always points to the worker that has gone the longest
            // without a steal attempt. This helps enforce fairness in the stealing.
            ++m_next_donor %= workers->size();
            return true;
        }

        ++m_next_donor %= workers->size();
    } while (m_next_donor != starting_index);

    return false;
}

template <typename Task, template <typename> class Queue>
bool Worker<Task, Queue>::tryHandleTask(Task& task, WorkerVector* workers, bool is_strong)
{
    if (!m_running_flag.load())
        throw WorkerStoppedException();

    // Prioritize local queue, then try stealing from sibling workers.
    if (tryGetLocalTask(task, is_strong) || tryRoundRobinSteal(task, workers, is_strong))
    {
        try
        {
            task();
        }
        catch (...)
        {
            // Suppress all exceptions.
        }

        return true;
    }

    return false;
}

template <typename Task, template<typename> class Queue>
inline void Worker<Task, Queue>::threadFunc(size_t id, WorkerVector* workers, BoundedRandomAccessBag* idle_workers, std::atomic<size_t>* num_busy_waiters)
{
    *detail::thread_id() = id;
    m_next_donor = (id + 1) % workers->size();
    Task handler;
    bool task_found = false;

    try
    {
        while (true)
        {
            // By default, this loop operates in the active state. 
            // We poll for items from our local task queue and try to steal from others.
            if (tryHandleTask(handler, workers, false)) continue;

            // We were unable to obtain a task. 
            // We now transition into the busy wait state.
            task_found = false;
            num_busy_waiters->fetch_add(1, std::memory_order_acq_rel);

            for (auto i = 0u; i < m_num_busy_wait_iterations && !task_found; i++)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<size_t>(pow(2, i))));
                task_found = tryHandleTask(handler, workers, false);
            }

            // If we found a task during our busy wait sequence, we abort it and transition back into the active loop.
            if (task_found)
            {
                num_busy_waiters->fetch_add(-1, std::memory_order_acq_rel);
                continue;
            }

            // No tasks were found during the busy wait sequence. 
            // We now transition into the idle state.
            {
                std::unique_lock<std::mutex> lock(m_idle_mutex);
                m_is_idle = false;
                m_abort_idle = false;
            }

            // We put this worker up for grabs as a recipient to new posts in the thread pool.
            idle_workers->add(id);

            // We need to transition out of the busy wait state after we have submitted ourselves to the idle 
            // worker queue in order to avoid a race.
            num_busy_waiters->fetch_add(-1, std::memory_order_acq_rel);

            // While we were adding this worker to the idle worker bag, a job may have been posted into this 
            // worker's queue. We need to check for work again before initiating the deep sleep sequence, otherwise
            // the given task may be lost. 
            // Any further posts will flip the m_abort_idle flag to true, and we will catch them later.
            if (tryHandleTask(handler, workers))
            {
                // A task was indeed posted in the time it took this worker to enter the bag.
                // We remove the worker from the bag. If the internal state of the bag was not changed,
                // this means a different thread has already removed this worker from the idle queue, 
                // and this case will be caught below.
                if (idle_workers->remove(id))
                    continue;
            }

            {
                std::unique_lock<std::mutex> lock(m_idle_mutex);

                // A post has occurred during the sleep sequence! Abort the sleep sequence.
                if (m_abort_idle)
                    continue;

                m_is_idle = true;
                m_idle_cv.wait(lock, [this]() { return m_abort_idle; });
            }
        }
    }
    catch (WorkerStoppedException)
    {
        // Allow thread function to complete.
    }
}

}