/// @file typed_arena.hpp
/// TypedArena<T>：绑定 Arena 的类型化便捷层（header-only，零额外开销）。
///
/// 语义（v2 成文，缺陷#9）：
///  - **视图值语义**：拷贝/赋值是浅拷贝——两个视图指向**同一个** Arena，
///    代价是一个指针拷贝；不存在深拷贝语义。
///  - 不拥有 Arena —— Arena 必须活得比 TypedArena 久（借用，编译期不可查，
///    本库也不做运行期 outlive 检查，属非目标；debug 下可配合
///    GARENA_MARK_BORROWED 登记）。
///  - 价值在签名：`TypedArena<Node>::create` 让「产物寿命 = 整池寿命」类型可见。

#pragma once

#include <utility>

#include <libca/core/datatype.hpp>

#include "garena/arena.hpp"

namespace garena {

template <class T>
class TypedArena {
public:
    explicit TypedArena(Arena& arena) noexcept : arena_(&arena) {}

    TypedArena(const TypedArena&) noexcept = default;
    TypedArena& operator=(const TypedArena&) noexcept = default;

    /// 等价于 arena.create<T>(...)；签名上把「产物寿命 = 整池寿命」类型可见化。
    template <class... A>
    T* create(A&&... args) {
        return arena_->create<T>(std::forward<A>(args)...);
    }

    T* createArray(ca::usize count) { return arena_->createArray<T>(count); }

    bool owns(const void* p) const noexcept { return arena_->owns(p); }

    Arena& arena() const noexcept { return *arena_; }

private:
    Arena* arena_;
};

}  // namespace garena
