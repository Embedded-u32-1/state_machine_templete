#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <type_traits>

/**
 * @brief 通用基础状态机（普通状态机）
 * @note 设计思想：
 *       1. 世界变化时调用 Sync()，与世界状态同步
 *       2. Sync() 内部通过 resolver 仲裁目标状态
 *       3. 状态变化 → 执行 exit + enter
 *       4. 状态不变 → 执行 hold
 *       5. 手动强制设置状态使用 SetState()
 *       6. 构造时必须传入初始状态 + 仲裁函数 resolver
 *       7. 校验函数 validator 为可选，用于防止非法数值强转状态枚举
 *
 * @note 使用方式：
 *       1. 定义状态枚举（必须是 enum class）
 *       2. 构造 BasicFsm，传入初始状态、状态行为、仲裁函数
 *       3. 世界变化时调用 Sync()
 *       4. 需要强制跳转状态时调用 SetState()
 *
 * @note 线程安全：
 *       - 内部使用 mutex 保证 Sync/SetState 串行执行
 *       - CurrentState() 读取是线程安全的（atomic）
 *       - resolver/validator 回调如果涉及共享数据，用户需自行保证线程安全
 *       - 递归检测：若在 enter/exit/hold 回调中调用 Sync/SetState，会抛出异常
 */

template <typename StateEnum>
class BasicFsm final {
    // static_assert 检查 StateEnum 必须是 enum class（强类型枚举）
    static_assert(std::is_scoped_enum_v<StateEnum>,
                  "StateEnum must be an enum class (scoped enum)");

private:
    // RAII 递归检测守卫：防止在 enter/exit/hold 回调中调用 Sync/SetState
    struct RecursionGuard {
        // inline static thread_local：C++17 类内直接初始化，每个线程独立存储
        // - inline: 允许类内初始化（C++17）
        // - static: 类内共享
        // - thread_local: 每个线程独立一份
        // 原理：同一线程内递归构造会检测到 s_in_use_ == true，从而抛出异常
        inline static thread_local bool s_in_use_ = false;

        RecursionGuard() {
            if (s_in_use_) {
                throw std::runtime_error("Recursive state transition detected");
            }
            s_in_use_ = true;
        }

        ~RecursionGuard() {
            s_in_use_ = false;
        }
    };

public:
    using FsmRef = BasicFsm&;
    using Action = std::function<void(FsmRef)>;
    using Resolver = std::function<StateEnum(StateEnum)>;
    using Validator = std::function<bool(StateEnum)>;
    using TransitionHook = std::function<void(StateEnum from, StateEnum to)>;

    struct StateActions {
        Action enter;  // 进入该状态时执行
        Action exit;   // 离开该状态时执行
        Action hold;   // 停留在该状态时执行（状态不变）
    };

    /**
     * @brief 构造函数
     * @param initial_state 初始状态
     * @param state_actions 状态行为字典：key=状态枚举，value=对应行为
     * @param resolver 状态仲裁函数【根据当前状态，走对应判断分支，决策目标状态】
     * @param validator 可选：状态值合法性校验，默认 nullptr
     * @param transition_hook 可选：状态转移钩子函数，默认 nullptr
     * @param hook_timeout_ms 可选：钩子执行超时时间（毫秒），默认 10ms
     * @throws std::invalid_argument resolver 不能为空
     * @throws std::invalid_argument 初始状态校验失败
     * @note state_actions 在构造后不可修改
     * @note transition_hook 在构造后不可修改，确保线程安全
     */
    explicit BasicFsm(StateEnum initial_state,
                      std::map<StateEnum, StateActions> state_actions,
                      Resolver resolver,
                      Validator validator = nullptr,
                      TransitionHook transition_hook = nullptr,
                      int hook_timeout_ms = 10)
        : current_state_(initial_state),
          state_actions_(std::move(state_actions)),
          resolver_(std::move(resolver)),
          validator_(std::move(validator)),
          on_transition_(std::move(transition_hook)),
          hook_timeout_ms_(hook_timeout_ms) {
        if (!resolver_) {
            throw std::invalid_argument("resolver cannot be null");
        }
        if (validator_ && !validator_(initial_state)) {
            throw std::invalid_argument("Invalid initial state");
        }
    }

    ~BasicFsm() = default;

    // 禁止拷贝与移动，避免状态混乱
    BasicFsm(const BasicFsm&) = delete;
    BasicFsm& operator=(const BasicFsm&) = delete;
    BasicFsm(BasicFsm&&) = delete;
    BasicFsm& operator=(BasicFsm&&) = delete;

    /**
     * @brief 与世界状态同步（核心调用接口）
     * @note 会执行：仲裁 -> 状态切换 或 状态保持
     * @note 内部加锁保证线程安全，锁粒度覆盖整个仲裁+切换过程
     * @note 防止递归：若在 enter/exit/hold 回调中调用 Sync()，会抛出异常
     * @throws std::runtime_error 状态转移失败时抛出
     */
    void Sync() {
        RecursionGuard recursion_guard;                  // 1. 防递归
        std::lock_guard<std::mutex> lock(action_mutex_);  // 2. 串行
        StateEnum desired = resolver_(current_state_.load());
        if (!SetStateInternal(desired)) {
            throw std::runtime_error("Invalid state transition in Sync()");
        }
    }

    /**
     * @brief 强制设置状态（teleport / reset 使用）
     * @param target 目标状态
     * @note 如果有 validator，则先校验合法性
     * @note 内部加锁保证线程安全，锁粒度覆盖整个校验+切换过程
     * @note 防止递归：若在 enter/exit/hold 回调中调用 SetState()，会抛出异常
     * @throws std::runtime_error 状态转移失败时抛出
     */
    void SetState(StateEnum target) {
        RecursionGuard recursion_guard;                  // 1. 防递归
        std::lock_guard<std::mutex> lock(action_mutex_);  // 2. 串行
        if (!SetStateInternal(target)) {
            throw std::runtime_error("Invalid state transition in SetState()");
        }
    }

    /**
     * @brief 获取当前状态
     * @return StateEnum 当前状态枚举值
     * @note 线程安全的原子读取
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
     * @note 由 Sync/SetState 在持有锁的前提下调用
     * @note 内部自动判断：
     *       - 目标 != 当前 → exit + enter
     *       - 目标 == 当前 → 执行 hold
     * @return true 表示状态设置成功，false 表示校验失败
     */
    inline bool SetStateInternal(StateEnum target) {
        // 有校验函数 → 校验；没有则跳过
        if (validator_ && !validator_(target)) {
            return false;
        }

        StateEnum current = current_state_.load();
        if (target != current) {
            CallTransitionHookWithTimeout(current, target);

            // 退出旧状态
            auto it_old = state_actions_.find(current);
            if (it_old != state_actions_.end() && it_old->second.exit) {
                it_old->second.exit(*this);
            }

            current_state_.store(target);

            // 进入新状态
            auto it_new = state_actions_.find(target);
            if (it_new != state_actions_.end() && it_new->second.enter) {
                it_new->second.enter(*this);
            }
        } else {
            // 状态保持
            auto it_curr = state_actions_.find(target);
            if (it_curr != state_actions_.end() && it_curr->second.hold) {
                it_curr->second.hold(*this);
            }
        }
        return true;
    }

private:
    std::atomic<StateEnum> current_state_;
    mutable std::mutex action_mutex_;  // 保证 Sync/SetState 串行执行
    const std::map<StateEnum, StateActions> state_actions_;  // 不可修改
    Resolver resolver_;
    Validator validator_;
    const TransitionHook on_transition_;
    const int hook_timeout_ms_;
};

