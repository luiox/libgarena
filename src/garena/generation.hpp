#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include <libca/core/datatype.hpp>

#include "garena/arena.hpp"  ///< kMaxAlloc / abortSizeOverflow：与 Layer A 共用的 OOM 契约
#include "garena/dangle_check.hpp"

/// @file generation.hpp
/// Layer B：生成式句柄池 GenerationalArena<T> + 句柄 GHandle{owner, slot, gen}。
/// 适用场景：需要逐个增删、以稳定身份被引用的长命对象（指令、基本块、Label、
/// 缓存项）；有共同死亡点的临时物请用 Layer A 的 garena::Arena（arena.hpp）。
///
/// API 冻结与语义基线：doc/design.md（Layer B 硬契约）与 doc/capabilities.md；
/// 句柄为 owner/slot/gen 三元组方言，与上层字节码 IR 的 InsnId 同构（字段顺序
/// 与宽度一致），见 doc/design.md「句柄方言」。

namespace garena {

/// 非法槽位哨兵。kNoHandle 即 {0, kInvalidSlot, 0}。
inline constexpr ca::u32 kInvalidSlot = 0xFFFFFFFFu;

/// 通用句柄：{owner, slot, gen}，12 字节，trivially copyable，可存入容器/跨函数传递。
/// - owner：实例进程级唯一代号（构造时自增分配）。跨实例（跨 DOM/跨池/跨类型）
///   使用句柄被结构性拒绝：debug abort；release 下 try_get 返回 nullptr / get abort。
/// - slot：槽位下标；gen：该槽位代数。remove 与 clear 均使 gen+1，旧句柄立即
///   失效，不存在 ABA。
/// - gen 回绕：u32 需单槽 2^32 次删除，实践不可达；回绕不作特殊处理（记录在案
///   的接受界）。
struct GHandle {
    ca::u32 owner = 0;
    ca::u32 slot  = kInvalidSlot;
    ca::u32 gen   = 0;

    constexpr explicit operator bool() const noexcept { return slot != kInvalidSlot; }
    friend constexpr bool operator==(const GHandle& a, const GHandle& b) noexcept {
        return a.owner == b.owner && a.slot == b.slot && a.gen == b.gen;
    }
    friend constexpr bool operator!=(const GHandle& a, const GHandle& b) noexcept {
        return !(a == b);
    }
};
inline constexpr GHandle kNoHandle{};

static_assert(sizeof(GHandle) == 12, "GHandle must stay 12 bytes (frozen ABI)");
static_assert(std::is_trivially_copyable_v<GHandle>, "GHandle must be trivially copyable");

/// 生成式句柄池：O(1) create/remove、地址稳定、gen 校验防 ABA/UAF。
///
/// 硬契约（违反即 bug，消费者可依赖）：
///  1. 地址稳定：T 存储在块链中（每块 block_slots 个 T，默认 1024），块分配后
///     永不搬动/归还。T* 在 remove(该槽) 或池析构前恒定；其他元素的增删不影响
///     任何既有 T*。槽位元数据在旁路 vector 中，可自由 realloc、地址不对外。
///  2. 单元素真删：remove(h) 原地析构 T、gen+1、槽入空闲链，O(1)。存储保留
///     （不归还，与 Layer A chunk 复用同哲学）。
///  3. 校验行为：slot/gen 失配（越界、gen 不等、槽已死）——try_get/alive 全构建
///     返回 nullptr/false（预期失败路径，热路径可用）；get 同样校验，失配
///     abort（打印期望/实际 owner-slot-gen）——release 也启用，确定性失败优于
///     UB。owner 失配（跨实例使用句柄）是结构性错误，按冻结规格区别对待：
///     debug 一律 abort（try_get/get/alive/remove 都是"使用"）；release 下
///     try_get/alive 返回 nullptr/false、get abort、remove 返回 false。
///     不提供 operator[]（防绕过校验的快路径）。
///  4. clear()：全部活槽置死并 gen+1（保留存储）。与 Arena::reset 的关键差异：
///     clear 后旧句柄必然校验失败，而 reset 后旧指针不可检。clear 不复用 gen 值。
///  5. 异常安全：create 中 T 构造抛出 → 槽置死、gen+1、重入空闲链后重抛，
///     不发布半构造对象。构造期间槽已出空闲链但尚未发布；T 的构造函数不得
///     重入本池的 clear() 或移动/销毁本池——正在构造的槽会被一并清空或存储
///     被释放。重入 create/reserve 受支持（create 在构造后重新获取槽元数据，
///     不持有跨构造期的引用）。
///  6. 非线程安全：单线程顺序模型，与 garena::Arena 一致。
///  7. move 语义：move-only；移动转移块与元数据所有权，owner 不变（目的实例
///     维持原代号）；被移动壳 owner 归零（== kNoHandle.owner，旧句柄必失配）。
///  8. 分配失败策略（与 Layer A 契约 6 对齐）：块/元数据 malloc 失败一律
///     std::abort（带消息），不抛 bad_alloc、不返回空；create 的唯一异常源是
///     T 的构造函数。
template <class T>
class GenerationalArena {
public:
    using Handle = GHandle;

