# ConcurrentHashMap 使用手册

本文档详细介绍 `ConcurrentHashMap` 并发哈希映射表模板的设计思想、API 接口和使用方法。

---

## 1. 设计思想

`ConcurrentHashMap` 是一种**基于分片锁的并发关联容器**，其核心思想是：

| 步骤 | 行为 |
|------|------|
| 1 | 将 Key 通过哈希函数映射到固定数量的桶（Bucket） |
| 2 | 每个桶独立持有一把 `std::shared_mutex` |
| 3 | 读操作加 `shared_lock`，多个读线程可并行访问同一桶 |
| 4 | 写操作加 `unique_lock`，独占访问目标桶 |
| 5 | 桶的数量在编译期确定，使用 `std::array` 存储 |

**核心优势**：通过哈希打散将并发竞争分散到多个独立桶，将全局大锁拆分为细粒度锁，在多读场景下实现高并发。

---

## 2. 核心概念

### 2.1 模板参数

```cpp
template <typename T, size_t BucketCount = 64>
class ConcurrentHashMap;
```

| 参数 | 说明 | 约束 |
|------|------|------|
| `T` | 存储的 Value 类型 | 任意可复制、可移动类型 |
| `BucketCount` | 桶数量，编译期确定 | **必须是 2 的幂次**（如 16, 32, 64, 128） |

**为什么必须是 2 的幂次？**

内部使用位掩码 `hash & (BucketCount - 1)` 代替 `%` 取模运算，CPU 周期更短。

### 2.2 Key 类型

```cpp
using KeyType = uint64_t;
```

当前实现固定使用 64 位无符号整数作为 Key。如需调整，可修改此别名：

```cpp
using KeyType = uint32_t;  // 改为 32 位
```

> ⚠️ **注意**：哈希算法 `HashKey()` 内部固定使用 `uint64_t` 参数进行位混合，即使 `KeyType` 改为 32 位，也会先提升为 64 位再计算，保证散列质量。

### 2.3 哈希算法

针对整数 Key 优化的低开销哈希（MurmurHash 风格最终化函数）：

```cpp
static size_t HashKey(uint64_t key) noexcept {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return static_cast<size_t>(key);
}
```

**特点**：
- 纯位运算 + 乘常量，CPU 开销极小
- 对连续递增的整数 ID（如线程 ID、序列号）打散极均匀
- 当 `BucketCount` 为 2 的幂次时，基本均匀铺满所有桶

---

## 3. API 接口

### 3.1 构造函数

```cpp
// 默认构造
ConcurrentHashMap<std::string> map;

// 指定桶数量
ConcurrentHashMap<std::string, 128> map128;
```

> ⚠️ 禁止拷贝和移动（锁成员不可拷贝）

### 3.2 写操作

#### Emplace — 原地构造插入或替换

```cpp
ConcurrentHashMap<std::string> map;

// 插入新值，返回 true
bool inserted = map.Emplace(1001, "Alice");

// 再次 Emplace 相同 Key，执行原地替换，返回 false
bool inserted2 = map.Emplace(1001, "Bob");  // false
```

支持移动语义和多重参数原地构造：

```cpp
std::string value = "LargeString";
map.Emplace(1002, std::move(value));

// 直接在桶内构造复杂对象
map.Emplace(1003, "Constructed", 42, 3.14);
```

#### Erase — 删除

```cpp
bool erased = map.Erase(1001);  // true（删除成功）
bool erased2 = map.Erase(9999); // false（不存在）
```

#### Clear — 清空全部

```cpp
map.Clear();  // 遍历所有桶，逐个加写锁清空
```

### 3.3 读操作

#### Find — 查找并返回指向原节点的指针

```cpp
auto result = map.Find(1001);
if (result.has_value()) {
    auto* ptr = result.value();
    std::cout << "Key: " << ptr->first << ", Value: " << ptr->second << "\n";
} else {
    std::cout << "Key not found\n";
}
```

> ⚠️ **指针生命周期警告**：返回的指针指向桶内部 `std::map` 的节点，在以下操作后**失效**：
> - 对该 Key 调用 `Erase()`
> - 对该 Key 重新调用 `Emplace()`（旧节点被销毁，新节点创建）
>
> 因此，**严禁在持有指针期间对同一 Key 执行写操作**。

#### Contains — 判断是否包含

```cpp
if (map.Contains(1001)) {
    // Key 存在
}
```

### 3.4 批量/统计操作

#### Keys — 返回所有 Key 的快照

```cpp
std::vector<uint64_t> all_keys = map.Keys();
```

> ⚠️ 返回的是快照，不保证与当前状态一致。遍历过程中其他线程可能修改数据。

#### Size — 总元素数量（近似值）

```cpp
size_t count = map.Size();
```

> ⚠️ 多线程下为近似值，不保证事务一致性。统计过程中数据可能变化。

#### Empty — 是否为空

```cpp
if (map.Empty()) {
    // 无任何元素
}
```

#### GetBucketDistribution — 各桶元素数量分布

