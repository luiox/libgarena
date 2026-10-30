#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "garena/arena.hpp"

using ca::u8;
using ca::usize;

namespace {

struct Trivial {
    int x;
};

static int g_dtor_order = 0;

struct NonTrivial {
    int id;
    int* dtor_order_slot;
    explicit NonTrivial(int i, int* slot = nullptr) : id(i), dtor_order_slot(slot) {}
    NonTrivial() : id(-1), dtor_order_slot(nullptr) {}
    ~NonTrivial() {
        if (dtor_order_slot) *dtor_order_slot = ++g_dtor_order;
    }
};

}  // namespace

TEST(ArenaTest, AllocBasicAndAlignment) {
    garena::Arena a;
    void* p1 = a.alloc(1, 1);
    void* p2 = a.alloc(8, 128);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(reinterpret_cast<usize>(p2) % 128, 0);
    EXPECT_TRUE(a.owns(p1));
    EXPECT_TRUE(a.owns(p2));
}

TEST(ArenaTest, AddressStableAcrossGrowth) {
    garena::Arena a(256);
    std::vector<void*> ptrs;
    for (int i = 0; i < 200; ++i) {
        ptrs.push_back(a.alloc(64, 16));
    }
    // 地址稳定契约 = 扩容不搬移旧数据：写入带槽位特征值，最后统一校验。
    // 不做跨 chunk 地址单调断言——同 chunk 内 bump 递增，但新 chunk 的
    // OS 分配地址不保证更高，单调性在 chunk 边界上无契约含义（偶发翻车）。
    for (usize i = 0; i < ptrs.size(); ++i) {
        ASSERT_TRUE(a.owns(ptrs[i]));
        *static_cast<u8*>(ptrs[i]) = static_cast<u8>(i & 0xFF);
    }
    for (usize i = 0; i < ptrs.size(); ++i) {
        ASSERT_EQ(*static_cast<u8*>(ptrs[i]), static_cast<u8>(i & 0xFF))
            << "slot " << i << " clobbered";
    }
    EXPECT_GT(a.total_bytes(), 256);  // 确认跨了多个 chunk
}

TEST(ArenaTest, OwnsRejectsForeign) {
    garena::Arena a;
    void* p = a.alloc(16, 8);
    int local = 0;
    EXPECT_FALSE(a.owns(&local));
    garena::Arena other;
    void* q = other.alloc(16, 8);
    EXPECT_FALSE(a.owns(q));
    EXPECT_TRUE(other.owns(q));
    (void)p;
}

TEST(ArenaTest, CreateRunsNonTrivialDtorsOnResetInReverseOrder) {
    g_dtor_order = 0;
    int order[3] = {0, 0, 0};
    {
        garena::Arena a;
        a.create<NonTrivial>(1, &order[0]);
        a.create<NonTrivial>(2, &order[1]);
        a.create<NonTrivial>(3, &order[2]);
        a.reset();  // 复用 chunk，不释放内存
    }
    // 逆序：id 3 先析构
    EXPECT_EQ(order[0], 3);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 1);
}

TEST(ArenaTest, DestructorRunsAtScopeExit) {
    g_dtor_order = 0;
    int order = 0;
    {
        garena::Arena a;
        a.create<NonTrivial>(1, &order);
    }
    EXPECT_EQ(order, 1);
}

TEST(ArenaTest, CreateArrayRunsAllDtors) {
    g_dtor_order = 0;
    std::vector<int> slots(10, 0);
    {
        garena::Arena a;
        a.createArray<NonTrivial>(10);  // dtor_order_slot 为空，仅验证不崩溃
        int* base = slots.data();
        for (int i = 0; i < 10; ++i) {
            a.create<NonTrivial>(i, base + i);
        }
    }
    for (int i = 0; i < 10; ++i) EXPECT_NE(slots[i], 0);
    for (int i = 1; i < 10; ++i) EXPECT_LT(slots[i], slots[i - 1]);  // 后创建先析构
}

