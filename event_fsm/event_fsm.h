#pragma once

#include <functional>
#include <map>

/**
 * @brief 事件驱动状态机（Header-Only）
 * @tparam StateEnum 状态枚举，建议使用 enum class
 * @tparam EventEnum 事件枚举，建议使用 enum class
 * @note 所有规则与生命周期均为内部拷贝存储，外部传入后无需维持生命周期
 * @note 支持批量 map 导入，无零散注册，结构集中易维护
 */
template <typename StateEnum, typename EventEnum>
class EventFsm final {
public:
    using SelfType = EventFsm&;
    using Action   = std::function<void(SelfType)>;

    /**
     * @brief 状态+事件组合键，作为事件规则 map 的 key
     */
    struct Key {
        StateEnum state;
        EventEnum event;

        bool operator<(const Key& other) const {
            if (state != other.state)
                return state < other.state;
            return event < other.event;
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
     * @brief 构造函数，必须指定初始状态
     */
    explicit EventFsm(StateEnum init_state)
        : current_state_(init_state) {}

    // 禁止拷贝/移动，防止状态被意外复制
    EventFsm(const EventFsm&) = delete;
    EventFsm& operator=(const EventFsm&) = delete;
    EventFsm(EventFsm&&) = delete;
    EventFsm& operator=(EventFsm&&) = delete;

    /**
     * @brief 批量注册【事件规则表】，内部拷贝存储
     * @param rules 外部 map: {Key} → Action
     */
    void register_rules(const std::map<Key, Action>& rules) {
        rules_ = rules;
    }

    /**
     * @brief 批量注册【状态生命周期表】，内部拷贝存储
     * @param lifecycles 外部 map: StateEnum → Lifecycle
     */
    void register_lifecycles(const std::map<StateEnum, Lifecycle>& lifecycles) {
        lifecycles_ = lifecycles;
    }

    /**
     * @brief 派发事件
     * @note 查找匹配 Key，执行对应 Action
     */
    void dispatch(EventEnum event) {
        auto key = Key{current_state_, event};
        auto it = rules_.find(key);

        if (it != rules_.end() && it->second) {
            it->second(*this);
        }
    }

    /**
     * @brief 状态切换（自动触发旧状态 exit、新状态 enter）
     * @param next_state 目标状态
     * @note 与当前状态相同时，不触发任何生命周期
     */
    void transition(StateEnum next_state) {
        if (current_state_ == next_state)
            return;

        // 退出旧状态
        auto old_lc = lifecycles_.find(current_state_);
        if (old_lc != lifecycles_.end() && old_lc->second.on_exit) {
            old_lc->second.on_exit(*this);
        }

        // 切换状态
        current_state_ = next_state;

        // 进入新状态
        auto new_lc = lifecycles_.find(current_state_);
        if (new_lc != lifecycles_.end() && new_lc->second.on_enter) {
            new_lc->second.on_enter(*this);
        }
    }

    /**
     * @brief 获取当前状态
     */
    StateEnum state() const {
        return current_state_;
    }

private:
    StateEnum                   current_state_;
    std::map<Key, Action>       rules_;       // 事件规则（内部存储）
    std::map<StateEnum, Lifecycle> lifecycles_; // 生命周期（内部存储）
};

/*
===============================================================================
使用示例（在你的类构造函数中）：

using Fsm = EventFsm<State, Event>;

std::map<Fsm::Key, Fsm::Action> rules = {
    {{State::idle, Event::start}, [this](Fsm& fsm) {
        // do something
        fsm.transition(State::running);
    }},
    {{State::running, Event::stop}, [this](Fsm& fsm) {
        fsm.transition(State::idle);
    }}
};

std::map<State, Fsm::Lifecycle> lifecycles = {
    {State::idle,    {[](Fsm&){}, [](Fsm&){}}},
    {State::running, {[](Fsm&){}, [](Fsm&){}}},
    {State::fault,   {[](Fsm&){}, [](Fsm&){}}}
};

fsm_.register_rules(rules);
fsm_.register_lifecycles(lifecycles);
===============================================================================
*/
