-- libgarena：通用 Arena / 资源生命周期库（独立构建根）。
--
-- 依赖：
--   - libca（luiox-repo git 直连包）：core 为运行依赖；版本锁固定 0.0.16，
--     升级须显式换版。
--   - gtest（xrepo，仅 --with_tests=y）。
--
-- 仓库注册：本文件不声明 add_repositories——包沙箱会继承宿主的仓库注册，
-- 在工程内重复注册同名仓库会导致嵌套包构建时 remote 路径错乱。独立开发/CI
-- 先执行：
--   xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git

set_languages("cxx17")
add_rules("mode.debug", "mode.release")

add_requires("libca 0.0.16", {configs = {modules = "core"}})

option("with_tests")
    set_default(false)
    set_showmenu(true)
    set_description("Enable unit tests (pulls gtest)")
option_end()

option("with_asan")
    set_default(false)
    set_showmenu(true)
    set_description("Enable AddressSanitizer instrumentation (gcc/clang)")
option_end()

if has_config("with_tests") then
    add_requires("gtest")
end

-- ASan 要同时作用于 libgarena 与测试：只给 unittest 加 flag 时静态库仍是未插桩代码
if has_config("with_asan") and not is_plat("windows") then
    add_cxflags("-fsanitize=address -fno-omit-frame-pointer", {force = true})
    add_ldflags("-fsanitize=address", {force = true})
end

target("libgarena")
    set_kind("static")
    add_packages("libca")
    add_files("src/garena/*.cpp")
    add_headerfiles("src/(garena/**.hpp)")
    add_includedirs("src", {public = true})
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

if has_config("with_tests") then

target("libgarena_unittest")
    set_kind("binary")
    set_default(false)
    add_deps("libgarena")
    add_packages("libca")
    add_packages("gtest")
    add_files("unittest/main.cpp")
    add_files("unittest/arena_test.cpp")
    add_files("unittest/typed_arena_test.cpp")
    add_files("unittest/dangle_test.cpp")
    add_files("unittest/generation_test.cpp")
    add_includedirs("src")
    if is_plat("linux") then
        add_syslinks("pthread")
    end
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

end