    /// 默认每块 1024 槽。
    static constexpr ca::usize kDefaultBlockSlots = 1024;

    GenerationalArena() : GenerationalArena(kDefaultBlockSlots) {}

    /// 每块槽位数（≥1，传 0 视为 1）。块在池生命期内尺寸恒定。
    explicit GenerationalArena(ca::usize block_slots);

    ~GenerationalArena();

    GenerationalArena(const GenerationalArena&)            = delete;
    GenerationalArena& operator=(const GenerationalArena&) = delete;
    GenerationalArena(GenerationalArena&& other) noexcept;
    GenerationalArena& operator=(GenerationalArena&& other) noexcept;

    /// 就地构造并发布，返回句柄。槽位取自空闲链（复用，gen 为该槽当前代）或追加。
    /// 强异常保证（契约 5）。
    template <class... A>
    Handle create(A&&... args);

    /// 校验取值。slot/gen 失配（越界、gen 不等、槽已死）→ nullptr，全构建生效；
    /// owner 失配（跨实例）→ release nullptr、debug abort（契约 3）。
    T*       try_get(Handle h) noexcept;
    const T* try_get(Handle h) const noexcept;

    /// 校验取值。任一失配 → std::abort（消息含期望/实际状态），全构建生效；
    /// kNoHandle 同样 abort（空句柄不对应任何活对象）。
    T&       get(Handle h);
    const T& get(Handle h) const;

    /// 句柄当前是否指向活元素。slot/gen 失配返回 false；owner 失配 release
    /// 返回 false、debug abort（契约 3）。kNoHandle 恒 false。
    bool     alive(Handle h) const noexcept;

    /// O(1) 删除。slot/gen 失配返回 false（预期失败路径）；owner 失配 release
    /// 返回 false、debug abort（契约 3）。成功：析构 T、gen+1、槽入空闲链、
    /// live_count-1，返回 true。
    /// debug 下：该槽存储范围内若有 GARENA_MARK_BORROWED 未归还 → abort
    /// （与 Arena::reset 同规）。
    bool     remove(Handle h);

    /// 全部置死：析构全部活元素（槽序逆序，与 Arena::reset 同向）、每个槽
    /// gen+1、空闲链重置为全槽。保留存储。O(n)。
    void     clear() noexcept;

    ca::usize size() const noexcept;      ///< 活元素数
    ca::usize capacity() const noexcept;  ///< 总槽位数（含死槽）

    /// 预扩槽位与块（只扩不缩；新槽 gen=0 未活）。
    void      reserve(ca::usize n);

