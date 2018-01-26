#pragma once

#include <algorithm>
#include <thread>

namespace tp
{

/**
 * @brief The ThreadPoolOptions class provides creation options for
 * ThreadPool.
 */
class ThreadPoolOptions
{
public:
    /**
     * @brief ThreadPoolOptions Construct default options for thread pool.
     */
    ThreadPoolOptions();

    /**
     * @brief setThreadCount Set thread count.
     * @param count Number of threads to be created.
     */
    void setThreadCount(size_t count);

    /**
     * @brief setQueueSize Set single worker queue size.
     * @param count Maximum length of queue of single worker.
     */
    void setQueueSize(size_t size);

    /**
    * @brief setNumBusyWaitIterations Set the number of sleeping iterations that take place during
    * a worker's busy wait state. 
    * @details Each iteration results in an exponentially increasing sleep time (by power of 2).
    * For example, a value of 5 will result in the following behaviour (in ms of sleep time): 1, 2, 4, 8, 16.
    * @param count The number of busy wait iterations.
    */
    void setNumBusyWaitIterations(size_t size);

    /**
     * @brief threadCount Return thread count.
     */
    size_t threadCount() const;

    /**
     * @brief queueSize Return single worker queue size.
     */
    size_t queueSize() const;

    /**
    * @brief numBusyWaitIterations Return the number of busy wait iterations.
    */
    size_t numBusyWaitIterations() const;

private:
    size_t m_thread_count;
    size_t m_queue_size;
    size_t m_num_busy_wait_iterations;
};

/// Implementation

inline ThreadPoolOptions::ThreadPoolOptions()
    : m_thread_count(std::max<size_t>(1u, std::thread::hardware_concurrency()))
    , m_queue_size(1024u)
    , m_num_busy_wait_iterations(3)
{
}

inline void ThreadPoolOptions::setThreadCount(size_t count)
{
    m_thread_count = std::max<size_t>(1u, count);
}

inline void ThreadPoolOptions::setQueueSize(size_t size)
{
    m_queue_size = std::max<size_t>(1u, size);
}

inline void ThreadPoolOptions::setNumBusyWaitIterations(size_t size)
{
    m_num_busy_wait_iterations = std::max<size_t>(0u, size);
}

inline size_t ThreadPoolOptions::threadCount() const
{
    return m_thread_count;
}

inline size_t ThreadPoolOptions::queueSize() const
{
    return m_queue_size;
}

inline size_t ThreadPoolOptions::numBusyWaitIterations() const
{
    return m_num_busy_wait_iterations;
}

}
