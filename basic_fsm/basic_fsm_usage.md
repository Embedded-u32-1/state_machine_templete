# BasicFsm 使用示例

本文档展示 `BasicFsm` 状态机模板在业务类中的使用方式。

## 1. 核心概念澄清

| 函数 | 签名 | 作用 |
|------|------|------|
| [`Resolver`](basic_fsm/basic_fsm.h:33) | `StateEnum(StateEnum current)` | 根据**当前状态**，走对应判断分支，**决策目标状态** |
| [`Validator`](basic_fsm/basic_fsm.h:34) | `bool(StateEnum target)` | 校验**目标状态值**是否合法 |

**`Resolver` 的关键**：它接收当前状态作为参数，通过内部判断返回目标状态。这意味着状态转移逻辑由 `Resolver` 决定。

**`Validator` 的关键**：它只接收目标状态，用于校验该状态值是否可以设置（例如防止外部直接传入非法枚举值）。

## 2. 业务场景：订单状态机

```cpp
#include "basic_fsm/basic_fsm.h"
#include <iostream>
#include <map>

// ==================== 订单状态定义 ====================
enum class OrderState { Pending, Paid, Shipped, Delivered, Cancelled };

// ==================== 订单业务类 ====================
class Order
{
public:
    using Fsm = BasicFsm<OrderState>;
    using StateActions = Fsm::StateActions;
    using Resolver = Fsm::Resolver;
    using Validator = Fsm::Validator;

    Order()
        : fsm_(OrderState::Pending, 
               [this](OrderState current) { return resolveNext(current); },
               nullptr)  // 暂不演示 validator
    {
        registerActions();
    }

    // 模拟外部触发：支付成功
    void onPaymentReceived()
    {
        pendingEvent_ = "paid";
        fsm_.sync();
    }

    // 模拟外部触发：发货
    void onShip()
    {
        pendingEvent_ = "ship";
        fsm_.sync();
    }

    // 模拟外部触发：确认收货
    void onDeliver()
    {
        pendingEvent_ = "deliver";
        fsm_.sync();
    }

    // 模拟外部触发：取消订单
    void onCancel()
    {
        pendingEvent_ = "cancel";
        fsm_.sync();
    }

    OrderState currentState() const
    {
        return fsm_.currentState();
    }

private:
    // Resolver：根据当前状态，决策目标状态
    // 这是状态机的核心转移逻辑
    OrderState resolveNext(OrderState current)
    {
        // 根据当前状态 + 待处理事件，决定目标状态
        if (pendingEvent_.empty())
            return current;  // 无事件，保持当前状态

        if (current == OrderState::Pending && pendingEvent_ == "paid")
            return OrderState::Paid;
        if (current == OrderState::Paid && pendingEvent_ == "ship")
            return OrderState::Shipped;
        if (current == OrderState::Shipped && pendingEvent_ == "deliver")
            return OrderState::Delivered;
        if ((current == OrderState::Pending || current == OrderState::Paid) 
            && pendingEvent_ == "cancel")
            return OrderState::Cancelled;
        
        return current;  // 其他情况保持不变
    }

    // 注册状态行为
    void registerActions()
    {
        std::map<OrderState, StateActions> actions = {
            {OrderState::Pending, {
                [] (auto&) { std::cout << "[状态] 进入 Pending，等待付款\\n"; },
                nullptr,
                [] (auto&) { std::cout << "[状态] Pending 中... 等待买家付款\\n"; }
            }},
            {OrderState::Paid, {
                [] (auto&) { std::cout << "[状态] 进入 Paid，已收到货款\\n"; },
                nullptr,
                [] (auto&) { std::cout << "[状态] Paid 中... 等待发货\\n"; }
            }},
            {OrderState::Shipped, {
                [] (auto&) { std::cout << "[状态] 进入 Shipped，已发货\\n"; },
                nullptr,
                [] (auto&) { std::cout << "[状态] Shipped 中... 运输中\\n"; }
            }},
            {OrderState::Delivered, {
                [] (auto&) { std::cout << "[状态] 进入 Delivered，已签收\\n"; },
                nullptr,
                nullptr  // 终态，无需 hold
            }},
            {OrderState::Cancelled, {
                [] (auto&) { std::cout << "[状态] 进入 Cancelled，已取消\\n"; },
                nullptr,
                nullptr  // 终态，无需 hold
            }}
        };
        fsm_.registerStateActions(actions);
    }

private:
    Fsm fsm_;
    std::string pendingEvent_;  // 模拟外部事件
};
```

## 3. 使用方式

```cpp
int main()
{
    Order order;

    std::cout << "初始状态: Pending\\n";
    
    std::cout << "\\n--- 用户完成支付 ---\\n";
    order.onPaymentReceived();
    
    std::cout << "\\n--- 商家发货 ---\\n";
    order.onShip();
    
    std::cout << "\\n--- 用户确认收货 ---\\n";
    order.onDeliver();
    
    return 0;
}
```

## 4. 状态流转示意

```
            ┌──[cancel]──> Cancelled
            │
Pending ──[paid]──> Paid ──[ship]──> Shipped ──[deliver]──> Delivered
```

## 5. 与世界状态同步

在实际游戏中，`sync()` 通常由游戏引擎在每帧调用：

```cpp
class GameObject
{
public:
    // 游戏循环每帧调用
    void update()
    {
        // 1. 从外界获取输入/事件
        gatherExternalEvents();
        
        // 2. 同步状态机
        fsm_.sync();
    }

private:
    void gatherExternalEvents()
    {
        // 从输入设备、网络、UI 等获取事件
        // 设置 pendingEvent_
    }

    Fsm fsm_;
    std::string pendingEvent_;
};
```

## 6. 设计要点总结

| 要点 | 说明 |
|------|------|
| **Resolver 接收当前状态** | 根据当前状态决定目标状态，类似状态转移表 |
| **状态转移逻辑内聚** | 所有状态转移规则集中在 `resolveNext()` 中 |
| **行为分离** | `enter`/`exit`/`hold` 与转移逻辑分离，便于维护 |
| **外部事件存储** | 外部事件存入 `pendingEvent_`，在 `sync()` 时生效 |
