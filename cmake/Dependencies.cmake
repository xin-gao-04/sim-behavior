# ─────────────────────────────────────────────────────────────────────────────
# Dependencies.cmake
#
# 所有依赖均从源码编译进项目，不依赖系统安装包。
# 适合跨平台部署和内网离线环境。
#
# 依赖引入策略（优先级从高到低）：
#   1. third_party/<dep>/ 子目录存在且有 CMakeLists.txt
#      → 直接 add_subdirectory（git submodule 或手动 vendor）
#   2. SIMBEHAVIOR_<DEP>_SOURCE_DIR 缓存变量被用户手动指定
#      → 使用指定路径（适合内网离线环境）
#   3. FetchContent 在线下载
#      → 仅当前两项均不可用时触发
#
# 内网离线部署方式（两选一）：
#   A) git submodule（推荐，一次 clone 全部携带）：
#      git submodule add <mirror-url> third_party/oneTBB
#      git submodule add <mirror-url> third_party/libuv
#      git submodule add <mirror-url> third_party/uvw
#      git submodule add <mirror-url> third_party/BehaviorTree.CPP
#      git submodule add <mirror-url> third_party/googletest
#
#   B) 手动指定路径（已有本地镜像目录时）：
#      cmake -B build \
#        -DSIMBEHAVIOR_TBB_SOURCE_DIR=/mirrors/oneTBB     \
#        -DSIMBEHAVIOR_LIBUV_SOURCE_DIR=/mirrors/libuv    \
#        -DSIMBEHAVIOR_UVW_SOURCE_DIR=/mirrors/uvw        \
#        -DSIMBEHAVIOR_BTCPP_SOURCE_DIR=/mirrors/BT.CPP  \
#        -DSIMBEHAVIOR_GTEST_SOURCE_DIR=/mirrors/gtest
# ─────────────────────────────────────────────────────────────────────────────

include(FetchContent)

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║ 辅助宏：_dep_add — 统一 submodule / 本地路径 / FetchContent 三种模式   ║
# ╚══════════════════════════════════════════════════════════════════════════╝
# 参数：
#   NAME        逻辑名（用于 FetchContent）
#   VENDOR_DIR  third_party/ 下的子目录名
#   SOURCE_VAR  用户可覆盖的 CMake 缓存路径变量名
#   GIT_REPO    FetchContent GIT_REPOSITORY
#   GIT_TAG     FetchContent GIT_TAG
#   OPTS        传入子项目的选项，形如 "KEY=val" 列表
macro(_dep_add _name _vendor_dir _source_var _git_repo _git_tag)
  # 处理可变参数 OPTS
  set(_dep_opts ${ARGN})
  foreach(_o IN LISTS _dep_opts)
    string(REPLACE "=" ";" _kv "${_o}")
    list(GET _kv 0 _k)
    list(GET _kv 1 _v)
    set(${_k} "${_v}" CACHE BOOL "" FORCE)
  endforeach()

  set(_vendor_path "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_vendor_dir}")

  if(EXISTS "${_vendor_path}/CMakeLists.txt")
    message(STATUS "[sim-behavior] ${_name}: submodule at ${_vendor_path}")
    add_subdirectory("${_vendor_path}"
      "${CMAKE_BINARY_DIR}/third_party/${_vendor_dir}" EXCLUDE_FROM_ALL)

  elseif(${_source_var} AND EXISTS "${${_source_var}}/CMakeLists.txt")
    message(STATUS "[sim-behavior] ${_name}: local path ${${_source_var}}")
    add_subdirectory("${${_source_var}}"
      "${CMAKE_BINARY_DIR}/third_party/${_vendor_dir}" EXCLUDE_FROM_ALL)

  else()
    message(STATUS "[sim-behavior] ${_name}: FetchContent ${_git_repo} @ ${_git_tag}")
    FetchContent_Declare(${_name}
      GIT_REPOSITORY "${_git_repo}"
      GIT_TAG        "${_git_tag}"
      GIT_SHALLOW    ON
    )
    FetchContent_MakeAvailable(${_name})
  endif()
endmacro()

