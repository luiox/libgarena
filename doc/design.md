# libgarena 设计

> 本文自足成文：双层架构契约、冻结 API 与缺陷闭环记录（#1–#12）均在本仓
> doc/ 内，不依赖外部治理文档。

## 动机与定位

libjbytecode 的混乱根因是**所有权从未被设计**：六套互不相同的后补制度（树节点手写
析构循环 / Label 池 / InsnList 摘链不删 / reader 借用 arena / 进程级单例 / clone 图）
全靠纪律维系，类型系统零检查。值语义槽 + 会话保活能解单个库内部的寿命问题，
但那是「一个库内的解」，通用底座仍然缺失：libca 只有字符串专用 arena。

libgarena v2 提供**双层**原语：

- **Layer A `garena::Arena`**：通用 bump arena + 归属断言 + debug 悬垂检查——
  「一批对象有共同死亡点」的归宿；
- **Layer B `GenerationalArena<T>` + `GHandle{owner, slot, gen}`**：O(1) 增删、地址
  稳定、gen 校验防 ABA/UAF 的生成式句柄池——需要逐个增删、以稳定身份被引用的对象
  的归宿。

选型判据：有共同死亡点 → Layer A bump Arena；逐个增删 + 句柄引用 → Layer B
GenerationalArena；确需共享可变所有权 → 引用计数（`ca::str::Utf8StringPool`，
本库不提供）；字符串 arena → `ca::str::Utf8StringArena`（intern 去重），本库
不包装、不平行实现。

### 所有权治理三原则

定位句「把内存所有权治理做成可复用的库原语」由三条原则支撑，本库逐条给出机制化
答案（而非纪律约束）：

1. **所有权结构性**：对象的寿命归属在结构上成立——「共同死亡点」的产物进 Arena，
   逐个增删的对象进句柄池，不存在靠注释维系的第三种隐式约定；
2. **类型可见**：`TypedArena<Node>::create`、`GenerationalArena<Insn>` + `GHandle`
   把「产物寿命 = 池寿命」「身份 = 句柄」写进类型签名，调用点与 review 即可见；
3. **可执行检查**：gen 校验使悬垂/ABA 确定性可检（release 生效）、debug 借用登记
   断言（`GARENA_MARK_BORROWED/RETURNED`）、debug 下 reset 毒化——「不得使用失效
   指针」从口头纪律变成可执行断言。

## 硬契约（Layer A: Arena）

1. **地址稳定**：已分配内存永不挪动，指针在 reset()/析构 前始终有效。
   （`ca::str::Utf8StringArena` 提供的正是同款契约，本库把它推广到任意对象。）
2. **无逐条释放**：单个对象没有 free/delete；non-trivial 析构由 arena 登记，reset()/析构
   时**逆序**统一执行（后构造先析构，与栈语义一致）。
3. **reset 红线**：reset()/析构 后所有旧指针失效。reset 复用已有 chunk 内存（不归还
   malloc），bump 从头开始——「reset 后旧指针恰好又被分配出去」是必须靠纪律不做的事，
   debug 悬垂检查（见下）把这条纪律变成可执行断言。
4. **非线程安全**：单线程顺序模型，与 Utf8StringArena 一致（Layer B 同）。
5. **move 语义**：chunk 所有权整体转移，旧 Arena 变空壳；地址稳定契约不破坏。
6. **分配失败策略（v2 成文，缺陷#6）**：malloc/realloc 失败或请求尺寸溢出
   （> `kMaxAlloc` = SIZE_MAX/2）一律 `std::abort`（带消息）；不抛异常、不返回空
   指针。唯一例外：`alloc(0, align)` 返回 nullptr——**空请求无地址语义**（缺陷#2
   改约，v0.1 的「可用作哨兵」注释作废；`createArray(0)` 同约）。

### 分配策略与开销披露（缺陷#7）

- chunk 链（动态数组持有），默认首块 64KB（对齐 Utf8StringArena）；放不下 → 新
  chunk，容量翻倍（首块 → 2× → 4× …）；分配 = 对齐 bump，O(1)；
- **trivial T：零 per-allocation 元数据**；
- **non-trivial T：每对象（数组按数组）一条 DtorEntry 登记（x64 上 ≈32B）**，reset
  逆序执行——dtor 内可以安全访问同 arena 中**先于自己构造**的对象。诊断 API
  `dtor_registrations()` / `dtor_overhead_bytes()` 可观测；
