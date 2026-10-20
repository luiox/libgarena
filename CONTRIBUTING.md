# Contributing

libgarena 是 C++17 的通用内存生命周期库：Layer A `garena::Arena` 做 bump 整批分配与整批释放，Layer B `GenerationalArena<T>` + `GHandle` 做 O(1) 增删与 gen 校验防悬垂。本库只依赖 libca 的 core 模块，不解析外部输入。

## 本库的范围

改动属于本库，如果它服务于「共同死亡点的对象整批释放」或「逐个增删、以稳定身份被引用的对象」。

以下方向不属于本库：字符串驻留（用 libca 的 `ca::str::Utf8StringArena`）、引用计数（用 `ca::str::Utf8StringPool`）、线程安全改造（本库是单线程顺序模型，跨线程由调用方外置同步）。

## 开始之前

非平凡改动先开 issue：API 形状、新增设施、契约变更。小修复和文档修正可以直接提 PR。

改变既有契约（校验失败时的 abort/false 分级、异常安全保证、内存开销）时，先改 `doc/design.md` 的硬契约，再改实现和测试，三者保持同一提交内一致。

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
- **debug 与 release 的行为差异必须成文。** 只在 debug 生效的检查（借用登记、0xDD 毒化）要在头文件注释与 `doc/design.md` 中写明 release 的边界。
- **头文件注释是 API 的唯一事实来源。** `doc/` 记录设计与取舍，不维护接口清单。

## 风格

- 跟随周边文件：函数/变量 `snake_case`、类型 `CamelCase`、常量 `UPPER_SNAKE`、4 空格缩进、K&R 花括号、单行不超过 120 列。
- 注释用中文，解释「为什么」而不是复述代码；公开接口的 Doxygen 注释写清参数、返回值与失败行为。

## 提交信息

一个提交一件事，写可观察的行为变化与契约影响；文件清单由 diff 给出，不写进提交信息。

## License

贡献按仓库的 MIT License 授权（见 [LICENSE](LICENSE)）。
