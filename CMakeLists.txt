cmake_minimum_required(VERSION 3.22)
project(sim_behavior VERSION 0.1.0 LANGUAGES CXX)

# ── 选项 ──────────────────────────────────────────────────────────────────────
option(SIMBEHAVIOR_BUILD_TESTS     "Build unit tests"         ON)
option(SIMBEHAVIOR_BUILD_TOOLS     "Build developer tools"    ON)
option(SIMBEHAVIOR_BUILD_SIM_HOST  "Build sim_host executable" ON)

# ── 编译标志 ──────────────────────────────────────────────────────────────────
include(cmake/CompilerFlags.cmake)

# ── 禁止三方子项目自动构建其测试（TBB / libuv / BTCPP 等均遵守 BUILD_TESTING）
# 我们通过 SIMBEHAVIOR_BUILD_TESTS 控制自己的测试，独立于此变量
set(BUILD_TESTING OFF CACHE BOOL "Disable testing in all subprojects" FORCE)

# ── 三方依赖 ──────────────────────────────────────────────────────────────────
include(cmake/Dependencies.cmake)

# ── 公共头文件目录（对所有子模块可见）────────────────────────────────────────
set(SIMBEHAVIOR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ── 子模块 ───────────────────────────────────────────────────────────────────

# 运行时层
add_subdirectory(src/runtime/compute_runtime)
add_subdirectory(src/runtime/async_runtime)
add_subdirectory(src/runtime/bt_runtime)

# 领域模型层
add_subdirectory(src/domain)

# 适配器层
add_subdirectory(src/adapters)

# BT 节点层
add_subdirectory(src/bt_nodes)

# 宿主进程
if(SIMBEHAVIOR_BUILD_SIM_HOST)
  add_subdirectory(src/sim_host)
endif()

# 工具
if(SIMBEHAVIOR_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

# 测试
if(SIMBEHAVIOR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
