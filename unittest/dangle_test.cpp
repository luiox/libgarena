#include <gtest/gtest.h>

#include <cstdlib>

#include "garena/arena.hpp"
#include "garena/dangle_check.hpp"

namespace {

// MSVC abort() 以退出码 3 终止；POSIX 下是 SIGABRT。
#ifdef _WIN32
const auto kAborted = ::testing::ExitedWithCode(3);
#else
const auto kAborted = ::testing::KilledBySignal(6);
#endif

void flushStderr() {
    fflush(stderr);
}

}  // namespace

// debug 构建：未归还借用在 reset 时应 abort；release 构建为空操作。
TEST(DangleTest, OutstandingBorrowAbortsAtReset) {
#ifndef NDEBUG
#ifdef _WIN32
    const auto kDied = ::testing::ExitedWithCode(3);  // MSVC abort()
#else
    const auto kDied = ::testing::KilledBySignal(6);  // SIGABRT
#endif
    ASSERT_EXIT({
        garena::Arena a;
        void* p = a.alloc(32, 8);
        GARENA_MARK_BORROWED(p);
        a.reset();  // 应 abort
        flushStderr();
        exit(0);
    }, kDied, "dangle");
#else
    GTEST_SUCCEED() << "release build: dangle check is no-op";
#endif
}

TEST(DangleTest, ReturnedBorrowPassesReset) {
    garena::Arena a;
    void* p = a.alloc(32, 8);
    GARENA_MARK_BORROWED(p);
    GARENA_MARK_RETURNED(p);
    a.reset();  // 不应 abort
    SUCCEED();
}

// 归还后再借出、再归还，多次循环不残留。
TEST(DangleTest, ReborrowCyclesAreClean) {
    garena::Arena a;
    for (int i = 0; i < 3; ++i) {
        void* p = a.alloc(16, 8);
        GARENA_MARK_BORROWED(p);
        GARENA_MARK_RETURNED(p);
    }
    a.reset();
    SUCCEED();
}

// ---------------------------------------------------------------------------
// v2 计数化回归（缺陷#3）
// ---------------------------------------------------------------------------

#ifndef NDEBUG

// 同一指针两次借出、只归还一次：集合去重会漏报，计数化必须仍检出。
TEST(DangleTest, CountedBorrowsStillDetectedAfterPartialReturn) {
    ASSERT_EXIT(
        {
            garena::Arena a;
            void* p = a.alloc(32, 8);
            GARENA_MARK_BORROWED(p);
            GARENA_MARK_BORROWED(p);  ///< 二次借出
            GARENA_MARK_RETURNED(p);  ///< 只归还一次
            a.reset();                ///< 仍有一次未归还 → abort
            exit(0);
        },
        kAborted, "dangle");
}

// 全部归还：计数归零，reset 干净通过。
TEST(DangleTest, CountedBorrowsAllReturnedIsClean) {
    garena::Arena a;
    void* p = a.alloc(32, 8);
    GARENA_MARK_BORROWED(p);
    GARENA_MARK_BORROWED(p);
    GARENA_MARK_RETURNED(p);
    GARENA_MARK_RETURNED(p);
    a.reset();  ///< 不应 abort
    SUCCEED();
}

// 下溢（RETURNED 多于 BORROWED）本身即违规：abort。
TEST(DangleTest, ReturnUnderflowAborts) {
    ASSERT_EXIT(
        {
            void* q = static_cast<void*>(&q);
            GARENA_MARK_RETURNED(q);  ///< 无配对 BORROWED
            exit(0);
        },
        kAborted, "dangle underflow");
    /// 归零后再次 RETURNED 同样是下溢
    ASSERT_EXIT(
        {
            garena::Arena a;
            void* p = a.alloc(16, 8);
            GARENA_MARK_BORROWED(p);
            GARENA_MARK_RETURNED(p);
            GARENA_MARK_RETURNED(p);  ///< 已归零再归还
            exit(0);
        },
        kAborted, "dangle underflow");
}

#endif  // !NDEBUG
