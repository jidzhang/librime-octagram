// 静态链接工具时 boost::throw_exception 无实现；rime.dll 路径由 dll 导出，
// 工具路径需要本桩。
#include <exception>

namespace boost {

void throw_exception(std::exception const& e) {
    throw e;
}

}  // namespace boost
