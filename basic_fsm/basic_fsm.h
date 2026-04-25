#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <type_traits>

/**
 * @brief 通用基础状态机（普通状态机）
 * 设计思想：
 *  1. 世界变化时调用 sync()，与世界状态同步
 *  2. sync() 内部通过 resolver 仲裁目标状态
 *  3. 状态变化 → 执行 exit + enter
 *  4. 状态不变 → 执行 hold
 *  5. 手动强制设置状态使用 setState()
 *  6. 构造时必须传入初始状态 + 仲裁函数 resolver
 *  7. 校验函数 validator 为可选，用于防止非法数值强转状态枚举
 *
 * 使用方式：
 *  1. 定义状态枚举（必须是 enum class）
 *  2. 构造 BasicFsm，传入初始状态、状态行为、仲裁函数
 *  3. 世界变化时调用 sync()
 *  4. 需要强制跳转状态时调用 setState()
 *
 * 线程安全：
 *  - currentState() 读取是线程安全的（atomic）
 *  - sync()/setState() 需要外部加锁保证串行执行
 *  - resolver/validator 回调需要用户保证线程安全
 */

template <typename StateEnum>
class BasicFsm final
{
    // static_assert 检查 StateEnum 必须是 enum class（强类型枚举）
    static_assert(std::is_scoped_enum_v<StateEnum>, "StateEnum must be an enum class (scoped enum)");

public:
    using FsmRef    = BasicFsm&;
    using Action    = std::function<void(FsmRef)>;
    using Resolver  = std::function<StateEnum(StateEnum)>;
    using Validator = std::function<bool(StateEnum)>;

    struct StateActions
    {
        Action enter;  // 进入该状态时执行
        Action exit;   // 离开该状态时执行
        Action hold;   // 停留在该状态时执行（状态不变）
    };

    /**
     * @brief 构造函数
     * @param initialState 初始状态
     * @param stateActions 状态行为字典：key=状态枚举，value=对应行为
     * @param resolver 状态仲裁函数【根据当前状态，走对应判断分支，决策目标状态】
     * @param validator 可选：状态值合法性校验，默认 nullptr
     * @attention resolver 不能为空，否则抛出 std::invalid_argument
     * @attention stateActions 在构造后不可修改
     */
    explicit BasicFsm(StateEnum initialState,
                      std::map<StateEnum, StateActions> stateActions,
                      Resolver resolver,
                      Validator validator = nullptr)
        : currentState_(initialState)
        , stateActions_(std::move(stateActions))
        , resolver_(std::move(resolver))
        , validator_(std::move(validator))
    {
        if (!resolver_)
        {
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
     * 会执行：仲裁 -> 状态切换 或 状态保持
     * @attention 需要外部加锁保证线程安全
     */
    void sync()
    {
        StateEnum desired = resolver_(currentState_.load());
        setState(desired);
    }

    /**
     * @brief 强制设置状态（teleport / reset 使用）
     * 如果有 validator，则先校验合法性
     * @attention 内部自动判断：
     *  目标 != 当前 → exit + enter
     *  目标 == 当前 → 执行 hold
     * @attention 需要外部加锁保证线程安全
     */
    void setState(StateEnum target)
    {
        // 有校验函数 → 校验；没有则跳过
        if (validator_ && !validator_(target))
        {
            return;
        }

        if (target != currentState_.load())
        {
            // 退出旧状态
            auto itOld = stateActions_.find(currentState_.load());
            if (itOld != stateActions_.end() && itOld->second.exit)
                itOld->second.exit(*this);

            currentState_.store(target);

            // 进入新状态
            auto itNew = stateActions_.find(target);
            if (itNew != stateActions_.end() && itNew->second.enter)
                itNew->second.enter(*this);
        }
        else
        {
            // 状态保持
            auto itCurr = stateActions_.find(target);
            if (itCurr != stateActions_.end() && itCurr->second.hold)
                itCurr->second.hold(*this);
        }
    }

    /**
     * @brief 获取当前状态
     * @note 线程安全的原子读取
     */
    StateEnum currentState() const
    {
        return currentState_.load();
    }

private:
    std::atomic<StateEnum>               currentState_;
    const std::map<StateEnum, StateActions> stateActions_;  // 不可修改
    Resolver                              resolver_;
    Validator                             validator_;
};
