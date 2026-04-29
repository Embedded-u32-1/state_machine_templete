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
 * @brief 通用基础状态机
 * @note 设计：
 *   1. 核心特性：
 *      - 模板类：通过 std::is_scoped_enum_v，编译时期 确保 传入类型安全
 *      - 仲裁式状态机：通过 resolver 函数决定下一个目标状态
 *      - 状态行为：每个状态可注册 enter/exit/hold 三种回调
 *   2. 状态转移：
 *      - Sync()：仲裁式同步，与外部世界状态对齐
 *      - SetState()：强制设置状态（用于 teleport/reset）
 *      - 状态保持时触发 hold 回调
 *   3. 线程安全：
 *      - mutex 保证 Sync/SetState 串行执行
 *      - atomic 存储当前状态，保证读取原子性
 *      - 递归检测：若在 enter/exit/hold 回调中调用 Sync/SetState，抛异常
 *   4. 禁止拷贝/移动：避免多实例导致状态混乱
 */

template <typename StateEnum>
class BasicFsm final {
    static_assert(std::is_scoped_enum_v<StateEnum>,
                  "StateEnum must be an enum class (scoped enum)");

private:
    struct RecursionGuard {
        // - inline: 允许类内初始化（C++17）
        // - static: 类内共享
        // - thread_local: 每个线程独立一份
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

    struct StateActions {
        Action enter;  // 进入该状态时执行
        Action exit;   // 离开该状态时执行
        Action hold;   // 停留在该状态时执行（状态不变）
    };

    /**
     * @brief 构造函数
     * @param initial_state 初始状态
     * @param state_actions 状态行为字典
     * @param resolver 仲裁函数
     * @param validator 可选校验
     * @param transition_hook 可选转移钩子
     * @param hook_timeout_ms 钩子超时(ms)，默认10
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
     * @note    仲裁式--状态 切换 / 保持
     */
    void Sync() {
        RecursionGuard recursion_guard;                  // 1. 防递归
        std::lock_guard<std::mutex> lock(action_mutex_);  // 2. 串行
        StateEnum desired = resolver_(current_state_.load());
        if (!SetStateInternal(desired)) {
            throw std::runtime_error("Invalid state transition in Sync()");
        }
    }

    /** @brief 强制设置状态（teleport / reset 使用） */
    void SetState(StateEnum target) {
        RecursionGuard recursion_guard;                  // 1. 防递归
        std::lock_guard<std::mutex> lock(action_mutex_);  // 2. 串行
        if (!SetStateInternal(target)) {
            throw std::runtime_error("Invalid state transition in SetState()");
        }
    }

    /** @brief 获取当前状态（原子读） */
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
    std::mutex action_mutex_;
    const std::map<StateEnum, StateActions> state_actions_;
    const Resolver resolver_;
    const Validator validator_;
    const TransitionHook on_transition_;
    const int hook_timeout_ms_;
};

