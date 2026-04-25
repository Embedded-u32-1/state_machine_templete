#pragma once

#include <atomic>
#include <functional>
#include <map>
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
 *       - CurrentState() 读取是线程安全的（atomic）
 *       - Sync()/SetState() 需要外部加锁保证串行执行
 *       - resolver/validator 回调需要用户保证线程安全
 */

template <typename StateEnum>
class BasicFsm final {
    // static_assert 检查 StateEnum 必须是 enum class（强类型枚举）
    static_assert(std::is_scoped_enum_v<StateEnum>,
                  "StateEnum must be an enum class (scoped enum)");

public:
    using FsmRef = BasicFsm&;
    using Action = std::function<void(FsmRef)>;
    using Resolver = std::function<StateEnum(StateEnum)>;
    using Validator = std::function<bool(StateEnum)>;

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
     * @throws std::invalid_argument resolver 不能为空
     * @note state_actions 在构造后不可修改
     */
    explicit BasicFsm(StateEnum initial_state,
                      std::map<StateEnum, StateActions> state_actions,
                      Resolver resolver,
                      Validator validator = nullptr)
        : current_state_(initial_state),
          state_actions_(std::move(state_actions)),
          resolver_(std::move(resolver)),
          validator_(std::move(validator)) {
        if (!resolver_) {
            throw std::invalid_argument("resolver cannot be null");
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
     * @note 需要外部加锁保证线程安全
     */
    void Sync() {
        StateEnum desired = resolver_(current_state_.load());
        SetState(desired);
    }

    /**
     * @brief 强制设置状态（teleport / reset 使用）
     * @param target 目标状态
     * @note 如果有 validator，则先校验合法性
     * @note 内部自动判断：
     *       - 目标 != 当前 → exit + enter
     *       - 目标 == 当前 → 执行 hold
     * @note 需要外部加锁保证线程安全
     */
    void SetState(StateEnum target) {
        // 有校验函数 → 校验；没有则跳过
        if (validator_ && !validator_(target)) {
            return;
        }

        if (target != current_state_.load()) {
            // 退出旧状态
            auto it_old = state_actions_.find(current_state_.load());
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
    std::atomic<StateEnum> current_state_;
    const std::map<StateEnum, StateActions> state_actions_;  // 不可修改
    Resolver resolver_;
    Validator validator_;
};

