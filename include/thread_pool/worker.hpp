#pragma once

#include <thread_pool/mpmc_bounded_queue.hpp>

#include <atomic>
#include <thread>
#include <limits>
#include <mutex>
#include <condition_variable>

namespace tp
{

class SPMCBoundedRandomAccessBag
{
    struct Element
    {
        //
        // State Machine:
        //
        //             +------- TryRemove() -------+-------------------------------+    +-- remove() --+
        //             |                           |                               |    |              |
        //             v                           |                               |    |              |
        //       +-----------+              +-------------+                 +---------------+          |
        //   +---| NotQueued | -- add() --> | QueuedValid | -- remove() --> | QueuedInvalid | <--------+
        //   |   +-----------+              +-------------+                 +---------------+
        //   |            ^                        ^                               |
        //   |            |                        |                               |
        //   +- remove() -+                        +------------- add() -----------+
        //
        enum class State
        {
            NotQueued,
            QueuedValid,
            QueuedInvalid,
        };

        std::atomic<State> m_state;
        size_t m_id;

        Element(size_t id)
            : m_state(State::NotQueued)
            , m_id(id)
        {
        }
    };

public:
    SPMCBoundedRandomAccessBag(size_t size)
        : m_queue(size)
    {
        m_entries.reserve(size);
        for (auto i = 0u; i < size; i++)
            m_entries.push_back(Element(i));
    }

    void add(size_t id)
    {

        switch(m_entries[id].m_state.load())
        {
            case Element::State::NotQueued:
                // No race, single producer.
                m_entries[id].m_state.store(Element::State::QueuedValid);
                if (!m_queue.push(&m_entries[id]))
                    throw std::exception("Internal Logic Error: The queue is full."); // This should never occur.
                break;

            case Element::State::QueuedValid:
                throw std::exception("The item has already been added to the bag.");

            case Element::State::QueuedInvalid:
                auto state = Element::State::QueuedInvalid;
                if (!m_entries[id].m_state.compare_exchange_strong(state, Element::State::QueuedValid))
                {
                    // Someone called TryRemoveAny, and our state had transitioned into NotQueued.
                    // Since we are the only producer, it is safe to throw the object back in the queue.
                    state = Element::State::NotQueued;
                    if (m_entries[id].m_state.compare_exchange_strong(state, Element::State::QueuedValid))
                    {
                        if (!m_queue.push(&m_entries[id]))
                            throw std::exception("Internal Logic Error: The queue is full."); // This should never occur.
                    }
                    else
                        throw std::exception("Another producer has added the item. This violates single producer semantics.");
                }
                break;
        }
             
    }

    void remove(size_t id)
    {
        // This consumer action is solely responsible for an indiscriminant QueuedValid -> QueuedInvalid state transition.
        auto state = Element::State::QueuedValid;
        m_entries[id].m_state.compare_exchange_strong(state, Element::State::QueuedInvalid);
    }

    bool tryRemoveAny(size_t& id)
    {
        Element* element;
        while (m_queue.pop(element))
        {
            // Once a consumer pops an element, they are the sole controller of that object's membership to the queue
            // (i.e. they are solely responsible for the transition back into the NotQueued state) by virtue of the
            // MPMCBoundedQueue's atomicity semantics.
            // In other words, only one consumer can hold a popped element before it its state is set to NotQueued.
            auto previous = element->m_state.exchange(Element::State::NotQueued);

            switch (previous)
            {
            case Element::State::NotQueued:
                throw std::exception("Internal Logic Error: State machine logic violation.");

            case Element::State::QueuedValid:
                id = element->m_id;
                return true;

            case Element::State::QueuedInvalid:
                // Try again.
                break;
            }
        }

        // Queue empty.
        return false;
    }

        

private:
    MPMCBoundedQueue<Element *> m_queue;
    std::vector<Element> m_entries;
};

/**
* @brief The Worker class owns task queue and executing thread.
* In thread it tries to pop task from queue. If queue is empty then it tries
* to steal task from the sibling worker. If steal was unsuccessful then spins
* with one millisecond delay.
*/
template <typename Task, template<typename> class Queue>
class Worker
{
    using WorkerVector = std::vector<std::unique_ptr<Worker<Task, Queue>>>;
    using IdleWorkerPtr = std::shared_ptr<std::atomic<Worker<Task, Queue>*>>;
    using IdleWorkerQueue = MPMCBoundedQueue<IdleWorkerPtr>;

public:
    

    /**
    * @brief Worker Constructor.
    * @param queue_size Length of undelaying task queue.
    */
    explicit Worker(size_t queue_size);

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
    void start(size_t id, WorkerVector* workers, IdleWorkerQueue* idle_workers, std::atomic<size_t>* num_busy_waiters);

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
    * @return true on success.
    */
    bool tryGetLocalTask(Task& task);

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
    */
    bool tryRoundRobinSteal(Task& task, WorkerVector* workers);

