#include <gtest/gtest.h>

#include <string>

#include "garena/typed_arena.hpp"

namespace {

struct Node {
    int opcode;
    std::string note;
    Node(int op, std::string n) : opcode(op), note(std::move(n)) {}
};

}  // namespace

TEST(TypedArenaTest, CreateReturnsArenaOwnedPointer) {
    garena::Arena arena;
    garena::TypedArena<Node> ta(arena);
    Node* n = ta.create(0x1a, "load");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->opcode, 0x1a);
    EXPECT_EQ(n->note, "load");
    EXPECT_TRUE(ta.owns(n));
    EXPECT_TRUE(arena.owns(n));
}

TEST(TypedArenaTest, ProductsShareArenaLifetime) {
    garena::Arena arena;
    {
        garena::TypedArena<Node> ta(arena);
        ta.create(1, "a");
        ta.create(2, "b");
    }
    // TypedArena 只是借用视图，arena 仍拥有全部产物
    EXPECT_GT(arena.used_bytes(), 0);
}

TEST(TypedArenaTest, ArenaAccessor) {
    garena::Arena arena;
    garena::TypedArena<Node> ta(arena);
    EXPECT_EQ(&ta.arena(), &arena);
}

TEST(TypedArenaTest, CreateArrayForwardsAndValueInitializes) {
    garena::Arena arena;
    garena::TypedArena<int> ta(arena);
    int* arr = ta.createArray(4);
    ASSERT_NE(arr, nullptr);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(arr[i], 0) << "elem " << i;  // 值初始化语义与 Arena::createArray 一致
    }
    EXPECT_TRUE(ta.owns(arr));
    EXPECT_EQ(arena.used_bytes(), 4 * sizeof(int));
}
