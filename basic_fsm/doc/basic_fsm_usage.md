# BasicFsm 使用手册

本文档详细介绍 `BasicFsm` 状态机模板的设计思想、API接口和使用方法。

---

## 1. 设计思想

`BasicFsm` 是一种**仲裁式状态机**，其核心思想是：

| 步骤 | 行为 |
|------|------|
| 1 | 世界变化时调用 [`Sync()`](basic_fsm/basic_fsm.h:109) |
| 2 | [`Sync()`](basic_fsm/basic_fsm.h:109) 内部通过 **resolver** 仲裁目标状态 |
| 3 | 状态变化 → 执行 `exit` + `enter` |
| 4 | 状态不变 → 执行 `hold` |
| 5 | 手动强制设置状态使用 [`SetState()`](basic_fsm/basic_fsm.h:123) |

---

## 2. 核心概念

### 2.1 状态枚举

状态枚举**必须是 `enum class`**（强类型枚举），编译时检查：

```cpp
enum class OrderState { kPending, kPaid, kShipped, kDelivered, kCancelled };
```

### 2.2 状态行为 (StateActions)

每个状态可配置三种行为：

```cpp
struct StateActions {
    Action enter;  // 进入该状态时执行
    Action exit;   // 离开该状态时执行
    Action hold;   // 停留在该状态时执行（状态不变）
};
```

其中 `Action` 定义为：
```cpp
using Action = std::function<void(FsmRef)>;  // FsmRef = BasicFsm&
```

### 2.3 仲裁函数 (Resolver)

```cpp
using Resolver = std::function<StateEnum(StateEnum)>;
```

**作用**：根据当前状态，决策目标状态。这是状态机的核心转移逻辑。

**签名**：`StateEnum ResolveNext(StateEnum current)`

```cpp
OrderState ResolveNext(OrderState current) {
    // 根据当前状态 + 待处理事件，决定目标状态
    if (current == OrderState::kPending && pending_event_ == "paid")
        return OrderState::kPaid;
    if (current == OrderState::kPaid && pending_event_ == "ship")
        return OrderState::kShipped;
    // ...
    return current;  // 保持不变
}
```

### 2.4 校验函数 (Validator)

```cpp
using Validator = std::function<bool(StateEnum)>;
```

**作用**：校验目标状态值是否合法（如防止外部直接传入非法枚举值）。可选参数。

### 2.5 状态转移钩子 (TransitionHook)

```cpp
using OldState = StateEnum;
using NewState = StateEnum;
using TransitionHook = std::function<void(OldState, NewState)>;
```

**作用**：在每次状态转移发生时调用，用于调试和监控目的。

| 适用场景 |
|----------|
| 日志记录：记录状态转移的轨迹和时间点 |
| 性能统计：统计状态转移次数、频率、耗时 |
| 监控告警：监控异常状态转移，触发告警 |
| 审计追踪：记录状态流转历史，满足合规要求 |

| 注意事项 |
|----------|
| ⚠️ 主要用于调试：此钩子在每次状态转移时同步执行，会影响状态机性能 |
| ⚠️ 不要滥用：严禁在此钩子中执行耗时操作（如IO、网络请求、锁等待） |
| ⚠️ 异常安全：钩子抛出的异常会被捕获并记录，不影响状态机核心逻辑 |
| ⚠️ 线程安全：确保钩子函数自身是线程安全的 |

| 参数 |
|------|
| `param_1` : 转移源状态 |
| `param_2` : 转移目标状态 |

---

## 3. API 接口

### 3.1 类型别名

| 类型 | 定义 | 说明 |
|------|------|------|
| `FsmRef` | `BasicFsm&` | 状态机引用，用于回调 |
| `Action` | `std::function<void(FsmRef)>` | 状态行为回调 |
| `Resolver` | `std::function<StateEnum(StateEnum)>` | 状态仲裁函数 |
| `Validator` | `std::function<bool(StateEnum)>` | 状态校验函数 |
| `OldState` | `StateEnum` | 状态转移钩子源状态类型 |
| `NewState` | `StateEnum` | 状态转移钩子目标状态类型 |
| `TransitionHook` | `std::function<void(OldState, NewState)>` | 状态转移钩子 |

