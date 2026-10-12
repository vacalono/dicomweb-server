// Buffer 为 header-only 实现（见 include/net/Buffer.h）。
// 此翻译单元存在是为了让 CMake 源列表保持稳定，并显式实例化编译检查。
#include "net/Buffer.h"

namespace dws {
namespace {
// 编译期检查：确保 Buffer 可正常实例化
[[maybe_unused]] Buffer* kCompileCheck = nullptr;
}  // namespace
}  // namespace dws
