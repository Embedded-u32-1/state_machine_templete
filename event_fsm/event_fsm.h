#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <map>
#include <type_traits>
#include <vector>

#include "thread_local.h"

/**
 * @brief 事件驱动状态机（Header-Only）
 * @tparam StateEnum 状态枚举，建议使用 enum class
 * @tparam EventEnum 事件枚举，建议使用 enum class
 * @note 执行模型：事件入队 + Sync 同步（仲裁式状态切换）
 * @note 核心原则：
 *       - 仲裁式状态切换：resolver 根据当前状态 + 事件快照决定目标状态
 *       - 事件回调仅在【状态保持】时执行：稳态环境下跑事件回调
 *       - 状态切换时：整批 pending 事件直接丢弃，不执行任何 Action
 *       - 状态保持时：合法事件按队列顺序依次执行，非法事件静默丢弃
 */
template <typename StateEnum, typename EventEnum>
class EventFsm final {
    static_assert(std::is_scoped_enum_v<StateEnum>,
                  "StateEnum must be an enum class (scoped enum)");
    static_assert(std::is_scoped_enum_v<EventEnum>,
                  "EventEnum must be an enum class (scoped enum)");

private:
    struct RecursionGuard {
        bool& flag_;

        explicit RecursionGuard(bool& flag) : flag_(flag) {
            if (flag_) {
                throw std::runtime_error(
                    "Recursive state transition detected");
            }
            flag_ = true;
        }

        ~RecursionGuard() {
            flag_ = false;
        }

        // 禁用拷贝和移动
        RecursionGuard(const RecursionGuard&) = delete;
        RecursionGuard& operator=(const RecursionGuard&) = delete;
        RecursionGuard(RecursionGuard&&) = delete;
        RecursionGuard& operator=(RecursionGuard&&) = delete;
    };

public:
    using SelfType = EventFsm&;
    using Action   = std::function<void(SelfType)>;

    /**
     * @brief 仲裁器：输入当前状态 + 待处理事件列表，输出目标状态
     * @note 禁止依赖事件的入队顺序或索引位置（同一批次事件视为同一时刻扰动）
     */
    using Resolver = std::function<StateEnum(StateEnum, const std::vector<EventEnum>&)>;

    /**
     * @brief 状态校验器：校验目标状态是否合法
     */
    using Validator = std::function<bool(StateEnum)>;

    /**
     * @brief 状态转移钩子函数: 用于调试。
     * 【适用场景】
     * - @b 日志记录：状态转移轨迹
     * - @b 性能统计：统计状态转移次数、频率；
     * 【注意事项】
     * - @b 不要滥用：严禁在此钩子中执行耗时操作（如IO、网络请求、锁等待），会影响状态机性能
     */
    using OldState = StateEnum;
    using NewState = StateEnum;
    using TransitionHook = std::function<void(OldState, NewState)>;

    /**
     * @brief 状态+事件组合键，作为事件规则 map 的 key
     */
    struct Key {
        StateEnum state_;
        EventEnum event_;

        bool operator<(const Key& other) const {
            if (state_ != other.state_)
                return state_ < other.state_;
            return event_ < other.event_;
        }
    };

    /**
     * @brief 状态生命周期：进入 + 退出
     * @note 成对出现，避免单独注册导致不对称
     */
    struct Lifecycle {
        Action on_enter;
        Action on_exit;
    };

    /**
     * @brief 构造函数，构造结束即固定，不允许后续修改
     * @param init_state        初始状态
     * @param rules             事件规则表
     * @param lifecycles        状态生命周期表
     * @param resolver          仲裁函数
     * @param validator         可选校验
     * @param transition_hook   可选转移钩子
     * @param hook_timeout_ms   钩子超时(ms)，默认10
     */
    explicit EventFsm(StateEnum init_state,
                      std::map<Key, Action> rules,
                      std::map<StateEnum, Lifecycle> lifecycles,
                      Resolver resolver,
                      Validator validator = nullptr,
                      TransitionHook transition_hook = nullptr,
                      int hook_timeout_ms = 10)
        : current_state_(init_state),
          rules_(std::move(rules)),
          lifecycles_(std::move(lifecycles)),
          resolver_(std::move(resolver)),
          validator_(std::move(validator)),
          on_transition_(std::move(transition_hook)),
          hook_timeout_ms_(hook_timeout_ms) {
        if (!resolver_) {
            throw std::invalid_argument("resolver cannot be null");
        }
        if (validator_ && !validator_(init_state)) {
            throw std::invalid_argument("Invalid initial state");
        }
    }

    // 禁止拷贝/移动，防止状态被意外复制
    EventFsm(const EventFsm&) = delete;
    EventFsm& operator=(const EventFsm&) = delete;
    EventFsm(EventFsm&&) = delete;
    EventFsm& operator=(EventFsm&&) = delete;

