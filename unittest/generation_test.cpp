#include <gtest/gtest.h>

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "garena/generation.hpp"

using garena::GHandle;
using garena::GenerationalArena;
using garena::kInvalidSlot;
using garena::kNoHandle;

namespace {

// MSVC abort() 以退出码 3 终止；POSIX 下是 SIGABRT。
#ifdef _WIN32
const auto kAborted = ::testing::ExitedWithCode(3);
#else
const auto kAborted = ::testing::KilledBySignal(6);
#endif

struct Item {
    int v;
    explicit Item(int x) : v(x) {}
};

/// 构造计数抛出体：第 throw_at 个构造（1-based）抛出；<=0 永不抛。
struct ThrowingCtor {
    static int throw_at;
    static int seen;  ///< 构造尝试计数（含抛出的那次）
    static int live;  ///< 存活对象数
    int v;
    explicit ThrowingCtor(int x) : v(x) {
        ++seen;
        if (throw_at > 0 && seen == throw_at) throw std::runtime_error("ctor boom");
        ++live;
    }
    ~ThrowingCtor() { --live; }
};
int ThrowingCtor::throw_at = 0;
int ThrowingCtor::seen = 0;
int ThrowingCtor::live = 0;

void resetThrowing(int throw_at) {
    ThrowingCtor::throw_at = throw_at;
    ThrowingCtor::seen = 0;
    ThrowingCtor::live = 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// GHandle 布局与 kNoHandle
// ---------------------------------------------------------------------------

TEST(GHandleTest, LayoutIsFrozen) {
    static_assert(sizeof(GHandle) == 12, "frozen: 12 bytes {u32 owner, slot, gen}");
    static_assert(std::is_trivially_copyable_v<GHandle>, "frozen: trivially copyable");
    EXPECT_EQ(kNoHandle.owner, 0u);
    EXPECT_EQ(kNoHandle.slot, kInvalidSlot);
    EXPECT_EQ(kNoHandle.gen, 0u);
    EXPECT_FALSE(static_cast<bool>(kNoHandle));
}

TEST(GHandleTest, EqualityComparesAllFields) {
    GHandle a{1, 2, 3};
    GHandle b{1, 2, 3};
    GHandle c{1, 2, 4};
    GHandle d{9, 2, 3};
    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a != c);
    EXPECT_TRUE(a != d);
}

// ---------------------------------------------------------------------------
// 基本语义
// ---------------------------------------------------------------------------

TEST(GenerationTest, CreateGetRoundTrip) {
    GenerationalArena<Item> pool;
    GHandle h = pool.create(42);
    EXPECT_TRUE(static_cast<bool>(h));
    EXPECT_EQ(pool.get(h).v, 42);
    pool.get(h).v = 7;
    EXPECT_EQ(pool.try_get(h)->v, 7);
    EXPECT_EQ(pool.size(), 1u);
    EXPECT_EQ(pool.capacity(), GenerationalArena<Item>::kDefaultBlockSlots);
}

TEST(GenerationTest, KNoHandleIsRejectedEverywhere) {
    GenerationalArena<Item> pool;
    EXPECT_FALSE(pool.alive(kNoHandle));
    EXPECT_EQ(pool.try_get(kNoHandle), nullptr);
    EXPECT_FALSE(pool.remove(kNoHandle));
}

TEST(GenerationTest, OwnerIdsAreUniqueAndNonZero) {
    GenerationalArena<Item> a;
    GenerationalArena<Item> b;
    EXPECT_NE(a.owner_id(), 0u);
    EXPECT_NE(b.owner_id(), 0u);
    EXPECT_NE(a.owner_id(), b.owner_id());
}

// ---------------------------------------------------------------------------
// remove：O(1)、gen 递增、防 ABA
// ---------------------------------------------------------------------------

TEST(GenerationTest, RemoveInvalidatesAndReturnsFalseOnRepeat) {
    GenerationalArena<Item> pool;
    GHandle h = pool.create(1);
    EXPECT_TRUE(pool.remove(h));
    EXPECT_EQ(pool.try_get(h), nullptr);
    EXPECT_FALSE(pool.alive(h));
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_FALSE(pool.remove(h));  ///< 已死句柄：预期失败路径，返回 false
}

TEST(GenerationTest, SlotReuseBumpsGenerationABA) {
    GenerationalArena<Item> pool;
    GHandle h1 = pool.create(1);
    EXPECT_TRUE(pool.remove(h1));
    GHandle h2 = pool.create(2);
    EXPECT_EQ(h2.slot, h1.slot);    ///< 复用同槽
    EXPECT_EQ(h2.gen, h1.gen + 1);  ///< gen+1：旧句柄必失配，无 ABA
    EXPECT_NE(h1, h2);
    EXPECT_EQ(pool.try_get(h1), nullptr);  ///< 旧句柄拿不到新对象
    EXPECT_EQ(pool.get(h2).v, 2);
}

TEST(GenerationTest, GetAfterRemoveAborts) {
    ///< 冻结规格：get 失配 abort，release 也启用 —— debug/release 双构建实跑。
    GenerationalArena<Item> pool;
    GHandle h = pool.create(1);
    pool.remove(h);
    ASSERT_EXIT({ pool.get(h); }, kAborted, "garena: handle rejected.*stale");
}

// ---------------------------------------------------------------------------
// clear：全失效、capacity 不变、gen 全部 +1
// ---------------------------------------------------------------------------

TEST(GenerationTest, ClearKillsAllHandlesAndKeepsCapacity) {
    GenerationalArena<Item> pool(8);
    std::vector<GHandle> hs;
    for (int i = 0; i < 5; ++i) hs.push_back(pool.create(i));
    pool.remove(hs[2]);
    const auto cap_before = pool.capacity();
    const auto gen0 = hs[0].gen;
    pool.clear();
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_EQ(pool.capacity(), cap_before);  ///< 存储保留
    for (GHandle h : hs) {
        EXPECT_EQ(pool.try_get(h), nullptr);  ///< 全部旧句柄必然失配
    }
    ///< clear 后新建：槽序从头，gen 已被 clear +1
    GHandle n = pool.create(9);
    EXPECT_EQ(n.slot, 0u);
    EXPECT_EQ(n.gen, gen0 + 1);
}

// ---------------------------------------------------------------------------
// 地址稳定（跨块）
// ---------------------------------------------------------------------------

TEST(GenerationTest, AddressStableAcrossBlocksAndRemovals) {
    GenerationalArena<Item> pool(4);  ///< 每块仅 4 槽，强制多块
    std::vector<Item*> ptrs;
    std::vector<GHandle> hs;
    for (int i = 0; i < 12; ++i) {  ///< 跨 3 块
        hs.push_back(pool.create(i));
        ptrs.push_back(pool.try_get(hs.back()));
    }
    for (int i = 0; i < 12; ++i) {
        ASSERT_NE(ptrs[i], nullptr);
        EXPECT_EQ(ptrs[i]->v, i);                 ///< 值未被搬动
        EXPECT_EQ(pool.try_get(hs[i]), ptrs[i]);  ///< 句柄→指针映射恒定
    }
    ///< 中间删除不影响任何既有 T*（含同块与跨块）
    pool.remove(hs[5]);
    EXPECT_EQ(pool.try_get(hs[5]), nullptr);
    for (int i = 0; i < 12; ++i) {
        if (i == 5) continue;
        EXPECT_EQ(pool.try_get(hs[i]), ptrs[i]) << "slot " << i;
        EXPECT_EQ(ptrs[i]->v, i);
    }
    ///< 复用被删槽：新对象地址 == 旧对象地址（同槽原地复用），其余不动
    GHandle reused = pool.create(100);
    EXPECT_EQ(reused.slot, hs[5].slot);
    EXPECT_EQ(pool.try_get(reused), ptrs[5]);
    EXPECT_EQ(ptrs[5]->v, 100);
}

// ---------------------------------------------------------------------------
// 跨实例 owner 结构性拒绝
// ---------------------------------------------------------------------------

TEST(GenerationTest, ForeignHandleRejectedByTryGet) {
    GenerationalArena<Item> a;
    GenerationalArena<Item> b;
    GHandle h = a.create(1);
#ifdef NDEBUG
    EXPECT_EQ(b.try_get(h), nullptr);  ///< release：返回 nullptr
#else
    ///< debug：结构性拒绝 —— 使用跨实例句柄直接 abort（冻结规格 §2.3.2-1）
    ASSERT_EXIT({ (void)b.try_get(h); }, kAborted, "garena: handle rejected.*owner mismatch");
#endif
}

TEST(GenerationTest, ForeignHandleGetAborts) {
    GenerationalArena<Item> a;
    GenerationalArena<Item> b;
    GHandle h = a.create(1);
    ASSERT_EXIT({ b.get(h); }, kAborted, "garena: handle rejected.*owner mismatch");
}

// ---------------------------------------------------------------------------
// move 语义
// ---------------------------------------------------------------------------

TEST(GenerationTest, MoveTransfersOwnershipKeepsOwner) {
    GenerationalArena<Item> a(4);
    const ca::u32 owner = a.owner_id();
    GHandle h = a.create(5);
    Item* p = a.try_get(h);
    GenerationalArena<Item> b(std::move(a));
    EXPECT_EQ(b.owner_id(), owner);  ///< 契约 7：owner 不变
    EXPECT_EQ(b.get(h).v, 5);
    EXPECT_EQ(b.try_get(h), p);     ///< 句柄与指针全部继续有效
    EXPECT_EQ(a.owner_id(), 0u);    ///< 被移动壳归零：旧壳句柄必失配
    EXPECT_EQ(a.size(), 0u);
    EXPECT_EQ(a.capacity(), 0u);

    GenerationalArena<Item> c;
    c = std::move(b);
    EXPECT_EQ(c.owner_id(), owner);
    EXPECT_EQ(c.get(h).v, 5);
}

// ---------------------------------------------------------------------------
// create 异常安全
// ---------------------------------------------------------------------------

TEST(GenerationTest, CreateExceptionDoesNotPublishHalfConstructed) {
    GenerationalArena<ThrowingCtor> pool(4);
    resetThrowing(2);  ///< 第 1 个成功，第 2 个抛出
    GHandle ok = pool.create(1);
    EXPECT_EQ(ThrowingCtor::live, 1);
    const auto cap = pool.capacity();
    try {
        pool.create(2);
        FAIL() << "expected throw";
    } catch (const std::runtime_error&) {
    }
    ///< 槽不发布：size 不变、live 不泄漏、半构造对象不出现
    EXPECT_EQ(pool.size(), 1u);
    EXPECT_EQ(pool.capacity(), cap);
    EXPECT_EQ(ThrowingCtor::live, 1);
    EXPECT_EQ(pool.try_get(ok)->v, 1);
    ///< 异常槽（gen 已 +1）可复用，池可继续使用
    resetThrowing(0);
    GHandle again = pool.create(3);
    EXPECT_EQ(again.slot, 1u);     ///< 复用抛出时的那个槽
    EXPECT_EQ(again.gen, 1u);      ///< 失败使槽 gen+1
    EXPECT_EQ(pool.size(), 2u);
    EXPECT_EQ(pool.get(again).v, 3);
}

TEST(GenerationTest, CreateExceptionOnFreshSlotKeepsPoolUsable) {
    GenerationalArena<ThrowingCtor> pool(4);
    resetThrowing(1);  ///< 第 1 个就抛
    try {
        pool.create(1);
        FAIL() << "expected throw";
    } catch (const std::runtime_error&) {
    }
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_EQ(ThrowingCtor::live, 0);
    resetThrowing(0);
    GHandle h = pool.create(7);
    EXPECT_EQ(h.slot, 0u);
    EXPECT_EQ(h.gen, 1u);  ///< 失败使槽 gen+1
    EXPECT_EQ(pool.get(h).v, 7);
}

// ---------------------------------------------------------------------------
// for_each / reserve
// ---------------------------------------------------------------------------

TEST(GenerationTest, ForEachSeesOnlyAliveAndHandlesAreGettable) {
    GenerationalArena<Item> pool(4);
    std::vector<GHandle> hs;
    for (int i = 0; i < 6; ++i) hs.push_back(pool.create(i));
    pool.remove(hs[1]);
    pool.remove(hs[4]);

    std::vector<std::pair<GHandle, int>> seen;
    pool.for_each([&](Item& it, GHandle h) { seen.emplace_back(h, it.v); });
    ASSERT_EQ(seen.size(), 4u);
    int sum = 0;
    for (const auto& [h, v] : seen) {
        EXPECT_FALSE(h == hs[1]);  ///< 已删句柄不得再被携出（精确比较，防恒真断言）
        EXPECT_EQ(pool.get(h).v, v);  ///< 句柄可直接 get
        sum += v;
    }
    EXPECT_EQ(sum, 0 + 2 + 3 + 5);  ///< 活元素集合正确（1、4 已删）

    ///< 可写遍历 + const 遍历
    pool.for_each([&](Item& it, GHandle) { it.v += 100; });
    const auto& cpool = pool;
    int count = 0;
    cpool.for_each([&](const Item& it, GHandle h) {
        EXPECT_TRUE(cpool.alive(h));
        EXPECT_GE(it.v, 100);
        ++count;
    });
    EXPECT_EQ(count, 4);
}

TEST(GenerationTest, ReservePreextendsWithoutMovingStorage) {
    GenerationalArena<Item> pool(4);
    GHandle h = pool.create(1);
    Item* p = pool.try_get(h);
    pool.reserve(1000);
    EXPECT_GE(pool.capacity(), 1000u);
    EXPECT_EQ(pool.size(), 1u);
    EXPECT_EQ(pool.try_get(h), p);  ///< 块未搬动
    EXPECT_EQ(pool.get(h).v, 1);
    ///< 预扩后的空槽可正常 create
    for (int i = 0; i < 999; ++i) pool.create(i);
    EXPECT_EQ(pool.size(), 1000u);
}

// ---------------------------------------------------------------------------
// debug：remove 时未归还借用断言（与 Arena::reset 同规）
// ---------------------------------------------------------------------------

TEST(GenerationTest, RemoveWithOutstandingBorrowAborts) {
#ifndef NDEBUG
    ///< 借用登记与 remove 全部放在死亡语句内：父进程不执行该语句，
    ///< 不残留登记污染后续用例。
    ASSERT_EXIT(
        {
            GenerationalArena<Item> pool;
            GHandle h = pool.create(1);
            GARENA_MARK_BORROWED(pool.try_get(h));
            pool.remove(h);  ///< 该槽存储范围内有未归还借用 → abort
        },
        kAborted, "dangle");
#else
    GTEST_SUCCEED() << "release build: borrow check is no-op";
#endif
}
