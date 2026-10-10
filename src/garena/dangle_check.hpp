#pragma once

#include <cstddef>

#include <libca/core/datatype.hpp>

/// @file dangle_check.hpp
/// 借用视图登记 / 悬垂检查（仅 debug 生效；NDEBUG 下全部为空操作，零开销）。
///
/// 用法：消费者把从 arena 借出的裸指针/视图显式登记：
///   GARENA_MARK_BORROWED(ptr);        // 借出
///   GARENA_MARK_RETURNED(ptr);        // 归还（用完立即成对）
/// Arena::reset()/析构 时若仍有未归还且落在本 arena chunk 内的借用，
/// 断言失败 —— 把「视图指向临时」从纪律变成可执行检查。
///
/// v2 计数化（缺陷#3）：登记表按 指针 → 借用计数 维护（v0.1 为集合去重，
/// 同一地址两次借出、一次归还会把未归还借用从表里抹掉而漏报）。归零才移除；
/// 「归还多于借出」（下溢）同样是被检查对象，直接 abort。

/// 诊断出口（定义在 dangle_check.cpp）。stderr 在 UCRT 仅为宏（无底层符号），
/// 且可被上游头合法 #undef（subprocess.hpp 为暴露同名成员），任何 TU 的 include
/// 顺序都可能让它失效——故全部 I/O 收敛到唯一 .cpp（stdio 最先包含、不碰
/// process 族头，stderr 宏必在）。打印后 abort，不返回。消息为 ASCII：死亡
/// 测试的 stderr 正则依赖它（跨代码页稳定）。
/// 声明无条件给出：generation.hpp 的 OOM/handle 拒收路径在 release（NDEBUG）
/// 下同样调用本出口。
namespace garena {
namespace detail {
[[noreturn]] void dangle_abort(const char* format, ...);
}
}  // namespace garena

#ifdef NDEBUG

#define GARENA_MARK_BORROWED(p) ((void)0)
#define GARENA_MARK_RETURNED(p) ((void)0)

#else

#include <unordered_map>

namespace garena {

/// 全局未归还借用登记表（计数化，缺陷#3）。单线程模型，非线程安全。
class DangleCheck {
public:
    static DangleCheck& instance() {
        /// 故意泄漏：本表被任意静态寿命 Arena 的 ~reset() 依赖，而 atexit 逆序
        /// 析构不可控（函数局部 static 单例可能先于它们被析构，析构后再访问即
        /// UAF）。checker 类单例不参与析构，进程退出由 OS 回收。
        static DangleCheck* s = new DangleCheck();
        return *s;
    }

    void borrow(const void* p) { ++outstanding_[p]; }

    void returned(const void* p) {
        auto it = outstanding_.find(p);
        if (it == outstanding_.end() || it->second == 0) {
            detail::dangle_abort(
                "[garena] dangle underflow: GARENA_MARK_RETURNED for %p "
                "without matching GARENA_MARK_BORROWED\n",
                p);
        }
        if (--it->second == 0) outstanding_.erase(it);
    }

    /// reset/析构 前调用：iter 覆盖所有未归还借用，任何一个落在本 arena
    /// chunk 范围内 [begin, end) 即判悬垂，报错终止。
    template <class RangeFn>
    void assertNoneOwnedBy(const RangeFn& chunk_range) {
        for (const auto& kv : outstanding_) {
            if (chunk_range(kv.first)) {
                detail::dangle_abort(
                    "[garena] dangle: borrowed pointer %p still outstanding "
                    "(%u time(s)) at arena teardown (missing GARENA_MARK_RETURNED)\n",
                    kv.first, kv.second);
            }
        }
    }

private:
    DangleCheck() = default;
    std::unordered_map<const void*, ca::u32> outstanding_;
};

}  // namespace garena

#define GARENA_MARK_BORROWED(p) ::garena::DangleCheck::instance().borrow(p)
#define GARENA_MARK_RETURNED(p) ::garena::DangleCheck::instance().returned(p)

#endif  // NDEBUG
