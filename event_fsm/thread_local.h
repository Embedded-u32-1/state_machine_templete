#pragma once

#include <optional>
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

    T &Get() {
        auto it = Data_.find(this);
        if (it == Data_.end()) {
            // 当线程本地存储不存在时，使用 default_value_ 或 T{} 初始化
            if (default_value_.has_value()) {
                // 使用 ConstType 构造，确保存储类型不可修改
                auto result = Data_.emplace(this, ConstType(default_value_->value()));
                return result.first->second;
            } else {
                auto result = Data_.emplace(this, T{});
                return result.first->second;
            }
        } else {
            // 该线程副本已经 存在
            return it->second;
        }
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
    // 类型别名：存储类型为 const T，确保多线程共享时仅初始化可修改
    using ConstType = const T;
    // 默认值：仅初始化赋值，线程间共享（const 保证）
    const std::optional<ConstType> default_value_;
    // 线程本地存储：每个线程独立一份 map，key 为 this 指针，value 为 T
    inline static thread_local std::unordered_map<const ThreadLocal<T> *, T> Data_;
};