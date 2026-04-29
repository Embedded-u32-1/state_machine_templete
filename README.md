# 状态机模板库

Head-Only 状态机模板，提供两种状态机实现：

## 1. 刷新型状态机 ([basic_fsm/basic_fsm.h](basic_fsm/basic_fsm.h))

**核心特性**：
- **仲裁式同步**：通过 [`resolver`](basic_fsm/basic_fsm.h:55) 函数决定下一个目标状态，与外部世界状态对齐
- **状态行为**：每个状态可注册 enter / exit / hold 三种回调
- **线程安全**：mutex + atomic，递归检测

**适用场景**：简单状态机、多层状态机

## 2. 事件型状态机 ([event_fsm/event_fsm.h](event_fsm/event_fsm.h))

> ⚠️ **代码未完善，仅作简单介绍**

通过事件触发状态转移，按键值 `(state, event)` 查找规则。

**适用场景**：中等复杂场景，性价比高

## 使用示例

见各个 FSM 文件内的注释示例：
- [basic_fsm 使用示例](basic_fsm/basic_fsm_usage.md)