### 3.2 构造函数

```cpp
explicit BasicFsm(
    StateEnum initial_state,                          // 初始状态
    std::map<StateEnum, StateActions> state_actions, // 状态行为字典
    Resolver resolver,                                // 仲裁函数【必填】
    Validator validator = nullptr,                    // 校验函数【可选】
    TransitionHook transition_hook = nullptr,         // 状态转移钩子【可选】
    int hook_timeout_ms = 10                          // 钩子执行超时时间（毫秒）【可选】
);
```

**注意**：
- `resolver` 不能为空，否则抛出 `std::invalid_argument`
- `state_actions` 在构造后不可修改
- `transition_hook` 在构造后不可修改，确保线程安全
- 钩子执行超时默认 10ms，超过会输出警告信息

### 3.3 核心方法

| 方法 | 说明 |
|------|------|
| [`void Sync()`](basic_fsm/basic_fsm.h:109) | 与世界状态同步（核心调用接口） |
| [`void SetState(StateEnum target)`](basic_fsm/basic_fsm.h:123) | 强制设置状态（teleport/reset 使用） |
| [`StateEnum CurrentState() const`](basic_fsm/basic_fsm.h:172) | 获取当前状态（线程安全） |

---

## 4. 使用示例

### 4.1 完整订单状态机示例

```cpp
#include "basic_fsm/basic_fsm.h"
#include <iostream>
#include <map>

// ==================== 订单状态定义 ====================
enum class OrderState { kPending, kPaid, kShipped, kDelivered, kCancelled };

// ==================== 订单业务类 ====================
class Order {
public:
    using Fsm = BasicFsm<OrderState>;
    using StateActions = Fsm::StateActions;
    using Resolver = Fsm::Resolver;
    using Validator = Fsm::Validator;

    Order()
        : fsm_(OrderState::kPending,
               BuildStateActions(),
               [this](OrderState current) { return ResolveNext(current); },
               nullptr) {}

    // 模拟外部触发：支付成功
    void OnPaymentReceived() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_event_ = "paid";
        fsm_.Sync();
    }

    // 模拟外部触发：发货
    void OnShip() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_event_ = "ship";
        fsm_.Sync();
    }

    // 模拟外部触发：确认收货
    void OnDeliver() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_event_ = "deliver";
        fsm_.Sync();
    }

    // 模拟外部触发：取消订单
    void OnCancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_event_ = "cancel";
        fsm_.Sync();
    }

    OrderState CurrentState() const { return fsm_.CurrentState(); }

private:
    static std::map<OrderState, StateActions> BuildStateActions() {
        return {
            {OrderState::kPending, {
                [] (auto&) { std::cout << "[状态] 进入 Pending，等待付款\n"; },
                nullptr,
                [] (auto&) { std::cout << "[状态] Pending 中... 等待买家付款\n"; }
            }},
            {OrderState::kPaid, {
                [] (auto&) { std::cout << "[状态] 进入 Paid，已收到货款\n"; },
                nullptr,
                [] (auto&) { std::cout << "[状态] Paid 中... 等待发货\n"; }
            }},
            {OrderState::kShipped, {
                [] (auto&) { std::cout << "[状态] 进入 Shipped，已发货\n"; },
                nullptr,
                [] (auto&) { std::cout << "[状态] Shipped 中... 运输中\n"; }
            }},
            {OrderState::kDelivered, {
                [] (auto&) { std::cout << "[状态] 进入 Delivered，已签收\n"; },
                nullptr,
                nullptr
            }},
            {OrderState::kCancelled, {
                [] (auto&) { std::cout << "[状态] 进入 Cancelled，已取消\n"; },
                nullptr,
                nullptr
            }}
        };
    }

    OrderState ResolveNext(OrderState current) {
        if (pending_event_.empty())
            return current;

        if (current == OrderState::kPending && pending_event_ == "paid")
            return OrderState::kPaid;
        if (current == OrderState::kPaid && pending_event_ == "ship")
            return OrderState::kShipped;
        if (current == OrderState::kShipped && pending_event_ == "deliver")
            return OrderState::kDelivered;
        if ((current == OrderState::kPending || current == OrderState::kPaid) 
            && pending_event_ == "cancel")
            return OrderState::kCancelled;
        
        return current;
    }

private:
    Fsm fsm_;
    std::string pending_event_;
    std::mutex mutex_;
};
```

