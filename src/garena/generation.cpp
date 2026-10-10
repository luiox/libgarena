#include "garena/generation.hpp"

#include <atomic>

namespace garena {

ca::u32 next_owner_id() noexcept {
    ///< 从 1 起：0 保留给 kNoHandle / 被移动壳，保证其句柄结构性失配。
    static std::atomic<ca::u32> s_next{1};
    return s_next.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace garena
