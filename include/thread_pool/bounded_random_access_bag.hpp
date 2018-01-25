#pragma once

#include <thread_pool/mpmc_bounded_queue.hpp>

#include <atomic>

namespace tp
{

/**
* @brief The Bounded Random Access Bag is a lockless bag which supports add/remove, as well as 
* unordered retrieval of its constituent elements. The bag supports multiple consumers, and a single
* producer per element.
*/
class BoundedRandomAccessBag
{
    /**
    * @brief The implementation of a single element of the bag. Maintains state and id.
    * @details State Machine:
        *
        *             +------- TryRemove() -------+-------------------------------+    +-- remove() --+
        *             |                           |                               |    |              |
        *             v                           |                               |    |              |
        *       +-----------+              +-------------+                 +---------------+          |
        *   +---| NotQueued | -- add() --> | QueuedValid | -- remove() --> | QueuedInvalid | <--------+
        *   |   +-----------+              +-------------+                 +---------------+
        *   |            ^                        ^                               |
        *   |            |                        |                               |
        *   +- remove() -+                        +------------- add() -----------+
    */
    struct Element
    {
        enum class State
        {
            NotQueued,
            QueuedValid,
            QueuedInvalid,
        };

        std::atomic<State> state;
        size_t id;

        Element()
            : state(State::NotQueued)
            , id(0)
        {
        }

        Element(const Element&) = delete;
        Element& operator=(const Element&) = delete;

        Element(Element&& rhs) = default;
        Element& operator=(Element&& rhs) = default;
    };

public:

    /**
    * @brief BoundedRandomAccessBag Constructor.
    * @param size Power of 2 number - queue length.
    * @throws std::invalid_argument if size is not a power of 2.
    */
    explicit BoundedRandomAccessBag(size_t size);

    BoundedRandomAccessBag(BoundedRandomAccessBag& rhs) = delete;
    BoundedRandomAccessBag& operator=(BoundedRandomAccessBag& rhs) = delete;

    BoundedRandomAccessBag(BoundedRandomAccessBag&& rhs) = default;
    BoundedRandomAccessBag& operator=(BoundedRandomAccessBag&& rhs) = default;

    /**
    * @brief add Add an element with the specified id to the bag.
    * @param id The id of the element to add.
    * @throws std::runtime_error if the id is already in the bag.
    * @note Other exceptions may be thrown if the single-producer-per-slot
    * semantics are violated.
    */
    void add(size_t id);

    /**
    * @brief remove Removes the element with the specified id from the bag.
    * @param id The id of the element to remove.
    */
    void remove(size_t id);

    /**
    * @brief tryRemoveAny Try to remove any element from the bag.
    * @param id The removed id will be stored in this variable upon success.
    * @return true upon success, false otherwise.
    * @note Other exceptions may be thrown if the single-producer-per-slot
    * semantics are violated.
    */
    bool tryRemoveAny(size_t& id);

private:
    MPMCBoundedQueue<Element*> m_queue;
    std::vector<Element> m_elements;
};

inline BoundedRandomAccessBag::BoundedRandomAccessBag(size_t size)
    : m_queue(size)
    , m_elements(size)
{
    for (auto i = 0u; i < size; i++)
        m_elements[i].id = i;
}

inline void BoundedRandomAccessBag::add(size_t id)
{

    switch (m_elements[id].state.load())
    {
    case Element::State::NotQueued:
        // No race, single producer.
        if (m_elements[id].state.exchange(Element::State::QueuedValid) != Element::State::NotQueued)
            throw std::logic_error("Another producer has added the item. This violates single producer semantics.");

        if (!m_queue.push(&m_elements[id]))
            throw std::logic_error("Queue is full.");

        break;

    case Element::State::QueuedValid:
        throw std::runtime_error("The item has already been added to the bag.");

    case Element::State::QueuedInvalid:
        auto state = Element::State::QueuedInvalid;
        if (!m_elements[id].state.compare_exchange_strong(state, Element::State::QueuedValid))
        {
            // Someone called TryRemoveAny, and our state had transitioned into NotQueued.
            // Since we are the only producer, it is safe to throw the object back in the queue.
            state = Element::State::NotQueued;
            if (m_elements[id].state.compare_exchange_strong(state, Element::State::QueuedValid))
            {
                if (!m_queue.push(&m_elements[id]))
                    throw std::logic_error("Queue is full.");
            }
            else
                throw std::runtime_error("Another producer has added the item. This violates single producer semantics.");
        }
        break;
    }
}

inline void BoundedRandomAccessBag::remove(size_t id)
{
    // This consumer action is solely responsible for an indiscriminant QueuedValid -> QueuedInvalid state transition.
    auto state = Element::State::QueuedValid;
    m_elements[id].state.compare_exchange_strong(state, Element::State::QueuedInvalid);
}

inline bool BoundedRandomAccessBag::tryRemoveAny(size_t& id)
{
    Element* element;
    while (m_queue.pop(element))
    {
        // Once a consumer pops an element, they are the sole controller of that object's membership to the queue
        // (i.e. they are solely responsible for the transition back into the NotQueued state) by virtue of the
        // MPMCBoundedQueue's atomicity semantics.
        // In other words, only one consumer can hold a popped element before it its state is set to NotQueued.
        auto previous = element->state.exchange(Element::State::NotQueued);

        switch (previous)
        {
        case Element::State::NotQueued:
            throw std::logic_error("State machine logic violation.");

        case Element::State::QueuedValid:
            id = element->id;
            return true;

        case Element::State::QueuedInvalid:
            // Try again.
            break;
        }
    }

    // Queue empty.
    return false;
}

}