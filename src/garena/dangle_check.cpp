#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "garena/dangle_check.hpp"

namespace garena::detail {

/// stderr 宏在本翻译单元必然存在：本文件是 garena 诊断 I/O 的唯一消费点，
/// stdio 最先包含、不碰 process 族头（subprocess.hpp 会 #undef stderr）。
/// 消息保持 ASCII：死亡测试的 stderr 正则依赖它（跨代码页稳定）。
void dangle_abort(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fflush(stderr);
    abort();
}

}  // namespace garena::detail
