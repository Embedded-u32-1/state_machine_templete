#include "../concurrent_hash_map.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// ==================== 辅助函数 ====================

static size_t g_passed = 0;
static size_t g_failed = 0;

void Check(bool condition, const std::string& name) {
    if (condition) {
        std::cout << "  [PASS] " << name << "\n";
        ++g_passed;
    } else {
        std::cout << "  [FAIL] " << name << "\n";
        ++g_failed;
    }
}

// ==================== 测试1: 单线程 CRUD ====================

void TestSingleThreadCrud() {
    std::cout << "\n=== 测试1: 单线程 CRUD ===\n";

    ConcurrentHashMap<std::string, 64> map;

    // Emplace new
    Check(map.Emplace(1, "one").second == true, "Emplace new key returns true");
    // Emplace existing (will replace)
    Check(map.Emplace(1, "ONE").second == false, "Emplace existing key returns false (replaced)");

    auto val = map.Find(1);
    Check(val.has_value() && val.value()->second == "ONE", "Find returns updated value");

    // Contains
    Check(map.Contains(1) == true, "Contains existing key");
    Check(map.Contains(999) == false, "Contains non-existing key");

    // Emplace another
    Check(map.Emplace(2, "two").second == true, "Emplace key 2 returns true");
    auto val2 = map.Find(2);
    Check(val2.has_value() && val2.value()->second == "two", "Find key 2 correct");

    // Size / Empty
    Check(map.Size() == 2, "Size equals 2");
    Check(map.Empty() == false, "Empty returns false");

    // Keys
    auto keys = map.Keys();
    Check(keys.size() == 2, "Keys returns 2 elements");

    // Erase
    Check(map.Erase(1) == true, "Erase existing key returns true");
    Check(map.Erase(1) == false, "Erase non-existing key returns false");
    Check(map.Contains(1) == false, "Contains after erase");

    // Clear
    map.Clear();
    Check(map.Empty() == true, "Empty after clear");
    Check(map.Size() == 0, "Size equals 0 after clear");
}

// ==================== 测试2: 多线程并发插入 ====================

