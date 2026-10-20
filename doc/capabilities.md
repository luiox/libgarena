# libgarena 能力一览（v2）

`#include "garena/arena.hpp"` / `"garena/generation.hpp"` / `"garena/typed_arena.hpp"` / `"garena/dangle_check.hpp"`

通用契约（两层的分配/释放失败策略一致）：malloc/realloc 失败或尺寸溢出（> `kMaxAlloc`）
一律 `std::abort`（带消息），不抛异常、不返回空；`alloc(0)` / `createArray(0)` 返回
nullptr（空请求无地址语义）。**全部设施非线程安全**（单线程顺序模型）。

## garena::Arena（src/garena/arena.hpp，Layer A: bump/monotonic）

| API | 说明 |
|---|---|
| `Arena()` / `Arena(usize first_chunk_bytes)` | 构造，默认首块 64KB |
| `Arena(Arena&&)` / `operator=(Arena&&)` | move-only，chunk 所有权整体转移 |
| `void* alloc(usize bytes, usize align)` | 对齐 bump 分配，地址终身稳定；`bytes==0` → nullptr；溢出 → abort |
| `T* create<T>(args...)` | bump 构造单对象；non-trivial 析构由 arena 接管 |
| `T* createArray<T>(usize count)` | bump 构造值初始化数组（标量元素零初始化）；构造抛出 → 逆序析构+回退 bump 后重抛 |
| `bool owns(const void* p)` | 指针是否落在本 arena chunk 内（基址有序二分+缓存；仅限低频诊断/归属判别路径，不入分配热路径） |
| `usize used_bytes()` / `usize total_bytes()` | 已用 / chunk 总容量 |
| `usize dtor_registrations()` | 当前 dtor 表条数（诊断） |
| `usize dtor_overhead_bytes()` | dtor 表占用字节（披露：trivial 零元数据；non-trivial 每对象/每数组 ≈32B @x64） |
| `void reset()` | 逆序析构 non-trivial 对象 + 复用 chunk；旧指针全失效；debug 下 0xDD 毒化 + 未归还借用断言 |
| `inline constexpr usize kMaxAlloc` | 单次分配尺寸上限（SIZE_MAX/2），超过即溢出 abort |

## garena::GenerationalArena<T> + GHandle（src/garena/generation.hpp，Layer B）

句柄采用 owner/slot/gen 三元组方言（u32×3，12B trivially copyable）。

| API | 说明 |
|---|---|
| `GenerationalArena()` / `explicit GenerationalArena(usize block_slots)` | 构造，默认每块 1024 槽；owner 进程级唯一自增 |
| `Handle create(args...)` | 就地构造并发布（空闲链复用或追加）；构造抛出 → 槽 gen+1 重入空闲链后重抛 |
| `T* try_get(Handle)` / `const T*` | 全构建校验；slot/gen 失配 → nullptr；owner 失配 → release nullptr / **debug abort** |
| `T& get(Handle)` / `const T&` | 同校验；失配 → **abort**（打印期望/实际 owner-slot-gen），release 也启用 |
| `bool alive(Handle)` | 句柄当前是否指向活元素（全构建校验；kNoHandle 恒 false） |
| `bool remove(Handle)` | O(1) 删除：析构 T、gen+1、槽入空闲链；slot/gen 失配返回 false，owner 失配 release false / **debug abort**；debug 下槽内有未归还借用 → abort |
| `void clear()` | 全部活槽置死并 gen+1，空闲链重置为全槽，保留存储；旧句柄此后必然失配 |
| `usize size()` / `usize capacity()` | 活元素数 / 总槽位（含死槽） |
| `void reserve(usize n)` | 预扩槽位与块（只扩不缩） |
| `void for_each(F)` / const | 遍历活元素（槽序）：`f(T&, GHandle)` |
| `bool owns(const void* p)` | 指针是否落在池某块存储内（调试用） |
| `u32 owner_id()` | 实例代号（诊断/日志） |

| GHandle | 说明 |
|---|---|
| `u32 owner, slot, gen` | owner=实例代号（跨实例使用句柄结构性拒绝）；slot=槽位；gen=槽代数（remove/clear/构造失败 均 +1，无 ABA） |
| `explicit operator bool` | `slot != kInvalidSlot` |
| `operator== / operator!=` | 三元组全等比较 |
| `kNoHandle` | `{0, kInvalidSlot, 0}` 空句柄 |

无 `operator[]`（防绕过校验）。move-only；move 后目的实例 owner 不变、被移动壳归零。
gen 为 u32 不处理回绕（单槽 2^32 次删除实践不可达，记录在案的接受界）。

## garena::TypedArena<T>（src/garena/typed_arena.hpp，header-only）

| API | 说明 |
|---|---|
| `explicit TypedArena(Arena&)` | 借用，不拥有；**视图值语义**（拷贝的两个视图指向同一 arena）；Arena 须活得比它久（文档约定，不做运行期检查） |
| `create<T>(args...)` / `createArray<T>(count)` | 转发 Arena，签名类型可见 |
| `owns(p)` / `arena()` | 归属查询 / 底层 Arena 访问 |

## 悬垂检查宏（src/garena/dangle_check.hpp，仅 debug；NDEBUG 下零开销）

| API | 说明 |
|---|---|
| `GARENA_MARK_BORROWED(p)` | 登记借出视图（计数 +1；同一指针可多次） |
| `GARENA_MARK_RETURNED(p)` | 归还（计数 -1，归零移除）；下溢（无配对借出）→ abort；Arena::reset/析构、GenerationalArena::remove/析构 时未归还且指向本池 → abort |
