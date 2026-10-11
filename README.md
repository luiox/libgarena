# libgarena

C++17 Arena 分配器与生成式句柄池：bump 整批分配、O(1) 增删、gen 校验防悬垂/ABA（xmake）。

## 选型：你需要哪一层？

手动内存管理的三种典型需求，各有一个对应解：

- 一批对象有**共同死亡点**（解析一棵 classfile、跑一个 pass）→ **Layer A：`garena::Arena`**（bump/monotonic）
- 对象需要**逐个增删**、以稳定身份被引用（指令、基本块、Label、缓存项）→ **Layer B：`GenerationalArena<T>` + `GHandle`**（生成式句柄池）
- 确需**共享可变所有权** → 引用计数（直用 `ca::str::Utf8StringPool`，本库不提供）；字符串 arena 同理直用 `ca::str::Utf8StringArena`（intern 去重），本库不包装

## 双层架构

**Layer A —— `garena::Arena`**：chunk 链式 bump 分配，整批分配、整批释放，无逐条
free；non-trivial 析构由 arena 登记并在 reset()/析构 时逆序统一执行；`owns(p)`
归属判别供诊断与断言使用。

**Layer B —— `GenerationalArena<T>`**：O(1) create/remove、`T*` 地址终身稳定；
句柄 `GHandle{owner, slot, gen}` 为 12 字节 trivially copyable。remove/clear 即
gen+1，旧句柄立即失效——`try_get` 返回 nullptr、`get` 确定性 abort（release 同样
生效），悬垂与 ABA 从「靠纪律」变成「可执行检查」。

```cpp
// Layer A：一批对象有共同死亡点 → 整池生死
garena::Arena arena;
auto* ast  = arena.create<Expr>(opcode);      // 构造参数完美转发
auto* toks = arena.createArray<Token>(n);     // 值初始化数组（标量零初始化）
// …… 产物随池一次性释放；reset 后旧指针全部失效（debug 下毒化 + 悬垂断言）
arena.reset();

// Layer B：逐个增删、以句柄为稳定身份
garena::GenerationalArena<Insn> insns;
garena::GHandle h = insns.create(opcode);     // 就地构造，O(1)
if (Insn* p = insns.try_get(h)) { /* 校验取值：失配 → nullptr */ }
insns.remove(h);                              // O(1) 删除：gen+1，旧句柄立即失效
assert(!insns.alive(h));                      // 死而可检：旧句柄必然校验失败
```

## 快速开始

依赖：xmake ≥ 3.0；[libca](https://github.com/luiox/libca)（经
[luiox-repo](https://github.com/luiox/luiox-repo) git 直连包解析，仅 core 模块）；
gtest 走 xrepo（仅测试）。

```sh
git clone https://github.com/luiox/libgarena.git
cd libgarena
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git  # 首次：注册依赖包仓库
xmake f --with_tests=y
xmake build libgarena_unittest
xmake run libgarena_unittest --gtest_brief=1
```

作为静态库接入你的工程（submodule / 子目录方式）：

```lua
-- 你的 xmake.lua：依赖包仓库注册与 libca 版本须与 libgarena 一致
add_requires("libca 0.0.15", {configs = {modules = "core"}})
includes("libs/libgarena")  -- 本仓路径

target("myapp")
    set_kind("binary")
    add_deps("libgarena")
    add_packages("libca")
```

## 构建与测试

| 配置 | 命令 | 说明 |
|---|---|---|
| 默认（release） | `xmake f --with_tests=y` | 日常构建 |
| debug | `xmake f -m debug --with_tests=y` | 死亡测试（abort 路径）与悬垂断言全量覆盖，发布前必跑 |
| ASan | `xmake f --with_tests=y --with_asan=y -p linux` | linux gcc/clang 全链插桩；MSVC 不支持该开关 |

```sh
xmake build libgarena libgarena_unittest
xmake run libgarena_unittest
```

## 契约要点

- **OOM**：malloc/realloc 失败或分配尺寸溢出（> `garena::kMaxAlloc` = SIZE_MAX/2）
  一律 `std::abort`（带消息），不抛异常、不返回空；唯一例外 `alloc(0)` /
  `createArray(0)` 返回 nullptr。
- **异常安全**：`createArray` / `GenerationalArena::create` 中 T 构造抛出 → 回滚
  （逆序析构已构造元素 / 槽位 gen+1 重入空闲链）后重抛，不泄漏半构造对象；create
  的唯一异常源是 T 的构造函数。
- **句柄校验分级**：`try_get`/`alive` 全构建返回 nullptr/false（热路径可用）；
  `get` 失配确定性 abort（release 也启用）；跨实例使用句柄被结构性拒绝（debug
  一律 abort）；不提供 `operator[]`。
- **开销披露**：trivial T 零 per-allocation 元数据；non-trivial T 在 Layer A 每对象
  （数组按数组）一条 ≈32B 析构登记（`dtor_registrations()` /
  `dtor_overhead_bytes()` 可观测）；Layer B 每槽 12B 元数据，块存储不归还。
- **线程安全**：全部设施非线程安全（单线程顺序模型），跨线程请外置同步。
- **失效语义分层**：Layer A `reset()` 后旧指针不可检（debug 下 0xDD 毒化提高崩溃
  概率）；Layer B `clear()`/`remove()` 后旧句柄必然校验失败——需要「死而可检」选
  Layer B。
- **debug 悬垂检查**：`GARENA_MARK_BORROWED(p)` / `GARENA_MARK_RETURNED(p)`
  计数化登记借出视图；reset/remove 时该范围内有未归还借用即 abort（NDEBUG 下零开销）。

## 状态与 License

- 状态：v2，public API 已冻结（`GHandle` 12B 布局由 `static_assert` 钉死）；gen 的
  u32 回绕为记录在案的接受界；CI 覆盖 Linux + Windows 双平台。
- License：MIT（见 [LICENSE](LICENSE)）。

## 深入阅读

- [doc/design.md](doc/design.md) —— 设计契约与取舍：所有权治理三原则、双层架构
  硬契约全文、句柄方言细节、libjbytecode 六套所有权制度的根因分析、缺陷闭环记录
  （#1–#12）与验证记录。README 未展开的深层内容以它为准。
- [doc/capabilities.md](doc/capabilities.md) —— API 一览（签名级逐条说明）。
- 头文件 Doxygen 注释是 API 使用方式的唯一事实来源。
