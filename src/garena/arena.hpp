/// @file arena.hpp
/// 通用 bump/monotonic arena：chunk 链式分配，整批分配、整批释放。
///
/// 硬契约（违反即 bug，消费者可依赖）：
///  1. 地址稳定：已分配内存永不挪动，指针在 reset()/析构 前始终有效；
///  2. 无逐条释放：单个对象没有 free/delete，只能整池 reset()/析构；
///  3. reset()/析构 后旧指针全部失效（红线）；
///  4. 非线程安全：单线程顺序模型，与 ca::str::Utf8StringArena 一致；
///  5. move 语义：move-only；move 后旧 arena 变空壳，chunk 所有权整体转移；
///  6. 分配失败策略（v2 成文）：底层 malloc/realloc 失败或请求尺寸溢出
///     （> kMaxAlloc）一律 std::abort（带消息）。本库不抛异常、不返回空指针；
///     唯一例外：alloc(0, align) 返回 nullptr（空请求无地址语义——v2 起改约，
///     v0.1 的"可用作哨兵"注释作废）。
///
/// 选型判据见 doc/design.md：有共同死亡点 → Arena；
/// 需要逐个增删、以稳定身份被引用 → GenerationalArena（generation.hpp）；
/// 确需共享可变所有权 → 引用计数，不要硬套本库。

#pragma once

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#include <libca/core/datatype.hpp>

#include "garena/dangle_check.hpp"

namespace garena {

/// 单次分配尺寸上限（SIZE_MAX/2）：超过即视为尺寸计算溢出，abort（缺陷#1）。
inline constexpr ca::usize kMaxAlloc = (~static_cast<ca::usize>(0)) / 2;

/// 尺寸计算溢出 → abort（契约 6；模板 createArray 与 .cpp 共用，缺陷#1）。
/// 注：声明必须先于 class Arena——成员模板 createArray 体内无限定调用它，
/// GCC 两阶段查找要求其类定义前可见（MSVC 宽容，Linux CI 实证报错）。
[[noreturn]] void abortSizeOverflow();

/// chunk 链式 bump arena。move-only；move 之后旧 arena 变空壳，旧指针仍有效
/// （chunk 所有权整体转移，地址稳定契约不破坏）。
class Arena {
public:
    /// 默认首块 64KB，与 ca::str::Utf8StringArena 对齐。
    static constexpr ca::usize kDefaultFirstChunkBytes = 64 * 1024;

    Arena() noexcept;
    explicit Arena(ca::usize first_chunk_bytes);
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;

    /// 按对齐分配 bytes 内存。返回地址终身稳定（直到 reset/析构）。
    /// bytes == 0 返回 nullptr（空请求无地址语义，契约 6）；
    /// bytes/align 溢出或系统分配失败 → std::abort，不返回空。
    void* alloc(ca::usize bytes, ca::usize align);

    /// bump 构造 T。non-trivial 析构在 reset()/析构 时逆序统一执行。
    template <class T, class... A>
    T* create(A&&... args) {
        void* mem = alloc(sizeof(T), alignof(T));
        T* obj = new (mem) T(std::forward<A>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            registerDestructor(obj, sizeof(T), [](void* p) { static_cast<T*>(p)->~T(); });
        }
        return obj;
    }

    /// 构造 T 的数组（元素个数运行期决定，值初始化：标量元素零初始化）。
    /// non-trivial 析构同样被接管。
    /// count == 0 返回 nullptr。异常安全（缺陷#5）：第 k 个元素构造抛出时，
    /// 逆序析构已构造的前 k-1 个、把 bump 指针回退到分配前位置后重抛——
    /// 不泄漏半构造数组（含数组存储本身）。
    /// （约束：T 的构造函数不得再向本 arena 分配内存，否则回退会波及嵌套产物。）
    template <class T>
    T* createArray(ca::usize count) {
        if (count == 0) return nullptr;
        if (count > kMaxAlloc / sizeof(T)) abortSizeOverflow();
        const ca::usize pre_chunk_count = chunk_count_;
        const ca::usize pre_used = lastChunkUsed();
        void* mem = alloc(sizeof(T) * count, alignof(T));
        T* arr = static_cast<T*>(mem);
        ca::usize constructed = 0;
        try {
            for (; constructed < count; ++constructed) {
                new (static_cast<void*>(arr + constructed)) T();
            }
        } catch (...) {
            while (constructed > 0) {
                --constructed;
                (arr + constructed)->~T();
            }
            rollbackBump(pre_chunk_count, pre_used);
            throw;
        }
        if constexpr (!std::is_trivially_destructible_v<T>) {
            registerArrayDestructor(arr, count, sizeof(T), [](void* p) { static_cast<T*>(p)->~T(); });
        }
        return arr;
    }

