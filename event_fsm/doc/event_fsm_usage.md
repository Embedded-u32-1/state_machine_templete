# EventFsm 使用手册

本文档详细介绍 `EventFsm` 事件驱动状态机模板的设计思想、API接口和使用方法。

---

## 1. 设计思想

`EventFsm` 是一种**事件驱动的仲裁式状态机**，其核心思想是：

| 步骤 | 行为 |
|------|------|
| 1 | 外部事件通过 [`Post()`](event_fsm/event_fsm.h#L147) 入队 |
| 2 | 调用 [`Sync()`](event_fsm/event_fsm.h#L154) 同步处理 |
| 3 | [`Sync()`](event_fsm/event_fsm.h#L154) 内部通过 **resolver** 仲裁目标状态 |
| 4 | 状态变化 → 清空事件 → 执行 `exit` + `enter` |
| 5 | 状态不变 → 按序执行合法事件的 `Action` |
| 6 | 手动强制设置状态使用 [`SetState()`](event_fsm/event_fsm.h#L176) |

---

## 2. 核心概念

### 2.1 状态和事件枚举

状态和事件枚举**必须是 `enum class`**（强类型枚举），编译时检查：

```cpp
enum class PlayerState { kStopped, kPlaying, kPaused };
enum class PlayerEvent { kPlay, kPause, kStop };
```

### 2.2 事件规则 (Event Rules)

每个 `(状态, 事件)` 组合可以配置一个 Action：

```cpp
struct Key {
    StateEnum state_;
    EventEnum event_;
    
    bool operator<(const Key& other) const {
        if (state_ != other.state_)
            return state_ < other.state_;
        return event_ < other.event_;
    }
};

using Action = std::function<void(SelfType)>;  // SelfType = EventFsm&
```

**注意**：事件 Action 仅在**状态保持**时执行，状态切换时整批事件会被清空。

### 2.3 状态生命周期 (Lifecycle)

每个状态可配置两种生命周期行为：

```cpp
struct Lifecycle {
    Action on_enter;  // 进入该状态时执行
    Action on_exit;   // 离开该状态时执行
};
```

### 2.4 仲裁函数 (Resolver)

```cpp
using Resolver = std::function<StateEnum(StateEnum, const std::vector<EventEnum>&)>;
```

**作用**：根据当前状态和待处理事件列表，决策目标状态。

**签名**：`StateEnum ResolveNext(StateEnum current, const std::vector<EventEnum>& events)`

**重要约束**：
- 禁止依赖事件的入队顺序或索引位置
- 同一批次事件被视为同一时刻的整体扰动
- 业务不应依赖事件的先后次序

```cpp
PlayerState ResolveNext(PlayerState current, const std::vector<PlayerEvent>& events) {
    if (events.empty())
        return current;

    // 仲裁逻辑：根据当前状态和事件列表决定目标状态
    // 注意：这里简化处理，只取最后一个事件作为决策依据
    // 实际应用中可以根据业务需求实现更复杂的仲裁逻辑
    PlayerEvent last_event = events.back();

    switch (current) {
        case PlayerState::kStopped:
            if (last_event == PlayerEvent::kPlay)
                return PlayerState::kPlaying;
            return PlayerState::kStopped;

        case PlayerState::kPlaying:
            if (last_event == PlayerEvent::kPause)
                return PlayerState::kPaused;
            if (last_event == PlayerEvent::kStop)
                return PlayerState::kStopped;
            return PlayerState::kPlaying;

        case PlayerState::kPaused:
            if (last_event == PlayerEvent::kPlay)
                return PlayerState::kPlaying;
            if (last_event == PlayerEvent::kStop)
                return PlayerState::kStopped;
            return PlayerState::kPaused;

        default:
            return current;
    }
}
```

### 2.5 校验函数 (Validator)

```cpp
using Validator = std::function<bool(StateEnum)>;
```

**作用**：校验目标状态值是否合法（如防止外部直接传入非法枚举值）。可选参数。

### 2.6 状态转移钩子 (TransitionHook)

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
| `SelfType` | `EventFsm&` | 状态机引用，用于回调 |
| `Action` | `std::function<void(SelfType)>` | 事件行为回调 |
| `Resolver` | `std::function<StateEnum(StateEnum, const std::vector<EventEnum>&)>` | 状态仲裁函数 |
| `Validator` | `std::function<bool(StateEnum)>` | 状态校验函数 |
| `OldState` | `StateEnum` | 状态转移钩子源状态类型 |
| `NewState` | `StateEnum` | 状态转移钩子目标状态类型 |
| `TransitionHook` | `std::function<void(OldState, NewState)>` | 状态转移钩子 |

### 3.2 构造函数

```cpp
explicit EventFsm(
    StateEnum init_state,                              // 初始状态
    std::map<Key, Action> rules,                       // 事件规则表
    std::map<StateEnum, Lifecycle> lifecycles,         // 状态生命周期表
    Resolver resolver,                                  // 仲裁函数【必填】
    Validator validator = nullptr,                      // 校验函数【可选】
    TransitionHook transition_hook = nullptr,         // 状态转移钩子【可选】
    int hook_timeout_ms = 10                           // 钩子执行超时时间（毫秒）【可选】
);
```

**注意**：
- `resolver` 不能为空，否则抛出 `std::invalid_argument`
- `rules`、`lifecycles` 在构造后不可修改
- `transition_hook` 在构造后不可修改，确保线程安全
- 钩子执行超时默认 10ms，超过会输出警告信息

### 3.3 核心方法

| 方法 | 说明 |
|------|------|
| [`void Post(EventEnum event)`](event_fsm/event_fsm.h#L147) | 投递事件（仅入队，不立即处理） |
| [`void Sync()`](event_fsm/event_fsm.h#L154) | 同步处理事件（核心入口：仲裁 + 状态切换 + 事件处理） |
| [`void SetState(StateEnum state)`](event_fsm/event_fsm.h#L176) | 强制设置状态（teleport/reset 使用） |
| [`StateEnum CurrentState() const`](event_fsm/event_fsm.h#L187) | 获取当前状态（线程安全） |

---

## 4. Sync() 执行流程

```
┌─────────────────────────────────────────────────────────────────┐
│ EventFsm 执行模型：事件驱动 + 仲裁式状态切换                     │
│                                                                 │
│  世界变化 ──[派发事件]──→ EventFsm                              │
│                                  ↓                               │
│                           pending_events_ (队列)                │
│                                  ↓                               │
│                           Sync() ← 核心入口                      │
│                                  ↓                               │
│                        resolver_() 仲裁目标状态                  │
│                                  ↓                               │
│              ┌─────────────────┴─────────────────┐              │
│         ┌────┴────┐                        ┌────┴────┐        │
│         │状态改变 │                        │状态保持 │        │
│         └────┬────┘                        └────┬────┘        │
│              ↓                                     ↓             │
│      清空整批事件                            遍历 events         │
│      不执行回调                            (state,event)        │
│              ↓                               匹配 rules_        │
│          exit(old)                                 ↓             │
│              ↓                            合法 → 执行 Action    │
│          [原子写]                           非法 → 静默丢弃       │
│              ↓                                                   │
│          enter(new)                                              │
│                                                                 │
│ 关键：状态切换时事件全部丢弃；状态保持时仅合法事件按序执行      │
└─────────────────────────────────────────────────────────────────┘
```

### 4.1 状态切换场景

```
状态转移时序（状态切换场景）：
T0: idle + 收到 start 事件
T1: Sync() 被调用
T2: resolver 返回 running  → 判定状态切换
T3: pending_events_ 被清空（start 事件丢弃）
T4: exit(idle) 执行
T5: 状态原子切换 → running
T6: enter(running) 执行
T7: 本次无事件 Action 执行
```

### 4.2 状态保持场景

```
状态转移时序（状态保持场景）：
T0: running + 收到 tick/tick/stop 事件
T1: Sync() 被调用
T2: resolver 返回 running  → 判定状态保持
T3: 取出 [tick, tick, stop]
T4: tick 合法 → Action(running, tick) 执行
T5: tick 合法 → Action(running, tick) 执行
T6: stop 在当前 running 状态下无规则 → 静默丢弃
```

---

## 5. 重复执行与业务兜底责任（核心契约声明）

EventFsm 内核对事件的执行遵循以下不可变契约：

| 内核保证 | 内核不保证 |
|---------|-----------|
| 每个入队的合法事件都会触发一次对应的 Action | 事件不会被自动去重或合并 |
| 同一批次内相同事件会多次执行对应 Action | 不识别事件是否属于"重发" |
| 状态保持时稳态执行 Action | 不干预、不限制 Action 的执行次数 |

> **关于执行顺序**：同一批次内事件按队列存储顺序遍历仅为**保证状态机行为确定性**（避免随机性导致不可复现的问题）。内核本身**不分辨事件时序**、**不相信投递顺序**，同一批次事件被视为同一时刻的整体扰动，业务不应依赖其先后次序。

这意味着：**所有「重复执行带来的差异」全部属于业务范畴**，使用者必须自行设计兜底策略：

- **要防重**（同一事件不应多次生效）→ 业务层自行实现幂等（如基于事件 ID 去重表、版本号、状态校验等）；
- **要计次 / 叠加**（多次执行需累加效果）→ 业务 Action 内部自行适配多次执行；
- **过程性、时序性、非幂等动作**（必须严格单次、顺序、不可重试）→ **改用串行单发模式**，不依赖 EventFsm 的批量稳态执行。

> 状态机内核只做三件事：**快照仲裁**、**状态收敛**、**状态变更清队列**。
> 除此之外的一切执行差异控制（去重、限次、时序依赖、补偿回滚）均不在内核职责范围内。

---

## 6. 使用示例

### 6.1 完整播放器状态机示例

```cpp
#include "event_fsm.h"
#include <iostream>
#include <map>

// ==================== 状态和事件定义 ====================
enum class PlayerState { kStopped, kPlaying, kPaused };

enum class PlayerEvent { kPlay, kPause, kStop };

// ==================== 播放器业务类 ====================
class Player {
public:
    using Fsm = EventFsm<PlayerState, PlayerEvent>;
    using Action = Fsm::Action;
    using Key = Fsm::Key;
    using Lifecycle = Fsm::Lifecycle;
    using Resolver = Fsm::Resolver;
    using Validator = Fsm::Validator;
    using TransitionHook = Fsm::TransitionHook;

    Player()
        : fsm_(PlayerState::kStopped,
               BuildRules(),
               BuildLifecycles(),
               [this](PlayerState current, const std::vector<PlayerEvent>& events) {
                   return ResolveNext(current, events);
               },
               nullptr,
               [this](PlayerState from, PlayerState to) {
                   OnTransition(from, to);
               },
               10) {}

    // 投递事件
    void PostEvent(PlayerEvent event) {
        fsm_.Post(event);
    }

    // 同步处理事件
    void Sync() {
        fsm_.Sync();
    }

    // 强制设置状态
    void SetState(PlayerState state) {
        fsm_.SetState(state);
    }

    PlayerState CurrentState() const { return fsm_.CurrentState(); }

    const char* StateToString(PlayerState s) const {
        switch (s) {
            case PlayerState::kStopped: return "Stopped";
            case PlayerState::kPlaying: return "Playing";
            case PlayerState::kPaused: return "Paused";
            default: return "Unknown";
        }
    }

private:
    // 构建事件规则表
    static std::map<Key, Action> BuildRules() {
        return {
            // Stopped 状态下的规则
            {Key{PlayerState::kStopped, PlayerEvent::kPlay},
             [](auto& fsm) { std::cout << "[Action] 开始播放\n"; }},
            {Key{PlayerState::kStopped, PlayerEvent::kStop},
             [](auto& fsm) { std::cout << "[Action] 已经停止，无需操作\n"; }},

            // Playing 状态下的规则
            {Key{PlayerState::kPlaying, PlayerEvent::kPause},
             [](auto& fsm) { std::cout << "[Action] 暂停播放\n"; }},
            {Key{PlayerState::kPlaying, PlayerEvent::kStop},
             [](auto& fsm) { std::cout << "[Action] 停止播放\n"; }},
            {Key{PlayerState::kPlaying, PlayerEvent::kPlay},
             [](auto& fsm) { std::cout << "[Action] 正在播放中\n"; }},

            // Paused 状态下的规则
            {Key{PlayerState::kPaused, PlayerEvent::kPlay},
             [](auto& fsm) { std::cout << "[Action] 继续播放\n"; }},
            {Key{PlayerState::kPaused, PlayerEvent::kStop},
             [](auto& fsm) { std::cout << "[Action] 停止播放\n"; }},
            {Key{PlayerState::kPaused, PlayerEvent::kPause},
             [](auto& fsm) { std::cout << "[Action] 已经暂停\n"; }}
        };
    }

    // 构建状态生命周期表
    static std::map<PlayerState, Lifecycle> BuildLifecycles() {
        return {
            {PlayerState::kStopped, {
                [] (auto& fsm) { std::cout << "[Enter] 进入停止状态\n"; },
                [] (auto& fsm) { std::cout << "[Exit] 离开停止状态\n"; }
            }},
            {PlayerState::kPlaying, {
                [] (auto& fsm) { std::cout << "[Enter] 进入播放状态\n"; },
                [] (auto& fsm) { std::cout << "[Exit] 离开播放状态\n"; }
            }},
            {PlayerState::kPaused, {
                [] (auto& fsm) { std::cout << "[Enter] 进入暂停状态\n"; },
                [] (auto& fsm) { std::cout << "[Exit] 离开暂停状态\n"; }
            }}
        };
    }

    // 仲裁器：根据当前状态和待处理事件列表决定目标状态
    PlayerState ResolveNext(PlayerState current, const std::vector<PlayerEvent>& events) {
        if (events.empty())
            return current;

        // 仲裁逻辑：根据当前状态和事件列表决定目标状态
        // 注意：这里简化处理，只取最后一个事件作为决策依据
        // 实际应用中可以根据业务需求实现更复杂的仲裁逻辑
        PlayerEvent last_event = events.back();

        switch (current) {
            case PlayerState::kStopped:
                if (last_event == PlayerEvent::kPlay)
                    return PlayerState::kPlaying;
                return PlayerState::kStopped;

            case PlayerState::kPlaying:
                if (last_event == PlayerEvent::kPause)
                    return PlayerState::kPaused;
                if (last_event == PlayerEvent::kStop)
                    return PlayerState::kStopped;
                return PlayerState::kPlaying;

            case PlayerState::kPaused:
                if (last_event == PlayerEvent::kPlay)
                    return PlayerState::kPlaying;
                if (last_event == PlayerEvent::kStop)
                    return PlayerState::kStopped;
                return PlayerState::kPaused;

            default:
                return current;
        }
    }

    // 状态转移钩子
    void OnTransition(PlayerState from, PlayerState to) {
        std::cout << "[Transition] " << StateToString(from) << " -> " << StateToString(to) << "\n";
    }

private:
    Fsm fsm_;
};
```

### 6.2 使用方式

```cpp
int main() {
    Player player;

    std::cout << "初始状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    // 测试1: Stopped -> Playing (Play)
    std::cout << "--- 测试1: Stopped + Play -> Playing ---\n";
    player.PostEvent(PlayerEvent::kPlay);
    player.Sync();
    std::cout << "当前状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    // 测试2: Playing -> Paused (Pause)
    std::cout << "--- 测试2: Playing + Pause -> Paused ---\n";
    player.PostEvent(PlayerEvent::kPause);
    player.Sync();
    std::cout << "当前状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    // 测试3: Paused -> Playing (Play)
    std::cout << "--- 测试3: Paused + Play -> Playing ---\n";
    player.PostEvent(PlayerEvent::kPlay);
    player.Sync();
    std::cout << "当前状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    // 测试4: Playing -> Stopped (Stop)
    std::cout << "--- 测试4: Playing + Stop -> Stopped ---\n";
    player.PostEvent(PlayerEvent::kStop);
    player.Sync();
    std::cout << "当前状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    // 测试5: 批量投递事件
    std::cout << "--- 测试5: 批量投递事件 (Play -> Pause -> Stop) ---\n";
    player.PostEvent(PlayerEvent::kPlay);
    player.PostEvent(PlayerEvent::kPause);
    player.PostEvent(PlayerEvent::kStop);
    player.Sync();
    std::cout << "当前状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    return 0;
}
```

---

## 7. 状态流转示意

```
            ┌──[Pause]──> Paused ──[Play]──┐
            │                           │
Stopped ──[Play]──> Playing ──[Stop]──> Stopped
            │                           │
            └──────────[Stop]────────────┘
```

---

## 8. 线程安全模型

| 资源 | 线程安全 | 说明 |
|------|----------|------|
| [`CurrentState()`](event_fsm/event_fsm.h#L187) | ✅ | `std::atomic` 保护，原子读取 |
| [`Sync()`](event_fsm/event_fsm.h#L154) | ✅ | 内部有 `mutex`，串行执行 |
| [`SetState()`](event_fsm/event_fsm.h#L176) | ✅ | 内部有 `mutex`，串行执行 |
| [`Post()`](event_fsm/event_fsm.h#L147) | ✅ | `ThreadLocal` 队列，线程本地存储 |
| `rules_` | ✅ | `const`，只读 |
| `lifecycles_` | ✅ | `const`，只读 |
| `resolver_` | ⚠️ | 用户需保证回调线程安全 |
| `validator_` | ⚠️ | 用户需保证回调线程安全 |
| `on_transition_` | ⚠️ | 用户需保证回调线程安全 |

### 推荐使用方式

```cpp
std::mutex mtx;
EventFsm fsm(State::kIdle, rules, lifecycles, resolver, validator);

{
    std::lock_guard<std::mutex> lock(mtx);  // 保护业务数据
    fsm.Post(Event::kStart);  // Post() 是线程安全的
    fsm.Sync();  // Sync() 内部已保证串行执行
}
```

> **注意**：[`Sync()`](event_fsm/event_fsm.h#L154) 内部已有 `mutex` 保证串行执行，外部加锁是为了保护业务层的共享数据。

### 递归检测

`EventFsm` 内部有递归检测机制：

- 若在 `on_enter`/`on_exit`/`Action` 回调中调用 [`Sync()`](event_fsm/event_fsm.h#L154) 或 [`SetState()`](event_fsm/event_fsm.h#L176)，会抛出 `std::runtime_error("Recursive state transition detected")`
- 使用 `thread_local` 实现，每个线程独立检测

### ThreadLocal 事件队列

`EventFsm` 使用 `ThreadLocal` 实现事件队列：

- 每个线程有独立的事件队列
- [`Post()`](event_fsm/event_fsm.h#L147) 操作是线程安全的，无需加锁
- [`Sync()`](event_fsm/event_fsm.h#L154) 只处理当前线程的事件队列

---

## 9. 与 BasicFsm 对比

```
┌─────────────────────┬────────────────────┬────────────────────┐
│                     │    BasicFsm        │     EventFsm       │
├─────────────────────┼────────────────────┼────────────────────┤
│ 驱动方式            │ 主动轮询 (resolver) │ 被动事件           │
│ 事件处理            │ 无                 │ 事件入队，稳态执行 │
│ 状态行为            │ enter/exit/hold    │ enter/exit         │
│ 规则机制            │ Validator          │ Key → Action       │
│ 适用场景            │ 传感器、协议       │ UI、Game AI、流程  │
│ 线程模型            │ mutex + atomic     │ mutex + atomic + ThreadLocal │
└─────────────────────┴────────────────────┴────────────────────┘
```

---

## 10. 设计要点总结

| 要点 | 说明 |
|------|------|
| 状态和事件枚举类型 | 必须是 `enum class`，编译时检查 |
| 事件规则和生命周期初始化 | 通过构造函数传入，构造后不可修改 |
| 公开接口 | `Post()`、`Sync()`、`SetState()`、`CurrentState()` 均可外部调用 |
| 线程安全 | `Sync()`/`SetState()` 内部加锁；`CurrentState()` 原子读取；`Post()` 使用 ThreadLocal |
| Resolver 必要性 | 构造函数中必须传入，不能为空 |
| 递归防护 | `on_enter`/`on_exit`/`Action` 中禁止调用 `Sync()`/`SetState()` |
| 拷贝/移动 | 禁止拷贝与移动，避免状态混乱 |
| TransitionHook | 可选钩子，用于调试监控，默认超时 10ms |
| 事件处理策略 | 状态切换时整批事件丢弃；状态保持时合法事件按序执行 |
| 重复执行责任 | 业务层自行处理去重、限次、时序依赖等问题 |