# ─────────────────────────────────────────────────────────────────────────────
# CompilerFlags.cmake
#
# 跨平台编译标志：MSVC 2022 / GCC 13+ / Clang 17+
# 目标 C++ 标准：C++17（uvw 最低要求）
#
# 设计原则：
#   - 只有 -Wall / -W4 等基础诊断标志才全局设置（apply_compile_options）
#   - -Werror / /WX / -Wpedantic / -Wextra 等严格标志放入
#     simbehavior_compile_warnings
#     INTERFACE 库，由我们自己的目标主动 link_libraries，不污染三方依赖
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SIMBEHAVIOR_WARNINGS_AS_ERRORS
  "Treat warnings as errors for sim-behavior targets"
  OFF)

# ── linb::any 类型比较修复 ─────────────────────────────────────────────────────
# macOS + -fvisibility=hidden 下，std::type_info::operator== 对 hidden visibility
# 的弱符号（weak private external）可能返回 false，即使 &a == &b 且名称相同。
# ANY_IMPL_FAST_TYPE_INFO_COMPARE 将比较改为地址比较（&a == &b），完全绕过该问题。
# 全局定义确保 BT.CPP 内部的 linb::any 与我们所有 TU 使用同一语义。
add_compile_definitions(ANY_IMPL_FAST_TYPE_INFO_COMPARE)

# ── 全局最低诊断（三方库也可以接受）────────────────────────────────────────
if(MSVC)
  add_compile_options(
    /utf-8       # 源文件 UTF-8（全局安全）
    /MP          # 并行编译
    /Zc:__cplusplus  # 正确暴露 __cplusplus 宏
  )
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  add_compile_options(-Wall)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_options(-O2)
  endif()
endif()

# 注意：-fvisibility=hidden 不放入全局，而是仅作用于我们自己的目标（见下方 INTERFACE 目标）。
# 全局设置 visibility=hidden 会隐藏第三方共享库（如 libbehaviortree_cpp.dylib）
# 中我们依赖的符号，导致链接时 "undefined symbols"。

# ── AddressSanitizer (可选) ───────────────────────────────────────────────────
option(SIMBEHAVIOR_ENABLE_ASAN "Enable AddressSanitizer (Debug builds)" OFF)
if(SIMBEHAVIOR_ENABLE_ASAN AND NOT MSVC)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

# ── simbehavior_compile_warnings：严格警告 INTERFACE 目标 ───────────────────
# 仅由我们自己的目标通过 target_link_libraries(... PRIVATE simbehavior_compile_warnings)
# 引入，不影响 TBB / libuv / uvw / BehaviorTree.CPP 等三方库
add_library(simbehavior_compile_warnings INTERFACE)

if(MSVC)
  target_compile_options(simbehavior_compile_warnings INTERFACE
    /W4          # 较高警告级别
    /permissive- # 严格标准一致性
  )
  if(SIMBEHAVIOR_WARNINGS_AS_ERRORS)
    target_compile_options(simbehavior_compile_warnings INTERFACE
      /WX        # 警告视为错误
    )
  endif()
else()
  target_compile_options(simbehavior_compile_warnings INTERFACE
    -Wextra
    -Wpedantic
    -Wno-unused-parameter         # BT 节点经常有未使用参数
    -fvisibility=hidden           # 隐藏非 API 符号（仅本模块生效，不污染三方库）
    # 注意：故意不加 -fvisibility-inlines-hidden。
    # 该标志使所有内联模板函数（包括 linb::any 的 vtable_for_type<T>()）获得
    # hidden visibility，导致链接器无法跨 TU 去重其内部静态局部变量。
    # 结果：BT::Blackboard 在一个 TU（bt_runtime_impl）存入 SyncNodeContextPtr，
    # 在另一个 TU（condition_base/async_action_base）取出时，linb::any 的类型比较
    # 因 vtable 副本不同而失败，上下文始终为 null，所有条件节点返回 FAILURE。
  )
  if(SIMBEHAVIOR_WARNINGS_AS_ERRORS)
    target_compile_options(simbehavior_compile_warnings INTERFACE
      -Werror
    )
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(simbehavior_compile_warnings INTERFACE
      -Wno-gnu-zero-variadic-macro-arguments
    )
  endif()
endif()