TEST(ArenaTest, TrivialNoDestructorRegistration) {
    garena::Arena a;
    Trivial* t = a.create<Trivial>(Trivial{42});
    EXPECT_EQ(t->x, 42);
}

TEST(ArenaTest, ResetReusesChunkMemory) {
    garena::Arena a(1024);
    void* p = a.alloc(512, 8);
    a.reset();
    void* q = a.alloc(512, 8);
    EXPECT_EQ(a.total_bytes(), 1024);   // 没有新 malloc chunk
    EXPECT_EQ(p, q);                    // bump 从头开始
}

/// 缺陷#12：多 chunk 下 reset 必须从首块重新 bump（此前只从末块恢复，
/// 前面块跨周期搁浅、同等负载反复追加新 chunk，总量无界增长）。
TEST(ArenaTest, ResetRestartsBumpAtFirstChunkInMultiChunkArena) {
    garena::Arena a(256);
    void* first = a.alloc(64, 16);
    for (int i = 0; i < 200; ++i) a.alloc(64, 16);  /// 跨多 chunk（256 起翻倍）
    const usize total = a.total_bytes();
    ASSERT_GT(total, 256u);

    a.reset();
    void* q = a.alloc(64, 16);
    EXPECT_EQ(first, q);  /// bump 从首块重新开始（design.md「bump 从头开始」承诺）
    for (int i = 0; i < 200; ++i) a.alloc(64, 16);  /// 与 reset 前完全同等的负载
    EXPECT_EQ(a.total_bytes(), total);  /// 零新 chunk：跨 reset 周期内存有界
}

TEST(ArenaTest, MoveTransfersOwnership) {
    garena::Arena a(512);
    void* p = a.alloc(64, 8);
    garena::Arena b(std::move(a));
    EXPECT_TRUE(b.owns(p));
    EXPECT_FALSE(a.owns(p));  // 旧壳为空
    EXPECT_EQ(b.used_bytes(), 64);

    garena::Arena c;
    c = std::move(b);
    EXPECT_TRUE(c.owns(p));
    EXPECT_EQ(c.used_bytes(), 64);
}

TEST(ArenaTest, StringObjectLivesInArena) {
    garena::Arena a;
    std::string* s = a.create<std::string>("hello arena");
    EXPECT_EQ(*s, "hello arena");
    s->append("!");
    EXPECT_EQ(*s, "hello arena!");
}

TEST(ArenaTest, UsedBytesAccounting) {
    garena::Arena a;
    EXPECT_EQ(a.used_bytes(), 0);
    a.alloc(100, 1);
    EXPECT_EQ(a.used_bytes(), 100);
    a.alloc(28, 8);                       // 起点对齐到 104
    EXPECT_EQ(a.used_bytes(), 132);
}

// ---------------------------------------------------------------------------
// v2 回归（附录 B 盲区补齐）
// ---------------------------------------------------------------------------

namespace {

struct ThrowAtN {
    static int countdown;  ///< >0: 剩余允许成功构造数，减穿即抛
    static int live;
    ThrowAtN() {
        if (countdown > 0) --countdown;
        if (countdown <= 0) throw std::runtime_error("elem boom");
        ++live;
    }
    ~ThrowAtN() { --live; }
};
int ThrowAtN::countdown = 0;
int ThrowAtN::live = 0;

}  // namespace

/// 缺陷#2 改约：alloc(0) 返回 nullptr，不再充当哨兵。
TEST(ArenaTest, AllocZeroReturnsNullptr) {
    garena::Arena a;
    EXPECT_EQ(a.alloc(0, 1), nullptr);
    EXPECT_EQ(a.alloc(0, 64), nullptr);
    EXPECT_EQ(a.used_bytes(), 0);
    /// createArray(0) 同约
    EXPECT_EQ(a.createArray<int>(0), nullptr);
}

/// createArray 为值初始化（T()）：标量元素零初始化，非不确定值（语义 pin，与头文件注释同源）。
TEST(ArenaTest, CreateArrayValueInitializesElements) {
    garena::Arena a;
    int* arr = a.createArray<int>(8);
    ASSERT_NE(arr, nullptr);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(arr[i], 0) << "elem " << i;
    }
}