```cpp
std::vector<size_t> distribution = map.GetBucketDistribution();
```

> 返回长度为 `BucketCount` 的 vector，每个元素对应该桶的 size。主要用于诊断和测试哈希均匀性。

---

## 4. 线程安全说明

| 操作 | 线程安全 | 锁类型 |
|------|----------|--------|
| `Emplace` | ✅ 安全 | `unique_lock`（写锁） |
| `Erase` | ✅ 安全 | `unique_lock`（写锁） |
| `Clear` | ✅ 安全 | `unique_lock`（全桶） |
| `Find` / `Contains` | ✅ 安全 | `shared_lock`（读锁） |
| `Keys` / `Size` / `Empty` / `GetBucketDistribution` | ✅ 安全 | `shared_lock`（逐桶） |

**并发特性**：
- 不同桶的读写操作完全并行，互不影响
- 同一桶的多个读操作可并行（`shared_lock` 共享）
- 同一桶的写操作与其他读写互斥（`unique_lock` 独占）

---

## 5. 性能建议

### 5.1 BucketCount 选择

| 数据量 | 建议 BucketCount | 理由 |
|--------|------------------|------|
| < 10 万 | 64（默认） | 内存占用低，竞争已足够分散 |
| 10 万 ~ 100 万 | 128 或 256 | 进一步降低单个桶的长度 |
| > 100 万 | 256 或 512 | 减少桶内 `std::map` 的查找深度 |

### 5.2 避免热点 Key

如果某些 Key 被极高频率访问，会导致对应桶的锁竞争激烈：

```cpp
// 热点 Key：所有线程都竞争同一个桶
map.Contains(0);   // 极高频
map.Find(1);       // 极高频
```

**优化方向**：
- 在 Value 层进一步拆分（如每个 Value 内部再分片）
- 对热点数据使用独立的 `std::atomic` 或无锁结构

### 5.3 Find 返回值的使用模式

由于 `Find` 返回的是指向桶内节点的指针，在多线程环境下应**尽快使用、尽快释放**：

```cpp
// 推荐：立即解引用并使用，不长期持有指针
if (auto opt = map.Find(key); opt.has_value()) {
    doSomething(opt.value()->second);
}
// 此处已离开 shared_lock 作用域，指针不再安全
```

**严禁**：将 `Find` 返回的指针存储到外部变量，然后在后续代码中长期使用，期间可能触发其他线程对该 Key 的 `Erase` 或 `Emplace`。

---

## 6. 注意事项

### 6.1 已知限制

| 限制 | 说明 |
|------|------|
| `Size()` 非精确 | 多线程下为近似值，不保证事务一致性 |
| `Keys()` 为 Snapshot | 返回时可能已有其他线程修改数据 |
| `Find()` 返回裸指针 | 生命周期受桶写操作影响，需谨慎使用 |
| 无迭代器接口 | 并发下迭代器难以安全暴露，故不提供 |
| 无范围查询 | 当前版本未暴露 `std::map` 的有序性 |
| 无 TryInsert | 当前版本不提供"仅当不存在时插入"语义 |

### 6.2 编译要求

- **C++17 或更高**（使用 `std::shared_mutex`、`std::optional`）
- 无需额外依赖，标准库即可

### 6.3 与现有项目集成

本组件为**独立模块**，不依赖 `event_fsm.h` 或 `thread_local.h`，可直接在项目中包含使用：

```cpp
#include "event_fsm/concurrent_hash_map.h"

ConcurrentHashMap<PlayerData, 64> player_map;
```

---

## 7. 完整示例

```cpp
#include "concurrent_hash_map.h"
#include <iostream>
#include <thread>
#include <vector>

struct UserInfo {
    std::string name;
    int age;
};

int main() {
    ConcurrentHashMap<UserInfo, 64> users;

    // 单线程写入
    users.Emplace(1001, UserInfo{"Alice", 25});
    users.Emplace(1002, UserInfo{"Bob", 30});

    // 读取（立即使用指针，不长期持有）
    if (auto opt = users.Find(1001); opt.has_value()) {
        std::cout << opt.value()->second.name << " is "
                  << opt.value()->second.age << " years old\n";
    }

    // 多线程并发写入
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&users, t]() {
            for (int i = 0; i < 1000; ++i) {
                uint64_t key = static_cast<uint64_t>(t) * 1000 + i;
                users.Emplace(key, UserInfo{"User" + std::to_string(key), 20});
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "Total users: " << users.Size() << "\n";
    return 0;
}
```

---

## 8. 后续可扩展方向

| 方向 | 说明 |
|------|------|
| 迭代器支持 | 提供 `Snapshot()` 返回全量拷贝后的只读视图 |
| 范围查询 | 利用 `std::map` 的有序性，提供 `[start, end]` 区间查询 |
| 自定义哈希 | 模板参数传入哈希函数对象，支持非默认哈希 |
| 动态扩缩容 | 运行期根据负载调整 BucketCount（复杂度较高） |
| TryInsert 语义 | 提供"仅当 Key 不存在时才插入"的原子操作 |