# header-only 变体（只需 populate 源码目录，不 add_subdirectory）
macro(_dep_headers _name _vendor_dir _source_var _git_repo _git_tag)
  set(_vendor_path "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_vendor_dir}")

  if(EXISTS "${_vendor_path}")
    set(_${_name}_src "${_vendor_path}")
    message(STATUS "[sim-behavior] ${_name}: submodule headers at ${_vendor_path}")
  elseif(${_source_var} AND EXISTS "${${_source_var}}")
    set(_${_name}_src "${${_source_var}}")
    message(STATUS "[sim-behavior] ${_name}: local headers at ${${_source_var}}")
  else()
    message(STATUS "[sim-behavior] ${_name}: FetchContent headers ${_git_repo} @ ${_git_tag}")
    FetchContent_Declare(${_name}
      GIT_REPOSITORY "${_git_repo}"
      GIT_TAG        "${_git_tag}"
      GIT_SHALLOW    ON
    )
    FetchContent_GetProperties(${_name})
    if(NOT ${_name}_POPULATED)
      FetchContent_Populate(${_name})
    endif()
    set(_${_name}_src "${${_name}_SOURCE_DIR}")
  endif()
endmacro()


# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  0. corekit  (git submodule — 必须，不可 FetchContent)                  ║
# ╚══════════════════════════════════════════════════════════════════════════╝
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/corekit/CMakeLists.txt")
  message(FATAL_ERROR
    "[sim-behavior] corekit submodule not initialized.\n"
    "  Run: git submodule update --init --recursive")
