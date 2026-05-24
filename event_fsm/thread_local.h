#pragma once

#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#include "concurrent_hash_map.h"

template <typename T>
class ThreadLocal
{
    static_assert(!std::is_reference_v<T>,
                  "T must not be a reference type");
    static_assert(!std::is_const_v<T>,
                  "T must not be const");
    static_assert(std::is_default_constructible_v<T>,
                  "T must be default constructible");

    // 验证 T 为纯类型（不能带 const、&、&&、volatile）
    static_assert(
        std::is_same_v<T, std::decay_t<T>>,
        "Error: Please pass a pure type (without const, &, &&, or volatile)!"
    );

public:
    ThreadLocal() = default;
    ~ThreadLocal() = default;

    // 禁止拷贝和移动
    ThreadLocal(const ThreadLocal &) = delete;
    ThreadLocal &operator=(const ThreadLocal &) = delete;
    ThreadLocal(ThreadLocal &&) = delete;
    ThreadLocal &operator=(ThreadLocal &&) = delete;

    // 支持默认值的构造函数（head-only 模式）
    explicit ThreadLocal(const T& value)
        : default_value_(std::in_place, value) {}

    explicit ThreadLocal(T&& value)
        : default_value_(std::in_place, std::move(value)) {}

    T& Get() {
        uint64_t tid = GetTid();
        auto opt = Data_.Find(tid);
        if (opt.has_value()) {
            // 该线程副本已经存在
            return opt.value()->second;
        } else {
            // 当线程本地存储不存在时，使用 default_value_ 或 T{} 初始化
            if (default_value_.has_value()) {
                auto [ptr, inserted] = Data_.Emplace(tid, default_value_.value());
                (void)inserted;
                return ptr->second;
            } else {
                auto [ptr, inserted] = Data_.Emplace(tid, T{});
                (void)inserted;
                return ptr->second;
            }
        }
    }

    void Set(const T &value)
    {
        Data_.Emplace(GetTid(), value);
    }

    void Set(T &&value)
    {
        Data_.Emplace(GetTid(), std::move(value));
    }

private:
    // 类型别名：存储类型为 const T，确保多线程共享时仅初始化可修改
    using ConstType = const T;
    // 默认值：仅初始化赋值，线程间共享（const 保证）
    const std::optional<ConstType> default_value_;
    // 线程本地存储：key 固定为【线程ID uint64_t】；value 为 T；
    ConcurrentHashMap<T> Data_;

    static inline uint64_t GetTid() {
        return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }
};
