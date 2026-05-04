#pragma once

#include <type_traits>
#include <unordered_map>
#include <utility>

template <typename T>
class ThreadLocal
{
    static_assert(!std::is_reference_v<T>,
                  "T must not be a reference type");
    static_assert(!std::is_const_v<T>,
                  "T must not be const");
    static_assert(std::is_default_constructible_v<T>,
                  "T must be default constructible");

public:
    ThreadLocal() = default;
    ~ThreadLocal() = default;

    ThreadLocal(const ThreadLocal &) = delete;
    ThreadLocal &operator=(const ThreadLocal &) = delete;
    ThreadLocal(ThreadLocal &&) = delete;
    ThreadLocal &operator=(ThreadLocal &&) = delete;

    T &Get()
    {
        return Data_[this];
    }

    void Set(const T &value)
    {
        Data_[this] = value;
    }

    void Set(T &&value)
    {
        Data_[this] = std::move(value);
    }

private:
    inline static thread_local std::unordered_map<const ThreadLocal<T> *, T> Data_;
};