/// 单次超大分配（> 首块）：必须整块放下，不回绕、不返回空。
TEST(ArenaTest, SingleHugeAllocationBeyondFirstChunk) {
    garena::Arena a(256);
    void* p = a.alloc(200 * 1024, 64);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(a.owns(p));
    std::memset(p, 0xAB, 200 * 1024);  /// 全程可写
    EXPECT_GT(a.total_bytes(), 200 * 1024);
}

/// move 后旧壳 reset/alloc 必须安全（附录 B：move 后对旧壳 reset）。
TEST(ArenaTest, ResetOnMovedOutShellIsSafeNoOp) {
    garena::Arena a(512);
    void* p = a.alloc(64, 8);
    garena::Arena b(std::move(a));
    a.reset();  /// 旧壳为空：无 chunk、无 dtor 登记，必须是无操作
    EXPECT_EQ(a.used_bytes(), 0);
    EXPECT_EQ(a.total_bytes(), 0);
    EXPECT_EQ(b.used_bytes(), 64);  /// 真身不受影响
    EXPECT_TRUE(b.owns(p));
    /// 旧壳此后可当全新 arena 使用
    void* q = a.alloc(32, 8);
    ASSERT_NE(q, nullptr);
    EXPECT_TRUE(a.owns(q));
    EXPECT_FALSE(b.owns(q));
}

/// 缺陷#5：createArray 构造到第 k 个抛出 → 逆序析构前 k-1 个 + 回退 bump + 重抛。
TEST(ArenaTest, CreateArrayRollsBackOnThrowingElement) {
    ThrowAtN::live = 0;
    ThrowAtN::countdown = 5;  /// 前 4 个成功，第 5 个抛出
    garena::Arena a;
    const usize used_before = a.used_bytes();
    try {
        a.createArray<ThrowAtN>(10);
        FAIL() << "expected throw";
    } catch (const std::runtime_error&) {
    }
    EXPECT_EQ(ThrowAtN::live, 0);            /// 前 4 个已被回滚析构
    EXPECT_EQ(a.used_bytes(), used_before);  /// bump 指针已回退
    EXPECT_EQ(a.dtor_registrations(), 0);    /// 未登记任何条目
    /// arena 可继续正常使用
    ThrowAtN::countdown = 0;
    try {
        a.createArray<ThrowAtN>(3);
        FAIL() << "expected throw (countdown=0 means first throws)";
    } catch (const std::runtime_error&) {
    }
    EXPECT_EQ(ThrowAtN::live, 0);
    ThrowAtN::countdown = 100;
    ThrowAtN* arr = a.createArray<ThrowAtN>(4);
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(ThrowAtN::live, 4);
    EXPECT_EQ(a.dtor_registrations(), 1);  /// 数组按一条登记
}

/// 缺陷#13：create 单对象构造抛出 → 回退 bump 后重抛（与 createArray 同规，
/// 不把泄漏空间滞留到 reset）。
TEST(ArenaTest, CreateRollsBackOnThrowingCtor) {
    ThrowAtN::live = 0;
    ThrowAtN::countdown = 0;  /// 首个构造即抛
    garena::Arena a;
    a.alloc(100, 1);  /// 前置占位：验证块内回退（恢复 100 而非清零）
    const usize used_before = a.used_bytes();
    try {
        a.create<ThrowAtN>();
        FAIL() << "expected throw";
    } catch (const std::runtime_error&) {
    }
    EXPECT_EQ(a.used_bytes(), used_before);       /// bump 已回退到分配前
    EXPECT_EQ(a.dtor_registrations(), 0);         /// 未登记析构
    /// arena 可继续正常使用
    ThrowAtN::countdown = 100;
    ThrowAtN* obj = a.create<ThrowAtN>();
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(ThrowAtN::live, 1);
    EXPECT_EQ(a.dtor_registrations(), 1);
}

