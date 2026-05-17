#pragma once

#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <type_traits>
#include <vector>

/**
 * @brief 并发哈希映射表（Header-Only）
 * @tparam T           存储的 Value 类型
 * @tparam BucketCount 哈希桶数量，必须是 2 的幂次，编译期确定
 * @note 设计特点：
 *       - 针对整数 Key（uint64_t）优化的低开销哈希
 *       - 每个桶独立使用 std::shared_mutex，读操作可并行
 *       - 桶内使用 std::map，避免 rehash 和迭代器失效
 *       - 无迭代器暴露，批量操作返回 snapshot
 */
template <typename T, size_t BucketCount = 64>
class ConcurrentHashMap final {
    // 编译期断言：BucketCount 必须是 2 的幂次（支持快速位掩码取模）
    static_assert((BucketCount & (BucketCount - 1)) == 0,
                  "BucketCount must be a power of 2");

public:
    // Key 类型别名，方便后续统一调整为 uint64_t 或 uint32_t
    using KeyType = uint64_t;

    // 构造/析构
    ConcurrentHashMap() = default;
    ~ConcurrentHashMap() = default;

    // 禁止拷贝和移动（锁成员不可拷贝）
    ConcurrentHashMap(const ConcurrentHashMap&) = delete;
    ConcurrentHashMap& operator=(const ConcurrentHashMap&) = delete;
    ConcurrentHashMap(ConcurrentHashMap&&) = delete;
    ConcurrentHashMap& operator=(ConcurrentHashMap&&) = delete;

    // ─── 写操作 ───

    /**
     * @brief 原地构造插入
     * @tparam Args 构造参数类型
     * @param key   键
     * @param args  构造 Value 的参数
     * @return true 表示新增，false 表示已存在并原地替换
     */
    template <typename... Args>
    bool Emplace(KeyType key, Args&&... args) {
        Bucket& bucket = GetBucket(key);
        std::unique_lock<std::shared_mutex> lock(bucket.mutex);
        auto [it, inserted] = bucket.data.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(key),
            std::forward_as_tuple(std::forward<Args>(args)...));
        if (!inserted) {
            it->second = T(std::forward<Args>(args)...);  // 原地替换
        }
        return inserted;
    }

    /**
     * @brief 删除指定 Key
     * @param key 键
     * @return true 表示删除成功，false 表示不存在
     */
    bool Erase(KeyType key) {
        Bucket& bucket = GetBucket(key);
        std::unique_lock<std::shared_mutex> lock(bucket.mutex);
        return bucket.data.erase(key) > 0;
    }

    /**
     * @brief 清空全部数据
     * @note 顺序锁定所有桶，避免死锁
     */
    void Clear() {
        for (auto& bucket : buckets_) {
            std::unique_lock<std::shared_mutex> lock(bucket.mutex);
            bucket.data.clear();
        }
    }

    // ─── 读操作 ───

    /**
     * @brief 查找并返回指向原节点的指针
     * @param key 键
     * @return 找到返回指向桶内 map 节点的指针，否则返回 std::nullopt
     * @note 返回的指针，在桶被如下操作之后无效：
     *          - Erase 这个key的节点；
     *          - 重新插入同一个 key（旧节点被销毁，新节点创建）;
     */
    std::optional<std::pair<const KeyType, T>*> Find(KeyType key) {
        Bucket& bucket = GetBucket(key);
        std::shared_lock<std::shared_mutex> lock(bucket.mutex);
        auto it = bucket.data.find(key);
        if (it != bucket.data.end()) {
            return &(*it);      // * 迭代器重载运算符, 拿到实际节点 类型：std::pair<const KeyType, T>
        }
        return std::nullopt;
    }

    /**
     * @brief 判断是否包含指定 Key
     * @param key 键
     * @return true 表示存在
     */
    bool Contains(KeyType key) const {
        const Bucket& bucket = GetBucket(key);
        std::shared_lock<std::shared_mutex> lock(bucket.mutex);
        return bucket.data.find(key) != bucket.data.end();
    }

    // ─── 批量/统计 ───

    /**
     * @brief 返回所有 Key 的快照（无序）
     * @return 包含所有 Key 的 vector
     * @note 返回的是快照，不保证与当前状态一致
     */
    std::vector<KeyType> Keys() const {
        std::vector<KeyType> result;
        // 先粗略预留空间，避免多次扩容
        result.reserve(Size());

        for (const auto& bucket : buckets_) {
            std::shared_lock<std::shared_mutex> lock(bucket.mutex);
            for (const auto& pair : bucket.data) {
                result.push_back(pair.first);
            }
        }
        return result;
    }

    /**
     * @brief 总元素数量（近似值，非事务性快照）
     * @return 元素总数
     */
    size_t Size() const {
        size_t total = 0;
        for (const auto& bucket : buckets_) {
            std::shared_lock<std::shared_mutex> lock(bucket.mutex);
            total += bucket.data.size();
        }
        return total;
    }

    /**
     * @brief 是否为空
     * @return true 表示无任何元素
     */
    bool Empty() const {
        for (const auto& bucket : buckets_) {
            std::shared_lock<std::shared_mutex> lock(bucket.mutex);
            if (!bucket.data.empty()) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 返回各桶元素数量分布
     * @return 长度为 BucketCount 的 vector，每个元素对应该桶的 size
     * @note 主要用于诊断和测试哈希均匀性
     */
    std::vector<size_t> GetBucketDistribution() const {
        std::vector<size_t> result;
        result.reserve(BucketCount);
        for (const auto& bucket : buckets_) {
            std::shared_lock<std::shared_mutex> lock(bucket.mutex);
            result.push_back(bucket.data.size());
        }
        return result;
    }

private:

    /**
     * @brief 64 位整数哈希（MurmurHash 风格最终化函数）
     * @param key 64 位整数键
     * @return 哈希值
     * @note 针对连续递增整数打散效果极佳，CPU 开销极小
     */
    static size_t HashKey(uint64_t key) noexcept {
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        key *= 0xc4ceb9fe1a85ec53ULL;
        key ^= key >> 33;
        return static_cast<size_t>(key);
    }

    /**
     * @brief 计算桶索引
     * @param key 键
     * @return 桶索引 [0, BucketCount)
     * @note 使用位掩码代替取模，要求 BucketCount 为 2 的幂次
     */
    static size_t GetBucketIndex(KeyType key) noexcept {
        return HashKey(key) & (BucketCount - 1);
    }

    Bucket& GetBucket(KeyType key) {
        return buckets_[GetBucketIndex(key)];
    }

    const Bucket& GetBucket(KeyType key) const {
        return buckets_[GetBucketIndex(key)];
    }

private:
    struct Bucket {
        std::map<KeyType, T> data;
        mutable std::shared_mutex mutex;
    };
    std::array<Bucket, BucketCount> buckets_;
};