### 4.2 使用方式

```cpp
int main() {
    Order order;

    std::cout << "初始状态: Pending\n";
    
    std::cout << "\n--- 用户完成支付 ---\n";
    order.OnPaymentReceived();
    
    std::cout << "\n--- 商家发货 ---\n";
    order.OnShip();
    
    std::cout << "\n--- 用户确认收货 ---\n";
    order.OnDeliver();
    
    return 0;
}
```

---

## 5. 状态流转示意

```
            ┌──[cancel]──> Cancelled
            │
Pending ──[paid]──> Paid ──[ship]──> Shipped ──[deliver]──> Delivered
```

---

## 6. 线程安全模型

| 资源 | 线程安全 | 说明 |
|------|----------|------|
| [`CurrentState()`](basic_fsm/basic_fsm.h:172) | ✅ | `std::atomic` 保护，原子读取 |
| [`Sync()`](basic_fsm/basic_fsm.h:109) | ✅ | 内部有 `mutex`，串行执行 |
| [`SetState()`](basic_fsm/basic_fsm.h:123) | ✅ | 内部有 `mutex`，串行执行 |
| `state_actions_` | ✅ | `const`，只读 |
| `resolver_` | ⚠️ | 用户需保证回调线程安全 |
| `validator_` | ⚠️ | 用户需保证回调线程安全 |

### 推荐使用方式

```cpp
std::mutex mtx;
BasicFsm fsm(State::kIdle, actions, resolver, validator);

{
    std::lock_guard<std::mutex> lock(mtx);  // 保护业务数据（如 pending_event_）
    fsm.Sync();  // Sync() 内部已保证串行执行
}
```

> **注意**：[`Sync()`](basic_fsm/basic_fsm.h:109) 内部已有 `mutex` 保证串行执行，外部加锁是为了保护业务层的共享数据。

### 递归检测

`BasicFsm` 内部有递归检测机制：

- 若在 `enter`/`exit`/`hold` 回调中调用 [`Sync()`](basic_fsm/basic_fsm.h:109) 或 [`SetState()`](basic_fsm/basic_fsm.h:123)，会抛出 `std::runtime_error("Recursive state transition detected")`
- 使用 `thread_local` 实现，每个线程独立检测

---

## 7. 游戏循环中的典型用法

```cpp
class GameObject {
public:
    void Update() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 1. 从外界获取输入/事件
        GatherExternalEvents();
        
        // 2. 同步状态机
        fsm_.Sync();
    }

private:
    void GatherExternalEvents() {
        // 从输入设备、网络、UI 等获取事件
        // 设置 pending_event_
    }

    Fsm fsm_;
    std::string pending_event_;
    std::mutex mutex_;
};
```

---

## 8. 设计要点总结

| 要点 | 说明 |
|------|------|
| 状态枚举类型 | 必须是 `enum class`，编译时检查 |
| 状态行为初始化 | 通过构造函数传入，构造后不可修改 |
| 公开接口 | `Sync()`、`SetState()`、`CurrentState()` 均可外部调用 |
| 线程安全 | `Sync()`/`SetState()` 内部加锁；`CurrentState()` 原子读取 |
| Resolver 必要性 | 构造函数中必须传入，不能为空 |
| 递归防护 | `enter`/`exit`/`hold` 中禁止调用 `Sync()`/`SetState()` |
| 拷贝/移动 | 禁止拷贝与移动，避免状态混乱 |
| TransitionHook | 可选钩子，用于调试监控，默认超时 10ms |