    /// 指针是否落在本 arena 的某个 chunk 内（含已分配与未使用的 chunk 空间）。
    /// 归属判别/诊断口径；范围判定，不追踪逐块 allocated 边界。
    /// v2 起内部为基址有序二分 + 最近命中缓存（缺陷#8），单次 O(log chunks)、
    /// 缓存命中 O(1)。口径：诊断断言与「迁移期混合清除」的节点归属
    /// 判别（如 libjbytecode InsnList::disposeNode，release 也生效）是合法消费方；
    /// 不得进入 alloc/create 这类逐元素分配热路径。
    bool owns(const void* p) const noexcept;

    /// 已分配字节数（各 chunk used 之和，不含 chunk 头）。
    ca::usize used_bytes() const noexcept;

    /// chunk 总容量字节数。
    ca::usize total_bytes() const noexcept;

    /// 当前 dtor 登记表条数（诊断用，缺陷#7 披露配套）。
    ca::usize dtor_registrations() const noexcept;

    /// 当前 dtor 登记表占用字节数。披露：trivial T 零 per-allocation 元数据；
    /// non-trivial T 每对象（数组按数组）一条 DtorEntry，x64 上 ≈32B（缺陷#7）。
    ca::usize dtor_overhead_bytes() const noexcept;

    /// 整批释放：逆序执行 non-trivial 析构 → 释放全部 chunk。
    /// 红线：此后所有旧指针失效；debug 下若仍有登记未归还的借用会断言失败。
    /// debug 下析构后对全部 chunk 空间填 0xDD 毒化（缺陷#4）：使"reset 后
    /// 继续用旧指针"大概率立即崩溃（raw 指针路线的检查边界：毒化只能提高
    /// 崩溃概率，不是确定性检查；确定性检查请用 Layer B 的 gen 句柄）。
    void reset() noexcept;

private:
    struct Chunk {
        ca::u8* data = nullptr;
        ca::usize capacity = 0;
        ca::usize used = 0;
    };
    struct DtorEntry {
        void* obj;
        void (*destroy)(void*);     ///< 销毁单个元素
        ca::usize count;            ///< 单对象为 1
        ca::usize elem_size;        ///< 单对象为 sizeof 元素
    };

    void registerDestructor(void* obj, ca::usize elem_size, void (*destroy)(void*));
    void registerArrayDestructor(void* obj, ca::usize count, ca::usize elem_size,
                                 void (*destroy)(void*));
    ca::u8* allocInChunk(ca::usize bytes, ca::usize align);
    void growChunk(ca::usize min_bytes);
    ca::usize lastChunkUsed() const noexcept;
    /// 回退到 (pre_chunk_count, pre_used) 快照。alloc 快照后新开了 chunk →
    /// 该块整块闲置（used=0，供后续复用）；否则恢复原 chunk 的 used。
    void rollbackBump(ca::usize pre_chunk_count, ca::usize pre_used) noexcept;
    void runDestructors() noexcept;
    void freeChunks() noexcept;

    Chunk* chunks_ = nullptr;      ///< 动态数组（分配序，bump 用末尾块）
    ca::usize* sorted_ = nullptr;  ///< chunk 下标按基址升序（owns() 二分用，缺陷#8）
    ca::usize chunk_count_ = 0;
    ca::usize chunk_capacity_ = 0;
    mutable ca::usize last_hit_ = kNoChunkIndex;  ///< owns() 最近命中缓存
    ca::usize first_chunk_bytes_ = kDefaultFirstChunkBytes;
    ca::usize next_chunk_bytes_ = 0;

    DtorEntry* dtors_ = nullptr;   ///< non-trivial 析构登记表，分配序 → reset 逆序执行
    ca::usize dtor_count_ = 0;
    ca::usize dtor_capacity_ = 0;

    static constexpr ca::usize kNoChunkIndex = static_cast<ca::usize>(-1);
};

/// 带进位加法：回绕即 abortSizeOverflow（growChunk 参数检查用）。
inline ca::usize checkedAdd(ca::usize a, ca::usize b) {
    const ca::usize r = a + b;
    if (r < a) abortSizeOverflow();
    return r;
}

}  // namespace garena
