# Contributing

libgarena 是 C++17 的通用内存生命周期库：Layer A `garena::Arena` 做 bump 整批分配与整批释放，Layer B `GenerationalArena<T>` + `GHandle` 做 O(1) 增删与 gen 校验防悬垂。本库只依赖 libca 的 core 模块，不解析外部输入。

## 本库的范围

改动属于本库，如果它服务于「共同死亡点的对象整批释放」或「逐个增删、以稳定身份被引用的对象」。

以下方向不属于本库：字符串驻留（用 libca 的 `ca::str::Utf8StringArena`）、引用计数（用 `ca::str::Utf8StringPool`）、线程安全改造（本库是单线程顺序模型，跨线程由调用方外置同步）。

## 开始之前

非平凡改动先开 issue：API 形状、新增设施、契约变更。小修复和文档修正可以直接提 PR。

改变既有契约（校验失败时的 abort/false 分级、异常安全保证、内存开销）时，先改 `doc/design.rst` 的硬契约，再改实现和测试，三者保持同一提交内一致。

## 构建与测试

注册依赖包仓库（只需一次）：

```sh
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
```

release 构建与测试：

```sh
xmake f --with_tests=y
xmake build libgarena_unittest
xmake run libgarena_unittest
```

提交前再跑 debug：死亡测试与借用断言只在 debug 生效，release 下这些分支是空操作。

```sh
xmake f -m debug --with_tests=y
xmake run libgarena_unittest
```

CI 跑 Linux 与 Windows 全量用例；平台与配置的当前口径以 `.github/workflows/ci.yml` 为准，debug 配置以本地跑通为准。

## 硬规则

- **测试必须先失败。** 新增用例在没有修复时应当失败，修复后通过；只证明「现在能跑」的用例不收录。
- **不得为了过测试放宽断言。** 断言确实有误时单独提交修正，并说明原断言错在哪。
- **分配失败一律 abort。** malloc/realloc 失败与尺寸溢出不抛 `bad_alloc`、不返回空指针；新增分配路径沿用这一契约。
- **debug 与 release 的行为差异必须成文。** 只在 debug 生效的检查（借用登记、0xDD 毒化）要在头文件注释与 `doc/design.rst` 中写明 release 的边界。
- **头文件注释是 API 的唯一事实来源。** `doc/` 记录设计与取舍，不维护接口清单。

## 风格

- 跟随周边文件：函数/变量 `snake_case`、类型 `CamelCase`、常量 `UPPER_SNAKE`、4 空格缩进、K&R 花括号、单行不超过 120 列。
- 注释用中文，解释「为什么」而不是复述代码；公开接口的 Doxygen 注释写清参数、返回值与失败行为。

## 提交信息

一个提交一件事，写可观察的行为变化与契约影响；文件清单由 diff 给出，不写进提交信息。

## License

贡献按仓库的 MIT License 授权（见 [LICENSE](LICENSE)）。

## 文档规范

三层按**权威来源**分工，扩展名即角色：

| 载体 | 层 | 写什么 | 谁能引用它 |
|---|---|---|---|
| `///` Doxygen | 微观 | 单个符号的契约：参数、返回、失败模式、不变量；单模块的「背景/定位」（`@defgroup` 组注） | 随代码移动 |
| `doc/*.rst` | 宏观 | 跨模块设计、硬契约、能力清单 | 代码/测试/构建脚本 |
| `*.md` | 人 + AI 方向 | README、CONTRIBUTING 等流程文档 | 只有人和 AI 读 |

理由：markdown 撑不住代码耦合的规格——无 label、无校验引用面，代码改动后引用静默腐烂。RST 白送可校验的引用面，规范才谈得上执行。

硬规则（`doc/check_refs.py` + Sphinx `-W` 在 CI 挡）：

1. 代码/测试/构建脚本只可引用 `doc/*.rst`，路径必须存在；出现 `.md` 设计引用即失败（流程文档白名单除外）。
2. 引用定位用 label，形式 `doc:<label>`；被引用的位置必须有 `.. _<label>:` 声明（不必每节都放），命名 `<doc-slug>[-<section-slug>]`。跨文档的 `§N` 一律换 `:ref:`，编号会随插入章节漂移。
3. RST 里「`X` 头注」指向微观层：`X` 必须是某个 `@defgroup` 组名或头文件名。
4. RST 站点以 `sphinx-build -b html -E -W --keep-going doc doc/_build` 构建，警告即失败；本地验证先 `pip install "sphinx==9.1.1"`（与 CI 钉同版）。
5. 微观层以 `doxygen doc/Doxyfile` 构建，`WARN_AS_ERROR` 生效；`///` 注释是 API 契约的唯一来源，与代码同仓同改。

RST 写作约定（中文正文的坑）：表格一律 `.. list-table::`（禁 grid table，全角字形会破坏边框校验）；行内代码紧邻中文或全角开括号时在闭定界符后补转义（docutils 要求 end-string 后接空白或 ASCII 标点）；标题层级 `=` / `-` / `^`，下划线按东亚列宽计长（全角算 2 列）；跨仓出处带仓名标记（libjbytecode / libca），带标记视为 prose 不校验存在性。