- `alloc()` 入口做尺寸溢出检查（缺陷#1），`growChunk` 用带进位加法，回绕即 abort；
- `createArray` 构造循环包 rollback guard（缺陷#5）：第 k 个元素抛出时逆序析构前
  k-1 个、回退 bump 指针后重抛，不泄漏半构造数组（约束：元素构造不得再向本 arena
  分配内存，否则回退波及嵌套产物）；
- debug 下 `reset()` 析构后对全部 chunk 填 `0xDD` 毒化（缺陷#4 的 raw 指针面）：使
  「reset 后继续用旧指针」大概率立即崩溃。**检查边界**：毒化只提高崩溃概率，不是
  确定性检查；确定性的「死而可检」在 Layer B（gen 句柄）；
- `owns()` 为基址有序数组 + 二分 + 最近命中缓存（缺陷#8；chunk 按基址插入有序，
  创建是低频事件）。**注意**：`owns()` 仅限诊断/归属判别等低频路径（release 的
  诊断性使用亦合法），不得进入 alloc/create 逐元素分配热路径。

## debug 悬垂检查（dangle_check.hpp）

`GARENA_MARK_BORROWED(p)` / `GARENA_MARK_RETURNED(p)`（NDEBUG 下空操作，零开销）：
消费者把从 arena 借出的视图显式登记/归还；`Arena::reset()`/析构 时若仍有未归还且落在
本 arena chunk 范围内的借用，abort 并报指针。

v2 计数化（缺陷#3）：登记表为 指针 → 借用计数 的 map（v0.1 为集合去重，同一地址
两次借出、一次归还会漏报）。归零才移除；**归还多于借出（下溢）同样 abort**——成对
性本身是被检查对象。

已知局限：登记按指针值；同一地址先归还再借出是干净的（测试覆盖）；「归还后仍持有
副本」无法检测（登记的是意图，不是全部副本）。

## TypedArena<T>

借用 Arena 的类型化便捷层（header-only）：**视图值语义**——拷贝/赋值是浅拷贝，两个
视图指向同一 Arena（缺陷#9 成文）；不拥有 Arena，Arena 必须活得比它久（仅文档约定，
不做运行期 outlive 检查，非目标）。价值在签名——`TypedArena<Node>::create` 让「产物
寿命 = 整池寿命」类型可见。

## 硬契约（Layer B: GenerationalArena<T> / GHandle）

1. **句柄**：`GHandle{owner: u32, slot: u32, gen: u32}`，12 字节 trivially copyable。
   `owner` 是实例进程级唯一代号（自增分配，从 1 起，0 留给 kNoHandle/被移动壳）；
   `remove`/`clear`/构造失败 均使 gen+1，旧句柄立即失效，**不存在 ABA**。gen u32
   回绕需单槽 2^32 次删除，实践不可达，为记录在案的接受界。
2. **地址稳定**：T 存储在块链（每块 block_slots 个 T，默认 1024；块 malloc 后永不
   搬动/归还）；槽位元数据在旁路 vector，地址不对外（每槽 12B：gen/next_free/alive，
   x64 含填充；存储保留不归还，与 Layer A chunk 复用同哲学）。`T*` 在 remove(该槽)
   或池析构 前恒定——IR 节点互指的前提。
3. **O(1) 增删**：create 走空闲链或追加；remove 校验后原地析构 T、gen+1、槽入空闲链。
4. **校验分级**：`try_get`/`alive` 全构建校验，slot/gen 失配返回 nullptr（预期失败
   路径）；`get` 失配 **abort**（打印期望/实际 owner-slot-gen，**release 也启用**），
   确定性失败优于 UB——use-after-free 在 release 即可检。**不提供 operator[]**。
   **owner 失配（跨实例使用句柄）结构性拒绝**：debug 一律 abort（try_get/get/
   alive/remove 都是「使用」）；release 下 try_get/alive 返回 false/nullptr、
   get abort、remove 返回 false。
5. **clear()**：全部置死 + 每槽 gen+1，保留存储。与 Layer A reset 的关键差异：clear
   后旧句柄**必然**校验失败，而 reset 后旧指针不可检。