    /// 遍历活元素（槽序）：f(T&, GHandle)。建立元素→外部映射的规范入口。
    template <class F>
    void for_each(F&& f);
    template <class F>
    void for_each(F&& f) const;

    /// 调试：指针是否落在本池某块存储内（块链线性，块数 = capacity/block_slots）。
    bool     owns(const void* p) const noexcept;

    /// 本实例代号（诊断/日志用）。
    ca::u32  owner_id() const noexcept;

private:
    struct SlotMeta {
        ca::u32 gen;        ///< 该槽当前代数；remove/clear/构造失败 均 +1
        ca::u32 next_free;  ///< 空闲链（kInvalidSlot = 链尾）
        bool    alive;
    };
    struct Block {
        void* raw = nullptr;  ///< malloc 原始指针（free 用）
        T*    base = nullptr; ///< 对齐后首槽地址
    };

    T*       slotPtr(ca::u32 slot) noexcept;
    const T* slotPtr(ca::u32 slot) const noexcept;
    SlotMeta&       metaAt(ca::u32 slot) noexcept;
    const SlotMeta& metaAt(ca::u32 slot) const noexcept;
    /// 内部校验：合法指向活元素返回非空；否则 nullptr。
    T*       validate(Handle h) noexcept;
    const T* validate(Handle h) const noexcept;
    /// 校验失败时打印期望/实际状态并 abort（get 的失败路径）。
    [[noreturn]] void abortMismatch(Handle h) const;
    /// 扩容到至少 n 槽：追加块 + 元数据 + 空闲链。系统分配失败 abort（契约 8）。
    void      ensureSlots(ca::usize n);
    void      destroyAllAlive() noexcept;
    void      freeBlocks() noexcept;

