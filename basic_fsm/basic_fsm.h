#pragma once

#include <functional>
#include <map>

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
 *  1. 定义状态枚举 StateEnum
 *  2. 构造 BasicFsm，传入初始状态和状态仲裁函数
 *  3. 调用 registerStateActions 注册每个状态的 enter/exit/hold
 *  4. 世界变化时调用 sync()
 *  5. 需要强制跳转状态时调用 setState()
 *
 * 作为类成员使用推荐写法（C++11 及以上）;
 */

template <typename StateEnum>
class BasicFsm final
{
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
     * @param resolver 状态仲裁函数【根据当前状态，走对应判断分支，决策目标状态】
     * @param validator 可选：状态值 合法性校验，默认 nullptr
     * explicit 防止隐式构造，保证安全
     */
    explicit BasicFsm(StateEnum initialState,
                      Resolver resolver,
                      Validator validator = nullptr)
        : currentState_(initialState)
        , resolver_(std::move(resolver))
        , validator_(std::move(validator))
    {
    }

    ~BasicFsm() = default;

    // 禁止拷贝与移动，避免状态混乱
    BasicFsm(const BasicFsm&) = delete;
    BasicFsm& operator=(const BasicFsm&) = delete;
    BasicFsm(BasicFsm&&) = delete;
    BasicFsm& operator=(BasicFsm&&) = delete;

    /**
     * @brief 注册所有状态的行为（enter/exit/hold）
     * @param actions 字典：key=状态枚举，value=对应行为
     */
    void registerStateActions(const std::map<StateEnum, StateActions>& actions)
    {
        stateActions_ = actions;
    }

    /**
     * @brief 与世界状态同步（核心调用接口）
     * 会执行：仲裁 -> 状态切换 或 状态保持
     */
    void sync()
    {
        StateEnum desired = resolver_(currentState_);
        setState(desired);
    }

    /**
     * @brief 强制设置状态（teleport / reset 使用）
     * 如果有 validator，则先校验合法性
     * @attention 内部自动判断：
     *  目标 != 当前 → exit + enter
     *  目标 == 当前 → 执行 hold
     */
    void setState(StateEnum target)
    {
        // 有校验函数 → 校验；没有则跳过
        if (validator_ && !validator_(target))
        {
            return;
        }

        if (target != currentState_)
        {
            // 退出旧状态
            auto itOld = stateActions_.find(currentState_);
            if (itOld != stateActions_.end() && itOld->second.exit)
                itOld->second.exit(*this);

            currentState_ = target;

            // 进入新状态
            auto itNew = stateActions_.find(currentState_);
            if (itNew != stateActions_.end() && itNew->second.enter)
                itNew->second.enter(*this);
        }
        else
        {
            // 状态保持
            auto itCurr = stateActions_.find(currentState_);
            if (itCurr != stateActions_.end() && itCurr->second.hold)
                itCurr->second.hold(*this);
        }
    }

    /**
     * @brief 获取当前状态
     */
    StateEnum currentState() const
    {
        return currentState_;
    }

private:
    StateEnum              currentState_;
    Resolver               resolver_;
    Validator              validator_;
    std::map<StateEnum, StateActions> stateActions_;
};
