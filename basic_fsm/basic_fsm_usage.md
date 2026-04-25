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
// 注意：必须是 enum class（强类型枚举）
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
               buildStateActions(),  // 状态行为在构造时传入
               [this](OrderState current) { return resolveNext(current); },
               nullptr)  // 暂不演示 validator
    {
    }

    // 模拟外部触发：支付成功
    void onPaymentReceived()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingEvent_ = "paid";
        fsm_.sync();
    }

    // 模拟外部触发：发货
    void onShip()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingEvent_ = "ship";
        fsm_.sync();
    }

    // 模拟外部触发：确认收货
    void onDeliver()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingEvent_ = "deliver";
        fsm_.sync();
    }

    // 模拟外部触发：取消订单
    void onCancel()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingEvent_ = "cancel";
        fsm_.sync();
    }

    OrderState currentState() const
    {
        return fsm_.currentState();  // 线程安全的原子读取
    }

private:
    // 构建状态行为（静态方法，构造时使用）
    static std::map<OrderState, StateActions> buildStateActions()
    {
        return {
            {OrderState::Pending, {
                [] (auto&) { std::cout << "[状态] 进入 Pending，等待付款\n"; },
                nullptr,
                [] (auto&) { std::cout << "[状态] Pending 中... 等待买家付款\n"; }
            }},
            {OrderState::Paid, {
                [] (auto&) { std::cout << "[状态] 进入 Paid，已收到货款\n"; },
                nullptr,
                [] (auto&) { std::cout << "[状态] Paid 中... 等待发货\n"; }
            }},
            {OrderState::Shipped, {
                [] (auto&) { std::cout << "[状态] 进入 Shipped，已发货\n"; },
                nullptr,
                [] (auto&) { std::cout << "[状态] Shipped 中... 运输中\n"; }
            }},
            {OrderState::Delivered, {
                [] (auto&) { std::cout << "[状态] 进入 Delivered，已签收\n"; },
                nullptr,
                nullptr  // 终态，无需 hold
            }},
            {OrderState::Cancelled, {
                [] (auto&) { std::cout << "[状态] 进入 Cancelled，已取消\n"; },
                nullptr,
                nullptr  // 终态，无需 hold
            }}
        };
    }

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

private:
    Fsm fsm_;
    std::string pendingEvent_;  // 模拟外部事件
    std::mutex mutex_;          // 保护 fsm_ 的互斥锁
};
```

## 3. 使用方式

```cpp
int main()
{
    Order order;

    std::cout << "初始状态: Pending\n";
    
    std::cout << "\n--- 用户完成支付 ---\n";
    order.onPaymentReceived();
    
    std::cout << "\n--- 商家发货 ---\n";
    order.onShip();
    
    std::cout << "\n--- 用户确认收货 ---\n";
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
        std::lock_guard<std::mutex> lock(mutex_);
        
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
    std::mutex mutex_;
};
```

## 6. 设计要点总结

| 要点 | 说明 |
|------|------|
| 状态枚举类型 | 必须是 `enum class`，编译时检查 |
| 状态行为初始化 | 通过构造函数传入，构造后不可修改 |
| 线程安全 | `currentState()` 读取是原子的；`sync()`/`setState()` 需要外部加锁 |
| Resolver 必要性 | 构造函数中必须传入，不能为空 |

## 7. API 变化说明

相比旧版本，本版本有以下变化：

| 变化项 | 旧版 | 新版 |
|--------|------|------|
| 状态枚举类型 | `enum` 或 `enum class` | 必须是 `enum class` |
| 状态行为初始化 | 构造后调用 `registerStateActions()` | 构造函数参数传入 |
| currentState_ 类型 | `StateEnum` | `std::atomic<StateEnum>` |
| 运行时注册 | 支持 `registerStateActions()` | 已删除 |

## 8. 线程安全模型

| 资源 | 线程安全 | 说明 |
|------|----------|------|
| `currentState()` | ✅ | `std::atomic` 保护 |
| `stateActions_` | ✅ | `const`，只读 |
| `sync()`/`setState()` | ⚠️ | 需要外部加锁保证串行执行 |
| `resolver_` | ⚠️ | 用户保证回调线程安全 |
| `validator_` | ⚠️ | 用户保证回调线程安全 |

推荐使用方式：

```cpp
std::mutex mtx;
BasicFsm fsm(State::Idle, actions, resolver, validator);

{
    std::lock_guard<std::mutex> lock(mtx);
    fsm.sync();  // 串行执行
}
```