    std::vector<Block>       blocks_;
    std::vector<SlotMeta>    slots_;       ///< 下标 = slot；地址不对外
    ca::u32                  free_head_ = kInvalidSlot;
    ca::u32                  owner_ = 0;
    ca::u32                  live_count_ = 0;
    ca::usize                block_slots_ = kDefaultBlockSlots;
};

/// 进程级 owner 代号源。单线程模型下普通静态计数即可；实现用 std::atomic
/// 以防未来复用。代号从 1 起（0 保留给 kNoHandle/被移动壳，保证其句柄必失配）。
ca::u32 next_owner_id() noexcept;

// ---------------------------------------------------------------------------
// 实现（header-only；generation.cpp 只放非模板核心）
// ---------------------------------------------------------------------------

template <class T>
GenerationalArena<T>::GenerationalArena(ca::usize block_slots)
    : owner_(next_owner_id()),
      block_slots_(block_slots == 0 ? 1 : block_slots) {}

template <class T>
GenerationalArena<T>::~GenerationalArena() {
    destroyAllAlive();
    freeBlocks();
}

template <class T>
GenerationalArena<T>::GenerationalArena(GenerationalArena&& other) noexcept {
    *this = std::move(other);
}

template <class T>
GenerationalArena<T>& GenerationalArena<T>::operator=(GenerationalArena&& other) noexcept {
    if (this != &other) {
        destroyAllAlive();
        freeBlocks();
        blocks_ = std::move(other.blocks_);
        slots_ = std::move(other.slots_);
        free_head_ = other.free_head_;
        owner_ = other.owner_;        ///< 契约 7：owner 不变（目的实例维持原代号）
        live_count_ = other.live_count_;
        block_slots_ = other.block_slots_;
        other.blocks_.clear();
        other.slots_.clear();
        other.free_head_ = kInvalidSlot;
        other.owner_ = 0;             ///< 被移动壳归零（0 == kNoHandle.owner）
        other.live_count_ = 0;
        other.block_slots_ = kDefaultBlockSlots;
    }
    return *this;
}

template <class T>
T* GenerationalArena<T>::slotPtr(ca::u32 slot) noexcept {
    const ca::usize b = slot / block_slots_;
    const ca::usize i = slot % block_slots_;
    return blocks_[b].base + i;
}

template <class T>
const T* GenerationalArena<T>::slotPtr(ca::u32 slot) const noexcept {
    const ca::usize b = slot / block_slots_;
    const ca::usize i = slot % block_slots_;
    return blocks_[b].base + i;
}

template <class T>
auto GenerationalArena<T>::metaAt(ca::u32 slot) noexcept -> SlotMeta& {
    return slots_[slot];
}

template <class T>
auto GenerationalArena<T>::metaAt(ca::u32 slot) const noexcept -> const SlotMeta& {
    return slots_[slot];
}

template <class T>
T* GenerationalArena<T>::validate(Handle h) noexcept {
    if (h.slot == kInvalidSlot) return nullptr;
    if (h.owner != owner_) {
#ifndef NDEBUG
        abortMismatch(h);  ///< 跨实例句柄：结构性拒绝（debug 一律 abort，契约 3）
#endif
        return nullptr;
    }
    if (h.slot >= slots_.size()) return nullptr;
    const SlotMeta& m = metaAt(h.slot);
    if (!m.alive || m.gen != h.gen) return nullptr;
    return slotPtr(h.slot);
}

template <class T>
const T* GenerationalArena<T>::validate(Handle h) const noexcept {
    if (h.slot == kInvalidSlot) return nullptr;
    if (h.owner != owner_) {
#ifndef NDEBUG
        abortMismatch(h);
#endif
        return nullptr;
    }
    if (h.slot >= slots_.size()) return nullptr;
    const SlotMeta& m = metaAt(h.slot);
    if (!m.alive || m.gen != h.gen) return nullptr;
    return slotPtr(h.slot);
}

template <class T>
void GenerationalArena<T>::abortMismatch(Handle h) const {
    // 消息为 ASCII：死亡测试的 stderr 正则依赖它（跨代码页稳定）。
    // I/O 一律走 dangle_abort（I/O 收敛在 dangle_check.cpp，规避上游 #undef stderr）。
    if (h.slot == kInvalidSlot) {
        ///< kNoHandle 的 owner=0 只是字段默认值，不是跨实例使用：先于 owner
        ///< 分支报告，避免 get(kNoHandle) 被误报成 owner mismatch。
        detail::dangle_abort(
            "garena: handle rejected - null handle: handle(owner=%u slot=0xFFFFFFFF gen=%u)\n",
            h.owner, h.gen);
    } else if (h.owner != owner_) {
        detail::dangle_abort(
            "garena: handle rejected - owner mismatch: handle(owner=%u slot=%u gen=%u) "
            "vs pool(owner=%u)\n",
            h.owner, h.slot, h.gen, owner_);
    } else if (h.slot >= slots_.size()) {
        detail::dangle_abort(
            "garena: handle rejected - slot out of range: handle(owner=%u slot=%u gen=%u) "
            "vs capacity=%llu\n",
            h.owner, h.slot, h.gen,
            static_cast<unsigned long long>(slots_.size()));
    } else {
        const SlotMeta& m = metaAt(h.slot);
        detail::dangle_abort(
            "garena: handle rejected - stale handle: handle(owner=%u slot=%u gen=%u) "
            "vs slot(gen=%u alive=%d)\n",
            h.owner, h.slot, h.gen, m.gen, m.alive ? 1 : 0);
    }
}

template <class T>
void GenerationalArena<T>::ensureSlots(ca::usize n) {
    if (n <= slots_.size()) return;
    ///< 契约 8：溢出先判后乘——块字节数/块数任一回绕即 abort，不静默欠额
    if (block_slots_ > (kMaxAlloc - (alignof(T) - 1)) / sizeof(T)) {
        abortSizeOverflow();
    }
    const ca::usize num_blocks = n / block_slots_ + (n % block_slots_ != 0 ? 1 : 0);
    if (num_blocks > (~static_cast<ca::usize>(0)) / block_slots_) {
        abortSizeOverflow();
    }
    // 块数对齐：需要的新块数（新块整块追加）
    const ca::usize old_cap = slots_.size();
    const ca::usize new_cap = num_blocks * block_slots_;
    ///< 槽位下标域为 u32（GHandle.slot / 空闲链 next_free）：new_cap 越过
    ///< kInvalidSlot 后下方 static_cast 会静默截断、破坏空闲链——先判后扩，
    ///< 与上方块数溢出检查同族（契约 8）。
    ///< == kInvalidSlot 时放行是刻意的：最大合法槽位下标为 kInvalidSlot-1，
    ///< 与空闲链 sentinel 不重合
    if (new_cap > static_cast<ca::usize>(kInvalidSlot)) {
        abortSizeOverflow();
    }
    try {
        blocks_.reserve(new_cap / block_slots_);
        slots_.resize(new_cap);
    } catch (...) {  ///< 契约 8：系统分配失败 abort，不向 create 泄漏 bad_alloc
        detail::dangle_abort("garena: out of memory (generation metadata)\n");
    }
    while (blocks_.size() * block_slots_ < new_cap) {
        const ca::usize bytes = block_slots_ * sizeof(T);
        void* raw = std::malloc(bytes + (alignof(T) - 1));
        if (!raw) {
            detail::dangle_abort("garena: out of memory (generation block, %llu bytes)\n",
                                 static_cast<unsigned long long>(bytes));
        }
        Block blk;
        blk.raw = raw;
        auto addr = reinterpret_cast<ca::usize>(raw);
        addr = (addr + (alignof(T) - 1)) / alignof(T) * alignof(T);
        blk.base = static_cast<T*>(reinterpret_cast<void*>(addr));
        blocks_.push_back(blk);  ///< blocks_.reserve 已保证不重分配（不抛）
    }
    ///< 新槽：gen=0、未活，串成空闲链（从高到低入链 → 弹出序即槽序）
    for (ca::usize s = new_cap; s > old_cap; --s) {
        SlotMeta& m = slots_[s - 1];
        m.gen = 0;
        m.alive = false;
        m.next_free = free_head_;
        free_head_ = static_cast<ca::u32>(s - 1);
    }
}

template <class T>
template <class... A>
auto GenerationalArena<T>::create(A&&... args) -> Handle {
    if (free_head_ == kInvalidSlot) ensureSlots(slots_.size() + 1);
    const ca::u32 slot = free_head_;
    free_head_ = metaAt(slot).next_free;
    T* p = slotPtr(slot);
    try {
        new (static_cast<void*>(p)) T(std::forward<A>(args)...);
    } catch (...) {
        ///< 契约 5：不发布半构造对象；gen+1 防异常期被捕获的旧句柄复活。
        ///< 构造期间槽元数据可能被重入的 create/reserve 扩容搬动，重新取引用。
        SlotMeta& m = metaAt(slot);
        m.gen++;
        m.alive = false;
        m.next_free = free_head_;
        free_head_ = slot;
        throw;
    }
    SlotMeta& m = metaAt(slot);
    m.alive = true;
    ++live_count_;
    return Handle{owner_, slot, m.gen};
}

template <class T>
T* GenerationalArena<T>::try_get(Handle h) noexcept {
    return validate(h);
}

template <class T>
const T* GenerationalArena<T>::try_get(Handle h) const noexcept {
    return validate(h);
}

template <class T>
T& GenerationalArena<T>::get(Handle h) {
    if (T* p = validate(h)) return *p;
    abortMismatch(h);  ///< release 也启用（契约 3）
}

template <class T>
const T& GenerationalArena<T>::get(Handle h) const {
    if (const T* p = validate(h)) return *p;
    abortMismatch(h);
}

template <class T>
bool GenerationalArena<T>::alive(Handle h) const noexcept {
    return validate(h) != nullptr;
}

template <class T>
bool GenerationalArena<T>::remove(Handle h) {
    if (!validate(h)) return false;  ///< 预期失败路径，不 abort
    T* p = slotPtr(h.slot);
#ifndef NDEBUG
    {
        const auto lo = reinterpret_cast<ca::usize>(p);
        const auto hi = lo + sizeof(T);
        DangleCheck::instance().assertNoneOwnedBy([lo, hi](const void* q) {
            const auto a = reinterpret_cast<ca::usize>(q);
            return a >= lo && a < hi;
        });
    }
#endif
    p->~T();
    SlotMeta& m = metaAt(h.slot);
    m.alive = false;
    m.gen++;
    m.next_free = free_head_;
    free_head_ = h.slot;
    --live_count_;
    return true;
}

template <class T>
void GenerationalArena<T>::destroyAllAlive() noexcept {
#ifndef NDEBUG
    ///< 不按 live_count_ 短路：已 remove 的槽仍可能有登记在案的借用，
    ///< 析构是清理这些登记的最后机会
    DangleCheck::instance().assertNoneOwnedBy(
        [this](const void* q) { return owns(q); });
#endif
    ///< 槽序逆序析构；槽位复用后与构造序无关（与 Arena::reset 同为逆序执行）
    for (ca::u32 s = static_cast<ca::u32>(slots_.size()); s > 0; --s) {
        SlotMeta& m = slots_[s - 1];
        if (m.alive) slotPtr(s - 1)->~T();
    }
}

template <class T>
void GenerationalArena<T>::freeBlocks() noexcept {
    for (Block& b : blocks_) {
        std::free(b.raw);
    }
    blocks_.clear();
    slots_.clear();
    free_head_ = kInvalidSlot;
    live_count_ = 0;
}

template <class T>
void GenerationalArena<T>::clear() noexcept {
    ///< destroyAllAlive 内含 debug 未归还借用断言（析构前执行）
    destroyAllAlive();
    ///< 全部槽 gen+1（契约 4：clear 与 remove 同责，旧句柄必然失配），
    ///< 空闲链重置为全槽（弹出序 = 槽序）
    for (ca::u32 s = static_cast<ca::u32>(slots_.size()); s > 0; --s) {
        SlotMeta& m = slots_[s - 1];
        m.alive = false;
        m.gen++;
        m.next_free = free_head_;
        free_head_ = s - 1;
    }
    live_count_ = 0;
}

template <class T>
ca::usize GenerationalArena<T>::size() const noexcept {
    return live_count_;
}

template <class T>
ca::usize GenerationalArena<T>::capacity() const noexcept {
    return slots_.size();
}

template <class T>
void GenerationalArena<T>::reserve(ca::usize n) {
    ensureSlots(n);
}

template <class T>
template <class F>
void GenerationalArena<T>::for_each(F&& f) {
    for (ca::u32 s = 0; s < slots_.size(); ++s) {
        const SlotMeta& m = metaAt(s);
        if (m.alive) f(*slotPtr(s), Handle{owner_, s, m.gen});
    }
}

template <class T>
template <class F>
void GenerationalArena<T>::for_each(F&& f) const {
    for (ca::u32 s = 0; s < slots_.size(); ++s) {
        const SlotMeta& m = metaAt(s);
        if (m.alive) f(*slotPtr(s), Handle{owner_, s, m.gen});
    }
}

template <class T>
bool GenerationalArena<T>::owns(const void* p) const noexcept {
    const auto addr = reinterpret_cast<ca::usize>(p);
    for (const Block& b : blocks_) {
        const auto lo = reinterpret_cast<ca::usize>(b.base);
        if (addr >= lo && addr < lo + block_slots_ * sizeof(T)) return true;
    }
    return false;
}

template <class T>
ca::u32 GenerationalArena<T>::owner_id() const noexcept {
    return owner_;
}

}  // namespace garena
