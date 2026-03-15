# ─────────────────────────────────────────────────────────────────────────────
# CompilerFlags.cmake
#
# 跨平台编译标志：MSVC 2022 / GCC 13+ / Clang 17+
# 目标 C++ 标准：C++17（uvw 最低要求；推荐升级至 C++20）
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(
    /W4          # 较高警告级别（不用 /Wall，避免 STL 噪音）
    /WX          # 警告视为错误
    /permissive- # 严格标准一致性
    /utf-8       # 源文件 UTF-8
    /MP          # 并行编译
    /Zc:__cplusplus  # 正确暴露 __cplusplus 宏
  )
  # MSVC 多线程运行时：Release 用 /MT，Debug 用 /MTd
  # 与 vcpkg 默认 triplet (x64-windows-static) 对齐
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wno-unused-parameter  # BT 节点经常有未使用参数
  )
  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_options(-O2)
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-gnu-zero-variadic-macro-arguments)
  endif()
endif()

# ── 跨平台符号可见性 ──────────────────────────────────────────────────────────
if(NOT MSVC)
  add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden)
endif()

# ── AddressSanitizer (Debug 可选) ─────────────────────────────────────────────
option(SIMBEHAVIOR_ENABLE_ASAN "Enable AddressSanitizer (Debug builds)" OFF)
if(SIMBEHAVIOR_ENABLE_ASAN AND NOT MSVC)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()