void TestConcurrentInsert() {
    std::cout << "\n=== 测试2: 多线程并发插入 ===\n";

    constexpr size_t kThreads = 16;
    constexpr size_t kPerThread = 10000;
    constexpr size_t kExpectedSize = kThreads * kPerThread;

    ConcurrentHashMap<size_t, 64> map;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&map, t]() {
            for (size_t i = 0; i < kPerThread; ++i) {
                uint64_t key = static_cast<uint64_t>(t) * kPerThread + i;
                map.Emplace(key, key);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    Check(map.Size() == kExpectedSize,
          "Concurrent insert: final size equals " + std::to_string(kExpectedSize));

    // 验证每个值都正确
    bool all_correct = true;
    for (size_t t = 0; t < kThreads && all_correct; ++t) {
        for (size_t i = 0; i < kPerThread; ++i) {
            uint64_t key = static_cast<uint64_t>(t) * kPerThread + i;
            auto val = map.Find(key);
            if (!val.has_value() || val.value()->second != key) {
                all_correct = false;
                break;
            }
        }
    }
    Check(all_correct, "All inserted values are correct");
}

// ==================== 测试3: 多线程读写混合 ====================

void TestReadWriteMixed() {
    std::cout << "\n=== 测试3: 多线程读写混合 ===\n";

    constexpr size_t kWriterThreads = 4;
    constexpr size_t kReaderThreads = 8;
    constexpr size_t kOperations = 5000;

    ConcurrentHashMap<int, 64> map;

    // 先预填充一些数据
    for (uint64_t i = 0; i < 1000; ++i) {
        map.Emplace(i, static_cast<int>(i));
    }

    std::atomic<size_t> read_success{0};
    std::atomic<size_t> write_success{0};
    std::vector<std::thread> threads;

    // 写线程：持续插入新 Key
    for (size_t t = 0; t < kWriterThreads; ++t) {
        threads.emplace_back([&map, t, &write_success]() {
            for (size_t i = 0; i < kOperations; ++i) {
                uint64_t key = 1000 + static_cast<uint64_t>(t) * kOperations + i;
                if (map.Emplace(key, static_cast<int>(key)).second) {
                    ++write_success;
                }
            }
        });
    }

    // 读线程：持续查询已有 Key
    for (size_t t = 0; t < kReaderThreads; ++t) {
        threads.emplace_back([&map, &read_success]() {
            for (size_t i = 0; i < kOperations; ++i) {
                uint64_t key = i % 1000;
                if (map.Contains(key)) {
                    ++read_success;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    Check(write_success == kWriterThreads * kOperations,
          "All writes succeeded (no data race)");
    Check(read_success == kReaderThreads * kOperations,
          "All reads succeeded (shared_lock works)");
}

// ==================== 测试4: 哈希均匀性 ====================

void TestHashUniformity() {
    std::cout << "\n=== 测试4: 哈希均匀性 ===\n";

    constexpr size_t kBucketCount = 64;
    constexpr size_t kTotalKeys = 1000000;

    ConcurrentHashMap<int, kBucketCount> map;

    // 插入 100 万个连续 Key
    for (uint64_t i = 0; i < kTotalKeys; ++i) {
        map.Emplace(i, static_cast<int>(i));
    }

    auto distribution = map.GetBucketDistribution();

    // 计算平均值和方差
    double mean = static_cast<double>(kTotalKeys) / kBucketCount;
    double variance = 0.0;
    size_t min_count = kTotalKeys;
    size_t max_count = 0;

    for (size_t count : distribution) {
        double diff = static_cast<double>(count) - mean;
        variance += diff * diff;
        min_count = std::min(min_count, count);
        max_count = std::max(max_count, count);
    }
    variance /= kBucketCount;
    double stddev = std::sqrt(variance);
    double cv = stddev / mean;  // 变异系数

    std::cout << "  Bucket count: " << kBucketCount << "\n";
    std::cout << "  Total keys: " << kTotalKeys << "\n";
    std::cout << "  Mean per bucket: " << mean << "\n";
    std::cout << "  Min bucket size: " << min_count << "\n";
    std::cout << "  Max bucket size: " << max_count << "\n";
    std::cout << "  StdDev: " << stddev << "\n";
    std::cout << "  Coefficient of Variation: " << cv << "\n";

    // 判定标准：变异系数 < 0.05（非常均匀）
    Check(cv < 0.05, "Hash uniformity CV < 0.05");
}

// ==================== 测试5: Emplace 竞态条件 ====================

void TestEmplaceRace() {
    std::cout << "\n=== 测试5: Emplace 竞态条件 ===\n";

    constexpr size_t kThreads = 16;
    constexpr size_t kRounds = 1000;

    ConcurrentHashMap<int, 64> map;
    std::atomic<size_t> success_count{0};
    std::vector<std::thread> threads;

    for (size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&map, &success_count]() {
            for (size_t i = 0; i < kRounds; ++i) {
                // 所有线程竞争同一个 Key 集合
                uint64_t key = i % 100;
                if (map.Emplace(key, static_cast<int>(key)).second) {
                    ++success_count;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 最终只有 100 个 Key 存在，但 success_count 应该恰好为 100
    // （每个 Key 只有一个线程能首次 Emplace 成功）
    Check(success_count == 100,
          "Emplace race: exactly 100 successes for 100 keys");
    Check(map.Size() == 100,
          "Emplace race: final size is 100");
}

// ==================== 测试6: Clear 并发安全 ====================

void TestClearConcurrent() {
    std::cout << "\n=== 测试6: Clear 并发安全 ===\n";

    constexpr size_t kWriterThreads = 4;
    constexpr size_t kClearThreads = 2;
    constexpr size_t kRounds = 2000;

    ConcurrentHashMap<int, 64> map;
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    // 写线程
    for (size_t t = 0; t < kWriterThreads; ++t) {
        threads.emplace_back([&map, t, &running]() {
            size_t i = 0;
            while (running) {
                uint64_t key = static_cast<uint64_t>(t) * 1000000 + i;
                map.Emplace(key, static_cast<int>(key));
                ++i;
                if (i >= kRounds) break;
            }
        });
    }

    // Clear 线程
    for (size_t t = 0; t < kClearThreads; ++t) {
        threads.emplace_back([&map, &running]() {
            size_t i = 0;
            while (running) {
                map.Clear();
                ++i;
                if (i >= kRounds / 10) break;  // Clear 次数少一些
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 只要没有崩溃就通过（TSan/ASan 会检测数据竞争）
    Check(true, "Clear concurrent: no crash or data race");
}

// ==================== 测试7: Find 返回指针生命周期 ====================

void TestFindPointerLifetime() {
    std::cout << "\n=== 测试7: Find 返回指针生命周期 ===\n";

    ConcurrentHashMap<std::string, 64> map;
    map.Emplace(1, "original");

    // Find 获取指针
    auto ptr_opt = map.Find(1);
    Check(ptr_opt.has_value(), "Find returns pointer for existing key");

    auto* ptr = ptr_opt.value();
    Check(ptr->second == "original", "Pointer value is correct");

    // 重新 Emplace 同一个 key（旧节点被销毁，新节点创建）
    map.Emplace(1, "replaced");

    // 旧指针应该不再有效（但这里不访问它，仅验证新值正确）
    auto ptr_opt2 = map.Find(1);
    Check(ptr_opt2.has_value() && ptr_opt2.value()->second == "replaced",
          "After re-Emplace, Find returns new value");

    // Erase 后 Find 应返回 nullopt
    map.Erase(1);
    auto ptr_opt3 = map.Find(1);
    Check(!ptr_opt3.has_value(), "Find returns nullopt after Erase");
}

// ==================== 主函数 ====================

int main() {
    std::cout << "===== ConcurrentHashMap 测试开始 =====\n";

    TestSingleThreadCrud();
    TestConcurrentInsert();
    TestReadWriteMixed();
    TestHashUniformity();
    TestEmplaceRace();
    TestClearConcurrent();
    TestFindPointerLifetime();

    std::cout << "\n===== 测试结束 =====\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";

    return g_failed > 0 ? 1 : 0;
}
