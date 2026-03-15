# ─────────────────────────────────────────────────────────────────────────────
# Dependencies.cmake
#
# 统一管理所有三方依赖的查找和引入方式。
# 支持两种模式：
#   vcpkg (推荐): -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#   系统安装    : 直接 find_package
# ─────────────────────────────────────────────────────────────────────────────

# ── corekit (git submodule) ──────────────────────────────────────────────────
set(COREKIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/corekit")
if(EXISTS "${COREKIT_DIR}/CMakeLists.txt")
  # 禁用 corekit 自带测试避免干扰
  set(COREKIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${COREKIT_DIR}" corekit EXCLUDE_FROM_ALL)
else()
  message(FATAL_ERROR
    "corekit submodule not found at ${COREKIT_DIR}. "
    "Run: git submodule update --init --recursive")
endif()

# ── BehaviorTree.CPP ─────────────────────────────────────────────────────────
find_package(behaviortree_cpp QUIET)
if(NOT behaviortree_cpp_FOUND)
  # vcpkg 安装名为 behaviortree-cpp
  find_package(BehaviorTreeV4 QUIET)
  if(NOT BehaviorTreeV4_FOUND)
    message(FATAL_ERROR
      "BehaviorTree.CPP not found. Install via vcpkg:\n"
      "  vcpkg install behaviortree-cpp\n"
      "or build from source: https://github.com/BehaviorTree/BehaviorTree.CPP")
  endif()
  # 统一 alias
  if(TARGET BT::behaviortree_cpp_v4 AND NOT TARGET behaviortree_cpp::behaviortree_cpp)
    add_library(behaviortree_cpp::behaviortree_cpp ALIAS BT::behaviortree_cpp_v4)
  endif()
endif()

# ── oneTBB ───────────────────────────────────────────────────────────────────
find_package(TBB REQUIRED COMPONENTS tbb)
# TBB::tbb 目标由 oneTBB 的 CMake 配置提供

# ── uvw (libuv C++ wrapper) ──────────────────────────────────────────────────
# uvw 可以 header-only 引入，也可以编译为静态库。
# 优先 find_package，其次 FetchContent。
find_package(uvw QUIET)
if(NOT uvw_FOUND)
  message(STATUS "uvw not found via find_package, trying FetchContent")
  include(FetchContent)
  FetchContent_Declare(
    uvw
    GIT_REPOSITORY https://github.com/skypjack/uvw.git
    GIT_TAG        v3.3.0_libuv_v1.46
    GIT_SHALLOW    ON
  )
  FetchContent_MakeAvailable(uvw)
  # uvw header-only 目标名为 uvw::uvw
endif()

# libuv (uvw 的底层依赖)
find_package(libuv QUIET)
if(NOT libuv_FOUND)
  find_package(unofficial-libuv CONFIG QUIET)  # vcpkg 包名
  if(NOT unofficial-libuv_FOUND)
    message(FATAL_ERROR
      "libuv not found. Install via vcpkg:\n"
      "  vcpkg install libuv\n"
      "or via system package manager.")
  endif()
endif()

# ── GoogleTest (仅测试构建时引入) ────────────────────────────────────────────
if(SIMBEHAVIOR_BUILD_TESTS)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.14.0
      GIT_SHALLOW    ON
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
endif()