    /**
    * @brief threadFunc Executing thread function.
    * @param id Worker ID to be associated with this thread.
    * @param workers Sibling workers for performing round robin work stealing.
    */
    void threadFunc(size_t id, WorkerVector* workers, IdleWorkerQueue* idle_workers, std::atomic<size_t>* num_busy_waiters);

    
    Queue<Task> m_queue;
    std::atomic<bool> m_running_flag;
    std::thread m_thread;
    size_t m_next_donor;
    const size_t m_num_busy_wait_iterations = 5;

    std::mutex m_sleep_mutex;
    std::condition_variable m_sleep_cv;
    bool m_abort_sleep;
    bool m_is_asleep;
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
inline Worker<Task, Queue>::Worker(size_t queue_size)
    : m_queue(queue_size)
    , m_running_flag(true)
    , m_next_donor(0) // Initialized in threadFunc.
    , m_is_asleep(false)
    , m_abort_sleep(false)
    , m_sleep_queue_ptr(nullptr)
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
        m_queue = std::move(rhs.m_queue);
        m_running_flag = rhs.m_running_flag.load();
        m_thread = std::move(rhs.m_thread);
    }
    return *this;
}

template <typename Task, template<typename> class Queue>
inline void Worker<Task, Queue>::stop()
{
    m_running_flag.store(false, std::memory_order_relaxed);
    m_thread.join();
}

template <typename Task, template<typename> class Queue>
inline void Worker<Task, Queue>::start(size_t id, WorkerVector* workers, IdleWorkerQueue* idle_workers, std::atomic<size_t>* num_busy_waiters)
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
    std::unique_lock<std::mutex> lock(m_sleep_mutex);

    m_abort_sleep = true;

    if (m_is_asleep)
        m_sleep_cv.notify_one();
}

template <typename Task, template<typename> class Queue>
template <typename Handler>
inline bool Worker<Task, Queue>::tryPost(Handler&& handler)
{
    return m_queue.push(std::forward<Handler>(handler));
}

template <typename Task, template<typename> class Queue>
inline bool Worker<Task, Queue>::tryGetLocalTask(Task& task)
{
    return m_queue.pop(task);
}

template <typename Task, template<typename> class Queue>
inline bool Worker<Task, Queue>::tryRoundRobinSteal(Task& task, WorkerVector* workers)
{
    auto starting_index = m_next_donor;

    // Iterate once through the worker ring, checking for queued work items on each thread.
    do
    {
        // Don't steal from local queue.
        if (m_next_donor != *detail::thread_id() && workers->at(m_next_donor)->tryGetLocalTask(task))
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

template <typename Task, template<typename> class Queue>
inline void Worker<Task, Queue>::threadFunc(size_t id, WorkerVector* workers, IdleWorkerQueue* idle_workers, std::atomic<size_t>* num_busy_waiters)
{
    *detail::thread_id() = id;
    m_next_donor = ++id % workers->size();
    auto idle_iterations = 0u;

    Task handler;

    while (m_running_flag.load(std::memory_order_relaxed))
    {
        // Prioritize local queue, then try stealing from sibling workers.
        if (tryGetLocalTask(handler) || tryRoundRobinSteal(handler, workers))
        {
            idle_iterations = 0;
            num_busy_waiters->fetch_add(-1);

            try
            {
                handler();
            }
            catch (...)
            {
                // Suppress all exceptions.
            }
        }
        else if (idle_iterations <= m_num_busy_wait_iterations)
        {
            if (idle_iterations == 0)
                num_busy_waiters->fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<size_t>(pow(2, idle_iterations))));
            idle_iterations++;
            
        }
        else
        {
            m_is_asleep = false;
            m_abort_sleep = false;
            idle_iterations = 0;

            if (!idle_workers->push(&m_idle_queue_entry))
                throw std::exception("Idle worker queue is full."); // This should never occur.

            num_busy_waiters->fetch_add(-1);

            if (tryGetLocalTask(handler) || tryRoundRobinSteal(handler, workers))
            {
                try
                {
                    handler();
                }
                catch (...)
                {
                    // Suppress all exceptions.
                }

                idle_queue_entry->store(nullptr);
            }
            else
            {
                std::unique_lock<std::mutex> lock(m_sleep_mutex);
                
                if (m_abort_sleep)
                    continue;

                m_is_asleep = true;
                m_sleep_cv.wait(lock, [this]() { return m_abort_sleep; });
            }
        }
    }
}

}
