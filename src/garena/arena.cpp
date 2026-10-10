#include "garena/arena.hpp"

#include <cstdio>
#include <cstdlib>

namespace garena {

namespace {

constexpr ca::usize kMinChunkBytes = 256;  ///< 过小请求也保证一块不至于太碎

ca::usize alignUp(ca::usize value, ca::usize align) {
    return (value + align - 1) / align * align;
}

bool addrInRange(const void* base, ca::usize capacity, ca::usize addr) {
    return addr >= reinterpret_cast<ca::usize>(base) &&
           addr < reinterpret_cast<ca::usize>(base) + capacity;
}

}  // namespace

void abortSizeOverflow() {
    std::fprintf(stderr, "garena: allocation size overflow\n");
    std::abort();
}

Arena::Arena() noexcept = default;

Arena::Arena(ca::usize first_chunk_bytes)
    : first_chunk_bytes_(first_chunk_bytes < kMinChunkBytes ? kMinChunkBytes
                                                            : first_chunk_bytes) {}

Arena::~Arena() {
    reset();
    freeChunks();
}

Arena::Arena(Arena&& other) noexcept { *this = std::move(other); }

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        reset();
        freeChunks();
        chunks_ = other.chunks_;
        sorted_ = other.sorted_;
        chunk_count_ = other.chunk_count_;
        chunk_capacity_ = other.chunk_capacity_;
        last_hit_ = other.last_hit_;
        first_chunk_bytes_ = other.first_chunk_bytes_;
        next_chunk_bytes_ = other.next_chunk_bytes_;
        dtors_ = other.dtors_;
        dtor_count_ = other.dtor_count_;
        dtor_capacity_ = other.dtor_capacity_;
        other.chunks_ = nullptr;
        other.sorted_ = nullptr;
        other.chunk_count_ = 0;
        other.chunk_capacity_ = 0;
        other.last_hit_ = kNoChunkIndex;
        other.next_chunk_bytes_ = 0;
        other.dtors_ = nullptr;
        other.dtor_count_ = 0;
        other.dtor_capacity_ = 0;
    }
    return *this;
}

void* Arena::alloc(ca::usize bytes, ca::usize align) {
    if (align == 0) align = 1;
    if (bytes == 0) return nullptr;  ///< 契约 6：空请求无地址语义（缺陷#2 改约）
    if (bytes > kMaxAlloc || align > kMaxAlloc) abortSizeOverflow();  ///< 缺陷#1
    if (ca::u8* p = allocInChunk(bytes, align)) return p;
    growChunk(checkedAdd(bytes, align));
    ca::u8* p = allocInChunk(bytes, align);
    if (!p) {  ///< 契约 6：growChunk 成功后必有空间，走不到即内部错误
        std::fprintf(stderr, "garena: internal error (bump failed after grow)\n");
        std::abort();
    }
    return p;
}

ca::u8* Arena::allocInChunk(ca::usize bytes, ca::usize align) {
    if (chunk_count_ == 0) return nullptr;
    Chunk& c = chunks_[chunk_count_ - 1];
    ca::usize start = alignUp(reinterpret_cast<ca::usize>(c.data + c.used), align);
    ca::usize end = start + bytes;
    if (end < start || end > reinterpret_cast<ca::usize>(c.data + c.capacity)) return nullptr;
    c.used = end - reinterpret_cast<ca::usize>(c.data);
    return reinterpret_cast<ca::u8*>(start);
}

void Arena::growChunk(ca::usize min_bytes) {
    ca::usize want = next_chunk_bytes_ != 0 ? next_chunk_bytes_ : first_chunk_bytes_;
    if (want < min_bytes) want = min_bytes;
    want = alignUp(want, 16);

    if (chunk_count_ == chunk_capacity_) {
        ca::usize new_cap = chunk_capacity_ == 0 ? 4 : chunk_capacity_ * 2;
        Chunk* nc = static_cast<Chunk*>(std::realloc(chunks_, new_cap * sizeof(Chunk)));
        ca::usize* ns =
            static_cast<ca::usize*>(std::realloc(sorted_, new_cap * sizeof(ca::usize)));
        if (!nc || !ns) {
            std::fprintf(stderr, "garena: out of memory (chunk table)\n");
            std::abort();
        }
        chunks_ = nc;
        sorted_ = ns;
        chunk_capacity_ = new_cap;
    }
    ca::u8* data = static_cast<ca::u8*>(std::malloc(want));
    if (!data) {
        std::fprintf(stderr, "garena: out of memory (chunk, %llu bytes)\n",
                     static_cast<unsigned long long>(want));
        std::abort();
    }
    chunks_[chunk_count_] = Chunk{data, want, 0};
    ///< 新 chunk 按基址插入有序数组（缺陷#8）；chunk 创建是低频事件，插入 O(n)
    ca::usize pos = chunk_count_;
    while (pos > 0 &&
           reinterpret_cast<ca::usize>(chunks_[sorted_[pos - 1]].data) >
               reinterpret_cast<ca::usize>(data)) {
        sorted_[pos] = sorted_[pos - 1];
        --pos;
    }
    sorted_[pos] = chunk_count_;
    ++chunk_count_;
    last_hit_ = kNoChunkIndex;
    next_chunk_bytes_ = want * 2;  ///< 后续 chunk 容量翻倍，摊薄大场景
}

