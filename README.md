# libgarena

通用 Arena / 资源生命周期库（v2）：把内存所有权治理的三原则——所有权结构性、
类型可见、可执行检查——做成可复用的库原语。

## 是什么：双层架构

**Layer A —— `garena::Arena`（bump/monotonic）**：chunk 链式分配，整批分配、整批
释放，无逐条 free。凡是「一批对象有共同死亡点」的场景（解析一棵 classfile、跑一个
pass），都应该建在 Arena 上而不是裸 new/delete 配对上。

**Layer B —— `GenerationalArena<T>` + `GHandle{owner, slot, gen}`（生成式句柄池）**：
需要逐个增删、以稳定身份被引用的对象（指令、基本块、Label、缓存项）不适用 bump
arena——用 Layer B：O(1) create/remove、`T*` 地址终身稳定、gen 校验防 ABA/use-after-
free（`remove` 后旧句柄 `try_get` 即 nullptr，`get` abort，release 同样可检）。句柄
语义采用 **owner/slot/gen 三元组方言**：字段顺序与宽度固定（u32×3），
owner 进程级唯一、跨实例使用句柄被结构性拒绝。

字符串 arena 不在本库范围：继续用 `ca::str::Utf8StringArena`（内容去重 intern），
libgarena 只依赖 libca_core，不包装、不平行实现。

选型判据：有共同死亡点 → bump Arena；逐个增删+句柄引用 → GenerationalArena；
确需共享可变所有权 → 引用计数（`ca::str::Utf8StringPool`，本库不提供）。
双层架构契约与缺陷闭环记录见 `doc/design.md`；libjbytecode 六套手动所有权
制度的根因分析亦在该文档开篇。

## 契约要点（v2）

- **OOM 契约**（Arena + GenerationalArena 一致）：底层 malloc/realloc 失败或分配
  尺寸溢出（> `garena::kMaxAlloc`）一律 `std::abort`（带消息），不抛异常、不返回空
  指针；唯一例外 `alloc(0)` / `createArray(0)` 返回 nullptr（空请求无地址语义）。
- **异常契约**：`Arena::createArray` 与 `GenerationalArena::create` 中 T 构造抛出时
  回滚（逆序析构已构造元素 / 槽位 gen+1 重入空闲链）后重抛，不泄漏半构造对象。
- **开销披露**：trivial T 在两个层都零 per-allocation 元数据；non-trivial T 在
  Layer A 每对象（数组按数组）+32B 析构登记（诊断 API
  `dtor_registrations()` / `dtor_overhead_bytes()`）；Layer B 每槽 12B 元数据 +
  空闲链，块存储不归还。
- **线程安全边界**：全部设施**非线程安全**，单线程顺序模型（与全仓单线程模型、
  `Utf8StringArena` 一致）；跨线程请外置同步。
- **reset 语义分层**：Layer A `reset()` 后旧指针不可检（debug 下 0xDD 毒化提高
  崩溃概率）；Layer B `clear()`/`remove()` 后旧句柄**必然**校验失败——需要「死而
  可检」就选 Layer B。

## 怎么建

xmake ≥ 3.0。libca 经 [luiox-repo](https://github.com/luiox/luiox-repo) git
直连包解析，gtest 走 xrepo。首次构建先注册包仓库：

```sh
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
xmake f --with_tests=y
xmake build libgarena_unittest
```

ASan 构建（linux 下 gcc/clang 全链插桩；MSVC 不支持该开关）：

```sh
xmake f --with_tests=y --with_asan=y -p linux
xmake build libgarena_unittest && xmake run libgarena_unittest
```

死亡测试（get 失配 abort、dangle 下溢 abort 等）在 debug 构建才有完整覆盖，
发布前应单跑一遍：

```sh
xmake f -m debug --with_tests=y
xmake build libgarena_unittest && xmake run libgarena_unittest
```

## 怎么跑

```sh
xmake run libgarena_unittest --gtest_brief=1
```

## 文档索引

- `doc/design.md` — 设计契约与红线（v2 双层架构）
- `doc/capabilities.md` — API 一览

## License

MIT
