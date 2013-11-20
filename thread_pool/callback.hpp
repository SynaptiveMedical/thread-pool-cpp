#ifndef CALLBACK_HPP
#define CALLBACK_HPP

#include "noncopyable.hpp"

#include <type_traits>
#include <cstring>

template <size_t STORAGE_SIZE = 32>
struct callback_t : private noncopyable_t
{
    template <typename T>
    callback_t(T &&object)
    {
        typedef typename std::remove_reference<T>::type unref_type;

        const size_t alignment = std::alignment_of<unref_type>::value;

        static_assert(sizeof(unref_type) + alignment < STORAGE_SIZE,
                      "functional object don't fit into internal storage");

        m_object_ptr = new (m_storage + alignment) unref_type(std::forward<T>(object));
        m_method_ptr = &method_stub<unref_type>;
        m_delete_ptr = &delete_stub<unref_type>;
    }

    callback_t(callback_t &&o)
    {
        move_from_other(o);
    }

    callback_t & operator=(callback_t &&o)
    {
        move_from_other(o);
        return *this;
    }

    ~callback_t()
    {
        if (m_delete_ptr) {
            (*m_delete_ptr)(m_object_ptr);
        }
    }

    void operator()() const
    {
        if (m_method_ptr) {
            (*m_method_ptr)(m_object_ptr);
        }
    }

private:
    char m_storage[STORAGE_SIZE];

    void *m_object_ptr = m_storage;

    typedef void (*method_type)(void *);

    method_type m_method_ptr = nullptr;
    method_type m_delete_ptr = nullptr;

    void move_from_other(callback_t &o)
    {
        const size_t o_alignment = o.m_method_ptr - o.m_storage;
        m_object_ptr = m_storage + o_alignment;
        m_method_ptr = o.m_method_ptr;
        m_delete_ptr = o.m_delete_ptr;

        memcpy(m_storage, o.m_storage, STORAGE_SIZE);

        o.m_method_ptr = nullptr;
        o.m_delete_ptr = nullptr;
    }

    template <class T>
    static void method_stub(void *object_ptr)
    {
        static_cast<T *>(object_ptr)->operator()();
    }

    template <class T>
    static void delete_stub(void *object_ptr)
    {
        static_cast<T *>(object_ptr)->~T();
    }
};


#endif // CALLBACK_HPP