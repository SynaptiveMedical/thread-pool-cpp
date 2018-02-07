#pragma once

#include <algorithm>
#include <thread>
#include <functional>
#include <chrono>

namespace tp
{

/**
 * @brief The ThreadPoolOptions class provides creation options for ThreadPool.
 */
class ThreadPoolOptions
{
public:
    /**
    * @brief The BusyWaitOptions class provides worker busy wait behaviour options.
    */
    class BusyWaitOptions
    {
    public:
        using IterationFunction = std::function<std::chrono::microseconds(size_t)>;

        /**
        * @brief BusyWaitOptions Construct default options for busy wait behaviour.
        */
        BusyWaitOptions(size_t num_iterations = defaultNumIterations()
            , IterationFunction&& function = defaultIterationFunction());

        /**
        * @brief setNumIterations Set the number of sleeping iterations that take place during
        * a worker's busy wait state. The iteration function will be called on every iteration 
        * with the iteration number.
        * @param count The number of busy wait iterations.
        */
        void setNumIterations(size_t count);

        /**
        * @brief setIterationFunction Set the function to be called upon each sleep iteration.
        * @param function The iteration function to be called.
        */
        void setIterationFunction(IterationFunction&& function);

        /**
        * @brief numIterations Return the number of busy wait iterations.
        */
        size_t numIterations() const;

        /**
        * @brief iterationFunction Return the busy wait iteration function.
        */
        IterationFunction const& iterationFunction() const;

        /**
        * @brief defaultThreadCount Obtain the default num iterations value.
        */
        static size_t defaultNumIterations();

        /**
        * @brief defaultIterationFunction Obtain the default iteration function value.
        */
        static IterationFunction defaultIterationFunction();

    private:
        size_t m_num_iterations;
        IterationFunction m_iteration_function;
    };

    /**
     * @brief ThreadPoolOptions Construct default options for thread pool.
     */
    ThreadPoolOptions(size_t thread_count = defaultThreadCount()
        , size_t queue_size = defaultQueueSize()
        , BusyWaitOptions&& busy_wait_options = defaultBusyWaitOptions());

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
    * @brief setBusyWaitOptions Set the parameters relating to worker busy waiting behaviour.
    * @param options The busy wait options.
    */
    void setBusyWaitOptions(BusyWaitOptions&& options);


    /**
     * @brief threadCount Return thread count.
     */
    size_t threadCount() const;

    /**
     * @brief queueSize Return single worker queue size.
     */
    size_t queueSize() const;

    /**
    * @brief busyWaitOptions Return a reference to the busy wait options.
    */
    BusyWaitOptions const& busyWaitOptions() const;
    
    /**
    * @brief defaultThreadCount Obtain the default thread count value.
    */
    static size_t defaultThreadCount();

    /**
    * @brief defaultQueueSize Obtain the default queue size value.
    */
    static size_t defaultQueueSize();

    /**
    * @brief defaultBusyWaitOptions Obtain the default busy wait options.
    */
    static BusyWaitOptions defaultBusyWaitOptions();


private:
    size_t m_thread_count;
    size_t m_queue_size;
    BusyWaitOptions m_busy_wait_options;
};

/// Implementation

inline ThreadPoolOptions::BusyWaitOptions::BusyWaitOptions(size_t num_iterations, ThreadPoolOptions::BusyWaitOptions::IterationFunction&& function)
    : m_num_iterations(num_iterations)
    , m_iteration_function(std::forward<ThreadPoolOptions::BusyWaitOptions::IterationFunction>(function))
{
}

inline void ThreadPoolOptions::BusyWaitOptions::setNumIterations(size_t count)
{
    m_num_iterations = std::max<size_t>(0u, count);
}

inline void ThreadPoolOptions::BusyWaitOptions::setIterationFunction(ThreadPoolOptions::BusyWaitOptions::IterationFunction&& function)
{
    m_iteration_function = std::forward<ThreadPoolOptions::BusyWaitOptions::IterationFunction>(function);
}

inline size_t ThreadPoolOptions::BusyWaitOptions::numIterations() const
{
    return m_num_iterations;
}

inline ThreadPoolOptions::BusyWaitOptions::IterationFunction const& ThreadPoolOptions::BusyWaitOptions::iterationFunction() const
{
    return m_iteration_function;
}


inline size_t ThreadPoolOptions::BusyWaitOptions::defaultNumIterations()
{
    static const size_t instance = 3;
    return instance;
}

inline ThreadPoolOptions::BusyWaitOptions::IterationFunction ThreadPoolOptions::BusyWaitOptions::defaultIterationFunction()
{
    return [](size_t i) { return std::chrono::microseconds(static_cast<size_t>(pow(2, i))*1000); };
}

inline ThreadPoolOptions::ThreadPoolOptions(size_t thread_count, size_t queue_size, BusyWaitOptions&& busy_wait_options)
    : m_thread_count(thread_count)
    , m_queue_size(queue_size)
    , m_busy_wait_options(busy_wait_options)
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

inline void ThreadPoolOptions::setBusyWaitOptions(BusyWaitOptions&& busy_wait_options)
{
    m_busy_wait_options = std::forward<BusyWaitOptions>(busy_wait_options);
}

inline size_t ThreadPoolOptions::threadCount() const
{
    return m_thread_count;
}

inline size_t ThreadPoolOptions::queueSize() const
{
    return m_queue_size;
}

inline ThreadPoolOptions::BusyWaitOptions const& ThreadPoolOptions::busyWaitOptions() const
{
    return m_busy_wait_options;
}

inline size_t ThreadPoolOptions::defaultThreadCount()
{
    static const size_t instance = std::max<size_t>(1u, std::thread::hardware_concurrency());
    return instance;
}

inline size_t ThreadPoolOptions::defaultQueueSize()
{
    return 1024;
}

inline ThreadPoolOptions::BusyWaitOptions ThreadPoolOptions::defaultBusyWaitOptions()
{
    return ThreadPoolOptions::BusyWaitOptions();
}


}