endif()
set(COREKIT_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(COREKIT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/corekit"
  "${CMAKE_BINARY_DIR}/third_party/corekit" EXCLUDE_FROM_ALL)

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  1. oneTBB  (TBB::tbb + TBB::tbbmalloc，从源码编译)                    ║
# ║                                                                          ║
# ║  同一个 oneTBB 源码树提供两个独立目标：                                  ║
# ║    TBB::tbb       — task_arena，sim-behavior 用于 CPU 调度              ║
# ║    TBB::tbbmalloc — 可扩展分配器，corekit 可选用作内存后端              ║
# ╚══════════════════════════════════════════════════════════════════════════╝
set(SIMBEHAVIOR_TBB_SOURCE_DIR "" CACHE PATH
  "oneTBB 本地源码目录（离线部署，优先级低于 third_party/oneTBB）")

if(NOT TARGET TBB::tbb)
  _dep_add(oneTBB oneTBB SIMBEHAVIOR_TBB_SOURCE_DIR
    https://github.com/oneapi-src/oneTBB.git v2022.0.0
    "TBB_TEST=OFF" "TBB_STRICT=OFF" "TBB_EXAMPLES=OFF" "TBBMALLOC_BUILD=ON"
  )
endif()

if(NOT TARGET TBB::tbb)
  message(FATAL_ERROR
    "[sim-behavior] TBB::tbb not available.\n"
    "  Add submodule: git submodule add <url> third_party/oneTBB\n"
    "  Or set: -DSIMBEHAVIOR_TBB_SOURCE_DIR=<path>")
endif()
message(STATUS "[sim-behavior] TBB::tbb ready")

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  2. libuv  (从源码编译，uv_a 静态库)                                    ║
# ╠══════════════════════════════════════════════════════════════════════════╣
# ║  静态库名：uv_a（libuv CMake 构建的标准目标名）                          ║
# ╚══════════════════════════════════════════════════════════════════════════╝
set(SIMBEHAVIOR_LIBUV_SOURCE_DIR "" CACHE PATH
  "libuv 本地源码目录（离线部署）")

if(NOT TARGET uv_a AND NOT TARGET uv)
  _dep_add(libuv libuv SIMBEHAVIOR_LIBUV_SOURCE_DIR
    https://github.com/libuv/libuv.git v1.48.0
    "LIBUV_BUILD_TESTS=OFF" "LIBUV_BUILD_BENCH=OFF"
  )
endif()

# 统一别名 uv::uv
if(NOT TARGET uv::uv)
  if(TARGET uv_a)
    add_library(uv::uv ALIAS uv_a)
  elseif(TARGET uv)
    add_library(uv::uv ALIAS uv)
  else()
    message(FATAL_ERROR
      "[sim-behavior] libuv target not found.\n"
      "  Add submodule: git submodule add <url> third_party/libuv\n"
      "  Or set: -DSIMBEHAVIOR_LIBUV_SOURCE_DIR=<path>")
  endif()
endif()
message(STATUS "[sim-behavior] libuv (uv::uv) ready")

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  3. uvw  (header-only C++17 包装，只需头文件 src/ 目录)                 ║
# ╚══════════════════════════════════════════════════════════════════════════╝
set(SIMBEHAVIOR_UVW_SOURCE_DIR "" CACHE PATH
  "uvw 本地源码目录（离线部署）")

if(NOT TARGET uvw::uvw)
  _dep_headers(uvw uvw SIMBEHAVIOR_UVW_SOURCE_DIR
    https://github.com/skypjack/uvw.git v3.4.0_libuv_v1.48
  )
  add_library(_simbehavior_uvw_iface INTERFACE)
  add_library(uvw::uvw ALIAS _simbehavior_uvw_iface)
  target_include_directories(_simbehavior_uvw_iface INTERFACE "${_uvw_src}/src")
  target_link_libraries(_simbehavior_uvw_iface INTERFACE uv::uv)
  target_compile_features(_simbehavior_uvw_iface INTERFACE cxx_std_17)
endif()
message(STATUS "[sim-behavior] uvw::uvw ready")

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  4. BehaviorTree.CPP v4  (从源码编译)                                   ║
# ╠══════════════════════════════════════════════════════════════════════════╣
# ║  CMake 目标：behaviortree_cpp                                            ║
# ║  别名：BT::behaviortree_cpp                                              ║
# ╚══════════════════════════════════════════════════════════════════════════╝
set(SIMBEHAVIOR_BTCPP_SOURCE_DIR "" CACHE PATH
  "BehaviorTree.CPP 本地源码目录（离线部署）")

if(NOT TARGET behaviortree_cpp AND NOT TARGET BT::behaviortree_cpp)
  _dep_add(behaviortree_cpp BehaviorTree.CPP SIMBEHAVIOR_BTCPP_SOURCE_DIR
    https://github.com/BehaviorTree/BehaviorTree.CPP.git 4.6.2
    "BTCPP_UNIT_TESTS=OFF" "BTCPP_BUILD_TOOLS=OFF"
    "BTCPP_EXAMPLES=OFF"   "BUILD_TESTING=OFF"
  )
endif()

if(NOT TARGET BT::behaviortree_cpp)
  if(TARGET behaviortree_cpp::behaviortree_cpp)
    add_library(BT::behaviortree_cpp ALIAS behaviortree_cpp::behaviortree_cpp)
  elseif(TARGET behaviortree_cpp)
    add_library(BT::behaviortree_cpp ALIAS behaviortree_cpp)
  else()
    message(FATAL_ERROR
      "[sim-behavior] BehaviorTree.CPP target not found.\n"
      "  Add submodule: git submodule add <url> third_party/BehaviorTree.CPP\n"
      "  Or set: -DSIMBEHAVIOR_BTCPP_SOURCE_DIR=<path>")
  endif()
endif()
message(STATUS "[sim-behavior] BT::behaviortree_cpp ready")

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  5. GoogleTest  (从源码编译，仅测试构建)                                 ║
# ╚══════════════════════════════════════════════════════════════════════════╝
if(SIMBEHAVIOR_BUILD_TESTS)
  set(SIMBEHAVIOR_GTEST_SOURCE_DIR "" CACHE PATH
    "googletest 本地源码目录（离线部署）")

  if(NOT TARGET GTest::gtest)
    _dep_add(googletest googletest SIMBEHAVIOR_GTEST_SOURCE_DIR
      https://github.com/google/googletest.git v1.14.0
      "INSTALL_GTEST=OFF" "BUILD_GMOCK=ON"
    )
  endif()
  message(STATUS "[sim-behavior] GoogleTest ready")
endif()
