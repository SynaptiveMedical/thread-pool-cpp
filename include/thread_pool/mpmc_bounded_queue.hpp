// Copyright (c) 2010-2011 Dmitry Vyukov. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided
// that the following conditions are met:
//
//   1. Redistributions of source code must retain the above copyright notice,
//   this list of
//      conditions and the following disclaimer.
//
//   2. Redistributions in binary form must reproduce the above copyright
//   notice, this list
//      of conditions and the following disclaimer in the documentation and/or
//      other materials
//      provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY DMITRY VYUKOV "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
// EVENT
// SHALL DMITRY VYUKOV OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
// OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//
// The views and conclusions contained in the software and documentation are
// those of the authors and
// should not be interpreted as representing official policies, either expressed
// or implied, of Dmitry Vyukov.

#pragma once

#include <atomic>
#include <type_traits>
#include <vector>
#include <stdexcept>

namespace tp
{

/**
 * @brief The MPMCBoundedQueue class implements bounded
 * multi-producers/multi-consumers lock-free queue.
 * Doesn't accept non-movable types as T.
 * Inspired by Dmitry Vyukov's mpmc queue.
 * http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
 */
template <typename T>
class MPMCBoundedQueue
{
    static_assert(
        std::is_move_constructible<T>::value, "Should be of movable type");

public:
    /**
     * @brief MPMCBoundedQueue Constructor.
     * @param size Power of 2 number - queue length.
     * @throws std::invalid_argument if size is bad.
     */
    explicit MPMCBoundedQueue(size_t size);

    /**
     * @brief Move ctor implementation.
     */
    MPMCBoundedQueue(MPMCBoundedQueue&& rhs) noexcept;

    /**
     * @brief Move assignment implementaion.
     */
    MPMCBoundedQueue& operator=(MPMCBoundedQueue&& rhs) noexcept;

    /**
     * @brief pushStrong Push data to queue.
     * @param data Data to be pushed.
     * @return true on success, if the result is false it is safe to 
     * infer that the queue is full at the time of the call.
     */
    template <typename U>
    bool pushStrong(U&& data);

    /**
     * @brief popStrong Pop data from queue.
     * @param data Place to store popped data.
     * @return true on sucess, if the result is false it is safe to
     * infer that the queue is empty at the time of the call.
     */
    bool popStrong(T& data);

    /**
    * @brief pushWeak Push data to queue. May fail spuriously, 
    * but is more performant than push_strong.
    * @param data Data to be pushed.
    * @return true on success, if the result is false it is NOT safe to 
     * infer that the queue is full at the time of the call.
    */
    template <typename U>
    bool pushWeak(U&& data);

    /**
    * @brief popWeak Pop data from queue. May fail spuriously, 
    * but is more performant than pop_strong.
    * @param data Place to store popped data.
    * @return true on sucess, if the result is false it is NOT safe to
     * infer that the queue is empty at the time of the call.
    */
    bool popWeak(T& data);

private:
    struct Cell
    {
        std::atomic<size_t> sequence;
        T data;

        Cell() = default;

        Cell(const Cell&) = delete;
        Cell& operator=(const Cell&) = delete;

        Cell(Cell&& rhs)
            : sequence(rhs.sequence.load()), data(std::move(rhs.data))
        {
        }

        Cell& operator=(Cell&& rhs)
        {
            sequence = rhs.sequence.load();
            data = std::move(rhs.data);

            return *this;
        }
    };

    /**
    * @brief push Push data to queue.
    * @param data Data to be pushed.
    * @param is_strong false if we wish to allow spurious failures 
    * to occur in interest of performance benefits.
    * @return true on success.
    */
    template <typename U>
    bool push(U&& data, bool is_strong);

    /**
    * @brief pop Pop data from queue.
    * @param data Place to store popped data.
    * @param is_strong false if we wish to allow spurious failures 
    * to occur in interest of performance benefits.
    * @return true on sucess.
    */
    bool pop(T& data, bool is_strong);