/// 缺陷#7：析构登记开销披露。trivial 零元数据；non-trivial 每对象/每数组一条。
TEST(ArenaTest, DtorRegistrationDiagnostics) {
    garena::Arena a;
    EXPECT_EQ(a.dtor_registrations(), 0);
    EXPECT_EQ(a.dtor_overhead_bytes(), 0);
    a.create<Trivial>(Trivial{1});
    a.createArray<Trivial>(10);
    EXPECT_EQ(a.dtor_registrations(), 0);  /// trivial T：零 per-allocation 元数据
    a.create<NonTrivial>(1);
    a.create<NonTrivial>(2);
    a.createArray<NonTrivial>(3);
    EXPECT_EQ(a.dtor_registrations(), 3);
    /// x64 上 DtorEntry = {void*, fn ptr, usize, usize} = 32B
    EXPECT_EQ(a.dtor_overhead_bytes(), 3u * sizeof(void*) * 4u);
    a.reset();
    EXPECT_EQ(a.dtor_registrations(), 0);
    EXPECT_EQ(a.dtor_overhead_bytes(), 0);
}

/// 缺陷#8：owns() 二分——大量 chunk（乱序基址可能）下归属判定仍正确。
TEST(ArenaTest, OwnsBinaryOverManyChunks) {
    garena::Arena a(256);
    std::vector<void*> ptrs;
    for (int i = 0; i < 64; ++i) {  /// 首块 256B（下限钳制）容量翻倍增长，足以跨多 chunk
        ptrs.push_back(a.alloc(64, 16));
    }
    for (void* p : ptrs) EXPECT_TRUE(a.owns(p));
    /// 反复查询（命中缓存 + 二分路径混合）
    for (int round = 0; round < 2; ++round) {
        EXPECT_TRUE(a.owns(ptrs.front()));
        EXPECT_TRUE(a.owns(ptrs[ptrs.size() / 2]));
        EXPECT_TRUE(a.owns(ptrs.back()));
    }
    int local = 0;
    EXPECT_FALSE(a.owns(&local));
}

/// 缺陷#1/#6/#14：分配尺寸溢出一律 abort（契约 6，全构建生效）。
TEST(ArenaTest, SizeOverflowAborts) {
#ifdef _WIN32
    const auto kAborted = ::testing::ExitedWithCode(3);
#else
    const auto kAborted = ::testing::KilledBySignal(6);
#endif
    ASSERT_EXIT(
        {
            garena::Arena a;
            a.alloc(garena::kMaxAlloc + 1, 1);
        },
        kAborted, "garena: allocation size overflow");
    ASSERT_EXIT(
        {
            garena::Arena a;
            a.createArray<int>(garena::kMaxAlloc / sizeof(int) + 1);
        },
        kAborted, "garena: allocation size overflow");
    /// 缺陷#14：bytes/align 各自 ≤ kMaxAlloc、但和近 SIZE_MAX——旧实现
    /// alignUp 回绕出 want=0，以 internal error / OOM(0 bytes) 误报
    ASSERT_EXIT(
        {
            garena::Arena a;
            a.alloc(garena::kMaxAlloc, garena::kMaxAlloc);
        },
        kAborted, "garena: allocation size overflow");
    /// 缺陷#14 收口：空请求不放行非法对齐（align 溢出即使 bytes==0 也 abort）
    ASSERT_EXIT(
        {
            garena::Arena a;
            a.alloc(0, garena::kMaxAlloc + 1);
        },
        kAborted, "garena: allocation size overflow");
}

/// 缺陷#4：debug 下 reset() 后 chunk 空闲区被 0xDD 毒化。
TEST(ArenaTest, ResetPoisonsChunkInDebug) {
#ifndef NDEBUG
    garena::Arena a(1024);
    void* p = a.alloc(64, 8);
    a.reset();
    const u8* bytes = static_cast<const u8*>(p);
    for (int i = 0; i < 64; ++i) {
        ASSERT_EQ(bytes[i], 0xDD) << "byte " << i;
    }
#else
    GTEST_SUCCEED() << "release build: poison disabled";
#endif
}