    /**
     * @brief 投递事件（仅入队，不立即处理）
     * @param event 待处理事件
     */
    void Post(EventEnum event) {
        pending_events_.Get().push_back(event);
    }

    /**
     * @brief 同步（核心入口：仲裁 + 状态切换 + 事件处理）
     * @note 执行流程：
     *       1. resolver 根据当前状态 + pending_events_ 仲裁目标状态
     *       2. 状态切换：清空整批事件 → exit(old) → 原子切换 → enter(new)
     *       3. 状态保持：取出全部事件 → 按序匹配 rules_ → 合法事件执行 Action，非法事件丢弃
     * @warning 线程安全：EventFsm 内部不再做线程同步。
     *          Sync() 与 SetState() 不可并发执行，同一时刻仅允许一个线程调用其中之一。
     */
    void Sync() {
        RecursionGuard recursion_guard(recursion_in_use_.Get());  // 防递归
        StateEnum target = resolver_(current_state_.load(), pending_events_.Get());
        if (!SetStateInternal(target)) {
            throw std::runtime_error("Invalid state transition in Sync()");
        }
    }

    /**
     * @brief 强制设置状态（teleport / reset）
     * @param state 目标状态
     * @note 与当前状态相同时，不触发任何生命周期
     * @note 状态切换时，pending_events_ 会被清空（遵循"状态切换时事件全部丢弃"原则）
     * @warning 线程安全：EventFsm 内部不再做线程同步。
     *          Sync() 与 SetState() 不可并发执行，同一时刻仅允许一个线程调用其中之一。
     */
    void SetState(StateEnum state) {
        RecursionGuard recursion_guard(recursion_in_use_.Get());  // 防递归
        if (!SetStateInternal(state)) {
            throw std::runtime_error("Invalid state transition in SetState()");
        }
    }

    /**
     * @brief 获取当前状态（原子读）
     */
    StateEnum CurrentState() const {
        return current_state_.load();
    }

private:
    void CallTransitionHookWithTimeout(StateEnum from, StateEnum to) {
        if (!on_transition_) {
            return;
        }
        
        auto start = std::chrono::steady_clock::now();
        on_transition_(from, to);
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        if (duration > hook_timeout_ms_) {
            std::cerr << "[WARNING] Transition hook execution time (" << duration 
                      << "ms) exceeds timeout threshold (" << hook_timeout_ms_ << "ms)" << std::endl;
        }
    }

    /**
     * @brief 设置状态的核心逻辑（不带锁）
     * @param target 目标状态
     * @return true 表示状态设置成功，false 表示校验失败
     * @note 状态切换：清空事件 → exit(old) → 原子切换 → enter(new)
     * @note 状态保持：取出事件 → 匹配 rules_ → 合法事件执行 Action，非法事件丢弃
     */
    inline bool SetStateInternal(StateEnum target) {
        // 有校验函数 → 校验；没有则跳过
        if (validator_ && !validator_(target)) {
            return false;
        }

        StateEnum current = current_state_.load();

        if (target != current) {
            CallTransitionHookWithTimeout(current, target);

            // 状态切换：直接清空整批事件，不执行任何事件回调
            pending_events_.Get().clear();

            // 退出旧状态
            if (auto it = lifecycles_.find(current);
                it != lifecycles_.end() && it->second.on_exit) {
                it->second.on_exit(*this);
            }

            // 切换状态（原子写）
            current_state_.store(target);

            // 进入新状态
            if (auto it = lifecycles_.find(target);
                it != lifecycles_.end() && it->second.on_enter) {
                it->second.on_enter(*this);
            }
        } else {
            // 状态保持：取出事件，做「状态+事件」合法性过滤，合法事件全部执行
            auto events = std::move(pending_events_.Get());
            pending_events_.Get().clear();

            for (const auto& event : events) {
                auto key = Key{target, event};
                if (auto it = rules_.find(key); it != rules_.end() && it->second) {
                    // 合法事件：按队列存储顺序依次执行对应 Action
                    it->second(*this);
                }
                // 非法事件（当前状态下无对应规则）：静默丢弃
            }
        }
        return true;
    }

private:
    std::atomic<StateEnum>         current_state_;     // 当前状态（原子）
    ThreadLocal<bool> recursion_in_use_{false};  // 递归检测存储（实例级）
    ThreadLocal<std::vector<EventEnum>> pending_events_;     // 待处理事件队列（线程本地）

    const std::map<Key, Action>          rules_;              // 状态-事件规则
    const std::map<StateEnum, Lifecycle> lifecycles_;         // 状态生命周期
    const Resolver                       resolver_;           // 仲裁器
    const Validator                      validator_;          // 状态校验器
    const TransitionHook                 on_transition_;      // 状态转移钩子
    const int                            hook_timeout_ms_;    // 钩子超时阈值(ms)
};