6. **异常安全**：create 中 T 构造抛出 → 槽置死、gen+1、重入空闲链后重抛，不发布
   半构造对象。系统级分配失败（块/元数据 malloc）与 Layer A 同约：abort，不抛
   bad_alloc——create 的唯一异常源是 T 的构造函数。
7. **move**：move-only；移动转移块与元数据，owner 不变；被移动壳 owner 归零。
8. **与 dangle_check 的关系**：Layer B 是「裸指针借出后对象死亡」的类型层解；仍以
   `T*` 短暂借出的场景沿用登记宏，`remove()`/析构 对该槽存储范围做未归还断言
   （debug，与 Arena::reset 同规）。

## 句柄方言

GHandle 采用业界常见的 generation handle 方言：字段顺序与宽度固定（u32×3、
12B trivially copyable）、owner 进程级唯一、删除即 gen+1、跨实例结构性拒绝。
上层字节码 IR 若采用同方言定义自己的句柄，可无损映射到 GHandle 域。

## 与相邻设施的关系

| 设施 | 关系 |
|---|---|
| `ca::str::Utf8StringArena` | 字符串专用（intern 去重），继续直用；本库不做字符串 |
| `ca::str::Utf8StringPool` | 无共同死亡点时的选择（引用计数），互斥选型；**保留 libca 侧设施、不在本库推广**（当前 0 采用） |
| 引用计数池（`ca::str::Utf8StringPool`） | 无共同死亡点时的互斥选型；本库不提供，需要时直用 libca |

## 版本注记

- v0.1：Arena + TypedArena + dangle_check（17 测试）。
- v2（2026-09-09，本文）：Layer A 缺陷闭环（#1–#12）+ Layer B
  GenerationalArena/GHandle 新增。public 头不再注入 `using ca::usize`（缺陷#10，
  未保留过渡别名，调用方显式写 `ca::usize`）；悬空文档引用修正（缺陷#11）。

## 验证记录

- 2026-09-09（v2）：libgarena_unittest × 三构建全绿——
  - release（`xmake f -m release --with_tests=y`）：43 用例；
  - debug（`xmake f -m debug --with_tests=y`）：46 用例（含 debug 专属的 3 条
    计数化悬垂用例；死亡测试全部实跑：get 失配 abort、跨实例 try_get/get 的
    owner 拒绝、dangle 下溢 abort、remove 未归还借用 abort、分配溢出 abort、
    reset 0xDD 毒化）；跑毕已切回默认 release 配置；
  - ASan（`xmake f --with_tests=y --with_asan=y`，全局插桩）：43 用例零报告
    （linux/gcc、clang 路径；MSVC ASan 不做泄漏检测）。
- 2026-10-11（质量审查）：注释/文档与实现对齐（createArray 值初始化语义、
  `owns()` 使用口径、悬空文档引用）+ 测试补缺（`GenerationalArena::owns`、
  createArray 值初始化 pin、`TypedArena::createArray`）+ ensureSlots 槽位 u32
  域防御检查——windows/MSVC 双构建实跑：release 46 用例、debug 49 用例全绿，
  死亡测试全部实跑。
- 2026-10-11（测试 skip 审计）：两配置全量实跑——release 46 / debug 49 全绿，
  输出无 [ SKIPPED ]、无 disabled 计数；用例数差恒为 3 = debug 专属的计数化
  悬垂用例（`#ifndef NDEBUG`：部分归还检出、全归还干净、下溢 abort）。死亡
  测试分级实跑：分配溢出 / get 失配 / 跨实例 get 为全构建；跨实例 try_get
  release 断言 nullptr、debug 死亡测试；dangle 借用类与 reset 毒化仅 debug
  实跑（release 分支 GTEST_SUCCEED 为既定配置门控，非缺陷掩盖）。排查结论：
  unittest 5 文件全部收录于 xmake.lua（无孤儿）；try/catch 均带 try 内
  FAIL、循环断言定长或前置 ASSERT 守卫、无提前 return——无空跑掩盖缺陷；
  `#ifdef _WIN32` 仅切换死亡判定（MSVC 退出码 3 / POSIX SIGABRT），不移除
  测试。CI（ci.yml）按默认 release 口径跑双平台，debug 为 README 约定的
  发布前人工步骤。
