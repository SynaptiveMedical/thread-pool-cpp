#pragma once

#include <thread_pool/slotted_bag.hpp>
#include <thread_pool/thread_pool_options.hpp>
#include <thread_pool/worker.hpp>

#include <atomic>
#include <thread>
#include <limits>
#include <mutex>
#include <chrono>
#include <condition_variable>

namespace tp
{

/**
* @brief Rouser is a specialized worker that periodically wakes other workers. This serves two purposes:
* The first is that it emplaces an upper bound on the processing time of tasks in the thread pool, since
* it is otherwise possible for the thread pool to enter a state where all threads are asleep, and tasks exist
* in worker queues. The second is that it increases the likelihood of at least one worker busy-waiting at any
* point in time, which speeds up task processing response time.
*/
class Rouser
{
public:
    /**
    * @brief Worker Constructor.
    */
    Rouser(std::chrono::microseconds rouse_period);

    /**
    * @brief Copy ctor implementation.
    */
    Rouser(Rouser const&) = delete;

    /**
    * @brief Copy assignment implementation.
    */
    Rouser& operator=(Rouser const& rhs) = delete;

    /**
    * @brief Move ctor implementation.
    */
    Rouser(Rouser&& rhs) noexcept = delete;

    /**
    * @brief Move assignment implementaion.
    */
    Rouser& operator=(Rouser&& rhs) noexcept = delete;

    /**
    * @brief start Create the executing thread and start tasks execution.
    * @param workers A pointer to the vector containing sibling workers for performing round robin work stealing.
    * @param idle_workers A pointer to the slotted bag containing all idle workers.
    * @param num_busy_waiters A pointer to the atomic busy waiter counter.
    * @note The parameters passed into this function generally relate to the global thread pool state.
    */
    template <typename Task, template<typename> class Queue>
    void start(std::vector<std::unique_ptr<Worker<Task, Queue>>>* workers, SlottedBag<Queue>* idle_workers, std::atomic<size_t>* num_busy_waiters);

    /**
    * @brief stop Stop all worker's thread and stealing activity.
    * Waits until the executing thread becomes finished.
    */
    void stop();

private:

    /**
    * @brief threadFunc Executing thread function.
    * @param workers A pointer to the vector containing sibling workers for performing round robin work stealing.
    * @param idle_workers A pointer to the slotted bag containing all idle workers.
    * @param num_busy_waiters A pointer to the atomic busy waiter counter.
    */
    template <typename Task, template<typename> class Queue>
    void threadFunc(std::vector<std::unique_ptr<Worker<Task, Queue>>>* workers, SlottedBag<Queue>* idle_workers, std::atomic<size_t>* num_busy_waiters);

    std::atomic<bool> m_running_flag;
    std::thread m_thread;
    std::chrono::microseconds m_rouse_period;
};

inline Rouser::Rouser(std::chrono::microseconds rouse_period)
    : m_running_flag(true)
    , m_rouse_period(std::move(rouse_period))
{
}

template <typename Task, template<typename> class Queue>
inline void Rouser::start(std::vector<std::unique_ptr<Worker<Task, Queue>>>* workers, SlottedBag<Queue>* idle_workers, std::atomic<size_t>* num_busy_waiters)
{
    m_thread = std::thread(&Rouser::threadFunc<Task, Queue>, this, workers, idle_workers, num_busy_waiters);
}

inline void Rouser::stop()
{
    m_running_flag.store(false, std::memory_order_release);
    m_thread.join();
}


template <typename Task, template<typename> class Queue>
inline void Rouser::threadFunc(std::vector<std::unique_ptr<Worker<Task, Queue>>>* workers, SlottedBag<Queue>* idle_workers, std::atomic<size_t>* num_busy_waiters)
{
    size_t idle_worker_id;

    while (m_running_flag.load(std::memory_order_acquire))
    {
        // Try to wake up a thread if there are no current busy waiters.
        if (num_busy_waiters->load(std::memory_order_acquire) == 0)
            if (idle_workers->tryEmptyAny(idle_worker_id))
                workers->at(idle_worker_id)->wake();

        // Sleep.
        std::this_thread::sleep_for(m_rouse_period);
    }
}

}