    typedef char Cacheline[64];

    Cacheline pad0;
    std::vector<Cell> m_buffer;
    /* const */ size_t m_buffer_mask;
    Cacheline pad1;
    std::atomic<size_t> m_enqueue_pos;
    Cacheline pad2;
    std::atomic<size_t> m_dequeue_pos;
    Cacheline pad3;
};


/// Implementation

template <typename T>
inline MPMCBoundedQueue<T>::MPMCBoundedQueue(size_t size)
    : m_buffer(size), m_buffer_mask(size - 1), m_enqueue_pos(0),
      m_dequeue_pos(0)
{
    bool size_is_power_of_2 = (size >= 2) && ((size & (size - 1)) == 0);
    if(!size_is_power_of_2)
    {
        throw std::invalid_argument("buffer size should be a power of 2");
    }

    for(size_t i = 0; i < size; ++i)
    {
        m_buffer[i].sequence = i;
    }
}

template <typename T>
inline MPMCBoundedQueue<T>::MPMCBoundedQueue(MPMCBoundedQueue&& rhs) noexcept
{
    *this = rhs;
}

template <typename T>
inline MPMCBoundedQueue<T>& MPMCBoundedQueue<T>::operator=(MPMCBoundedQueue&& rhs) noexcept
{
    if (this != &rhs)
    {
        m_buffer = std::move(rhs.m_buffer);
        m_buffer_mask = std::move(rhs.m_buffer_mask);
        m_enqueue_pos = rhs.m_enqueue_pos.load();
        m_dequeue_pos = rhs.m_dequeue_pos.load();
    }
    return *this;
}

template <typename T>
template <typename U>
inline bool MPMCBoundedQueue<T>::pushStrong(U&& data)
{
    return push(std::forward<U>(data), true);
}

template <typename T>
inline bool MPMCBoundedQueue<T>::popStrong(T& data)
{
    return pop(data, true);
}

template <typename T>
template <typename U>
inline bool MPMCBoundedQueue<T>::pushWeak(U&& data)
{
    return push(std::forward<U>(data), false);
}

template <typename T>
inline bool MPMCBoundedQueue<T>::popWeak(T& data)
{
    return pop(data, false);
}

template <typename T>
template <typename U>
inline bool MPMCBoundedQueue<T>::push(U&& data, bool is_strong)
{
    Cell* cell;
    size_t pos = m_enqueue_pos.load(std::memory_order_relaxed);
    for(;;)
    {
        cell = &m_buffer[pos & m_buffer_mask];
        size_t seq = cell->sequence.load(std::memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)pos;
        if(dif == 0)
        {
            if(m_enqueue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                break;
        }
        else if(dif < 0 && (!is_strong || m_enqueue_pos.load(std::memory_order_relaxed) - m_buffer_mask - 1 == m_dequeue_pos.load(std::memory_order_relaxed)))
        {
            return false;
        }
        else
        {
            pos = m_enqueue_pos.load(std::memory_order_relaxed);
        }
    }

    cell->data = std::forward<U>(data);

    cell->sequence.store(pos + 1, std::memory_order_release);

    return true;
}

template <typename T>
inline bool MPMCBoundedQueue<T>::pop(T& data, bool is_strong)
{
    Cell* cell;
    size_t pos = m_dequeue_pos.load(std::memory_order_relaxed);
    for(;;)
    {
        cell = &m_buffer[pos & m_buffer_mask];
        size_t seq = cell->sequence.load(std::memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);
        if(dif == 0)
        {
            if(m_dequeue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                break;
        }
        else if(dif < 0 && (!is_strong || m_dequeue_pos.load(std::memory_order_relaxed) == m_enqueue_pos.load(std::memory_order_relaxed)))
        {
            return false;
        }
        else
        {
            pos = m_dequeue_pos.load(std::memory_order_relaxed);
        }
    }

    data = std::move(cell->data);

    cell->sequence.store(pos + m_buffer_mask + 1, std::memory_order_release);

    return true;
}

}