bool Arena::owns(const void* p) const noexcept {
    if (chunk_count_ == 0) return false;
    const ca::usize addr = reinterpret_cast<ca::usize>(p);
    if (last_hit_ != kNoChunkIndex &&
        addrInRange(chunks_[last_hit_].data, chunks_[last_hit_].capacity, addr)) {
        return true;
    }
    ///< 基址有序二分（缺陷#8）：找最后一个 base <= addr 的 chunk
    ca::usize lo = 0;
    ca::usize hi = chunk_count_;
    while (lo < hi) {
        const ca::usize mid = lo + (hi - lo) / 2;
        if (reinterpret_cast<ca::usize>(chunks_[sorted_[mid]].data) <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) return false;
    const ca::usize idx = sorted_[lo - 1];
    if (addrInRange(chunks_[idx].data, chunks_[idx].capacity, addr)) {
        last_hit_ = idx;  ///< chunk 基址终身不变，缓存恒有效
        return true;
    }
    return false;
}

ca::usize Arena::used_bytes() const noexcept {
    ca::usize sum = 0;
    for (ca::usize i = 0; i < chunk_count_; ++i) sum += chunks_[i].used;
    return sum;
}

ca::usize Arena::total_bytes() const noexcept {
    ca::usize sum = 0;
    for (ca::usize i = 0; i < chunk_count_; ++i) sum += chunks_[i].capacity;
    return sum;
}

ca::usize Arena::dtor_registrations() const noexcept { return dtor_count_; }

ca::usize Arena::dtor_overhead_bytes() const noexcept { return dtor_count_ * sizeof(DtorEntry); }

void Arena::registerDestructor(void* obj, ca::usize elem_size, void (*destroy)(void*)) {
    registerArrayDestructor(obj, 1, elem_size, destroy);
}

void Arena::registerArrayDestructor(void* obj, ca::usize count, ca::usize elem_size,
                                    void (*destroy)(void*)) {
    if (dtor_count_ == dtor_capacity_) {
        ca::usize new_cap = dtor_capacity_ == 0 ? 16 : dtor_capacity_ * 2;
        DtorEntry* nd = static_cast<DtorEntry*>(
            std::realloc(dtors_, new_cap * sizeof(DtorEntry)));
        if (!nd) {
            std::fprintf(stderr, "garena: out of memory (dtor table)\n");
            std::abort();
        }
        dtors_ = nd;
        dtor_capacity_ = new_cap;
    }
    dtors_[dtor_count_++] = DtorEntry{obj, destroy, count, elem_size};
}

ca::usize Arena::lastChunkUsed() const noexcept {
    return chunk_count_ != 0 ? chunks_[chunk_count_ - 1].used : 0;
}

void Arena::rollbackBump(ca::usize pre_chunk_count, ca::usize pre_used) noexcept {
    if (chunk_count_ == 0) return;
    if (chunk_count_ > pre_chunk_count) {
        chunks_[chunk_count_ - 1].used = 0;  ///< 快照后新开的块：整块闲置
    } else {
        chunks_[chunk_count_ - 1].used = pre_used;
    }
}

void Arena::reset() noexcept {
#ifndef NDEBUG
    DangleCheck::instance().assertNoneOwnedBy([this](const void* p) { return owns(p); });
#endif
    runDestructors();
    dtor_count_ = 0;
#ifndef NDEBUG
    ///< 缺陷#4：毒化使 reset 后继续使用旧指针大概率立即崩溃（debug 专属，
    ///< release 零开销；只提高崩溃概率，非确定性检查）
    for (ca::usize i = 0; i < chunk_count_; ++i) {
        std::memset(chunks_[i].data, 0xDD, chunks_[i].capacity);
    }
#endif
    for (ca::usize i = 0; i < chunk_count_; ++i) chunks_[i].used = 0;
    next_chunk_bytes_ = 0;  ///< 复用已有 chunk，容量策略从头开始
}

void Arena::runDestructors() noexcept {
    for (ca::usize i = dtor_count_; i > 0; --i) {
        const DtorEntry& e = dtors_[i - 1];
        for (ca::usize k = e.count; k > 0; --k) {
            e.destroy(static_cast<char*>(e.obj) + (k - 1) * e.elem_size);
        }
    }
}

void Arena::freeChunks() noexcept {
    for (ca::usize i = 0; i < chunk_count_; ++i) std::free(chunks_[i].data);
    std::free(chunks_);
    chunks_ = nullptr;
    std::free(sorted_);
    sorted_ = nullptr;
    chunk_count_ = 0;
    chunk_capacity_ = 0;
    last_hit_ = kNoChunkIndex;
    std::free(dtors_);
    dtors_ = nullptr;
    dtor_capacity_ = 0;
    dtor_count_ = 0;
}

}  // namespace garena
