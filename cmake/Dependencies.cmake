# ─────────────────────────────────────────────────────────────────────────────
# Dependencies.cmake
#
# 所有依赖均从源码编译，永远不依赖系统安装包。
#
# 依赖引入优先级（从高到低）：
#   1. third_party/<dep>/  目录已存在且含 CMakeLists.txt
#      → 直接使用（已解压 or git submodule）
#   2. third_party/<dep>.zip  文件存在
#      → 自动解压到 third_party/<dep>/ 后使用
#      → zip 文件需事先放入仓库（内网离线首选方案）
#   3. FetchContent 在线拉取
#      → 仅当前两项均不可用时触发（需要网络）
#
# 内网部署流程（无需修改 CMake）：
#   1. 在有网机器运行 scripts/vendor-deps.sh，生成各 .zip
#   2. 将 .zip 文件提交到仓库 third_party/ 目录
#   3. 内网机器直接 cmake -B build，自动解压并编译
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.18)  # file(ARCHIVE_EXTRACT) 需要 3.18+
include(FetchContent)

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  辅助函数：_unzip_dep                                                   ║
# ║                                                                          ║
# ║  将 third_party/<dep>.zip 解压到 third_party/<dep>/。                   ║
# ║  GitHub zip 顶层通常含一个 <repo>-<tag>/ 子目录，函数自动处理。         ║
# ║  已存在目录则跳过，保证幂等。                                             ║
# ╚══════════════════════════════════════════════════════════════════════════╝
function(_unzip_dep dep_name zip_path dest_dir)
  # 已解压则跳过
  if(EXISTS "${dest_dir}/CMakeLists.txt")
    return()
  endif()
  if(NOT EXISTS "${zip_path}")
    return()
  endif()

  message(STATUS "[sim-behavior] Extracting ${dep_name}: ${zip_path} → ${dest_dir}")

  # 解压到临时目录，避免部分解压污染目标目录
  set(_tmp "${dest_dir}__tmp_extract")
  file(REMOVE_RECURSE "${_tmp}")
  file(MAKE_DIRECTORY "${_tmp}")

  # file(ARCHIVE_EXTRACT) 跨平台，CMake 3.18+；支持 .zip / .tar.gz
  file(ARCHIVE_EXTRACT INPUT "${zip_path}" DESTINATION "${_tmp}")

  # GitHub zip 通常解压为单个顶层子目录（如 oneTBB-v2022.0.0/）
  # 若如此，将该子目录直接重命名为 dest_dir；否则将整个 tmp 重命名
  file(GLOB _top_entries LIST_DIRECTORIES true "${_tmp}/*")
  list(LENGTH _top_entries _top_count)

  if(_top_count EQUAL 1)
    list(GET _top_entries 0 _single)
    if(IS_DIRECTORY "${_single}")
      file(RENAME "${_single}" "${dest_dir}")
      file(REMOVE_RECURSE "${_tmp}")
      message(STATUS "[sim-behavior] ${dep_name}: extracted (single root dir) → ${dest_dir}")
      return()
    endif()
  endif()

  # 多个顶层条目（平铺 zip）：直接使用 tmp 目录
  file(RENAME "${_tmp}" "${dest_dir}")
  message(STATUS "[sim-behavior] ${dep_name}: extracted (flat zip) → ${dest_dir}")
endfunction()


# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  辅助宏：_dep_add                                                       ║
# ║                                                                          ║
# ║  统一三种来源：已解压目录 / .zip 自动解压 / FetchContent 在线拉取       ║
# ║                                                                          ║
# ║  调用格式：                                                              ║
# ║    _dep_add(<name> <vendor_dir> <git_repo> <git_tag>                    ║
# ║             [OPT "KEY=val" ...])                                         ║
# ╚══════════════════════════════════════════════════════════════════════════╝
macro(_dep_add _name _vendor_dir _git_repo _git_tag)
  # 先处理附加的 CMake 选项（设置 CACHE 变量影响子项目）
  foreach(_o ${ARGN})
    string(REPLACE "=" ";" _kv "${_o}")
    list(GET _kv 0 _k)
    list(GET _kv 1 _v)
    set(${_k} "${_v}" CACHE BOOL "" FORCE)
  endforeach()

  set(_dep_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_vendor_dir}")
  set(_dep_zip "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_vendor_dir}.zip")

  # 优先级 1：目录已存在
  if(EXISTS "${_dep_dir}/CMakeLists.txt")
    message(STATUS "[sim-behavior] ${_name}: using ${_dep_dir}")
    add_subdirectory("${_dep_dir}"
      "${CMAKE_BINARY_DIR}/third_party/${_vendor_dir}" EXCLUDE_FROM_ALL)

  # 优先级 2：zip 存在，自动解压
  elseif(EXISTS "${_dep_zip}")
    _unzip_dep("${_name}" "${_dep_zip}" "${_dep_dir}")
    if(EXISTS "${_dep_dir}/CMakeLists.txt")
      add_subdirectory("${_dep_dir}"
        "${CMAKE_BINARY_DIR}/third_party/${_vendor_dir}" EXCLUDE_FROM_ALL)
    else()
      message(FATAL_ERROR
        "[sim-behavior] ${_name}: zip extracted but CMakeLists.txt not found in ${_dep_dir}\n"
        "  请检查 zip 内容是否完整，或重新打包。")
    endif()

  # 优先级 3：FetchContent 在线拉取
  else()
    message(STATUS "[sim-behavior] ${_name}: no zip found, FetchContent ${_git_repo} @ ${_git_tag}")
    message(STATUS "[sim-behavior]   提示：可将 zip 放入 third_party/${_vendor_dir}.zip 以实现离线编译")
    FetchContent_Declare(${_name}
      GIT_REPOSITORY "${_git_repo}"
      GIT_TAG        "${_git_tag}"
      GIT_SHALLOW    ON
    )
    FetchContent_MakeAvailable(${_name})
  endif()

  unset(_dep_dir)
  unset(_dep_zip)
endmacro()


# header-only 变体（不需要 add_subdirectory，只需源码头文件目录）
macro(_dep_headers _name _vendor_dir _git_repo _git_tag)
  set(_dep_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_vendor_dir}")
  set(_dep_zip "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_vendor_dir}.zip")

  if(EXISTS "${_dep_dir}/src")
    # 已解压（src/ 是 uvw 的头文件入口目录）
    set(_${_name}_src "${_dep_dir}")
    message(STATUS "[sim-behavior] ${_name}: using vendored headers ${_dep_dir}")

  elseif(EXISTS "${_dep_zip}")
    _unzip_dep("${_name}" "${_dep_zip}" "${_dep_dir}")
    set(_${_name}_src "${_dep_dir}")
    message(STATUS "[sim-behavior] ${_name}: extracted headers ${_dep_dir}")

  else()
    message(STATUS "[sim-behavior] ${_name}: no zip found, FetchContent ${_git_repo} @ ${_git_tag}")
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

  unset(_dep_dir)
  unset(_dep_zip)
endmacro()


# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  0. corekit  (git submodule — 必须，不支持 zip / FetchContent)          ║
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
# ║  1. oneTBB  (TBB::tbb + TBB::tbbmalloc)                                ║
# ║                                                                          ║
# ║  zip 打包来源：                                                          ║
# ║    https://github.com/oneapi-src/oneTBB/archive/refs/tags/v2022.0.0.zip ║
# ║  放置为：third_party/oneTBB.zip                                         ║
# ╚══════════════════════════════════════════════════════════════════════════╝
if(NOT TARGET TBB::tbb)
  _dep_add(oneTBB oneTBB
    https://github.com/oneapi-src/oneTBB.git v2022.0.0
    "TBB_TEST=OFF" "TBB_STRICT=OFF" "TBB_EXAMPLES=OFF" "TBBMALLOC_BUILD=ON"
  )
endif()
if(NOT TARGET TBB::tbb)
  message(FATAL_ERROR
    "[sim-behavior] TBB::tbb not found after oneTBB setup.\n"
    "  请将 third_party/oneTBB.zip 加入仓库，或确保网络可访问 GitHub。")
endif()
message(STATUS "[sim-behavior] TBB::tbb ready")


# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  2. libuv  (uv_a 静态库)                                                ║
# ║                                                                          ║
# ║  zip 打包来源：                                                          ║
# ║    https://github.com/libuv/libuv/archive/refs/tags/v1.48.0.zip         ║
# ║  放置为：third_party/libuv.zip                                          ║
# ╚══════════════════════════════════════════════════════════════════════════╝
if(NOT TARGET uv_a AND NOT TARGET uv)
  _dep_add(libuv libuv
    https://github.com/libuv/libuv.git v1.48.0
    "LIBUV_BUILD_TESTS=OFF" "LIBUV_BUILD_BENCH=OFF"
  )
endif()
if(NOT TARGET uv::uv)
  if(TARGET uv_a)
    add_library(uv::uv ALIAS uv_a)
  elseif(TARGET uv)
    add_library(uv::uv ALIAS uv)
  else()
    message(FATAL_ERROR
      "[sim-behavior] libuv target not found.\n"
      "  请将 third_party/libuv.zip 加入仓库，或确保网络可访问 GitHub。")
  endif()
endif()
message(STATUS "[sim-behavior] libuv (uv::uv) ready")


# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  3. uvw  (header-only C++17 包装)                                       ║
# ║                                                                          ║
# ║  zip 打包来源：                                                          ║
# ║    https://github.com/skypjack/uvw/archive/refs/tags/v3.4.0_libuv_v1.48.zip ║
# ║  放置为：third_party/uvw.zip                                            ║
# ╚══════════════════════════════════════════════════════════════════════════╝
if(NOT TARGET uvw::uvw)
  _dep_headers(uvw uvw
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
# ║  4. BehaviorTree.CPP v4                                                 ║
# ║                                                                          ║
# ║  zip 打包来源：                                                          ║
# ║    https://github.com/BehaviorTree/BehaviorTree.CPP/archive/refs/tags/4.9.0.zip ║
# ║  放置为：third_party/BehaviorTree.CPP.zip                               ║
# ╚══════════════════════════════════════════════════════════════════════════╝
if(NOT TARGET behaviortree_cpp AND NOT TARGET BT::behaviortree_cpp)
  _dep_add(behaviortree_cpp BehaviorTree.CPP
    https://github.com/BehaviorTree/BehaviorTree.CPP.git 4.9.0
    "BTCPP_GROOT_INTERFACE=OFF" "BTCPP_SQLITE_LOGGING=OFF"
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
      "  请将 third_party/BehaviorTree.CPP.zip 加入仓库，或确保网络可访问 GitHub。")
  endif()
endif()
message(STATUS "[sim-behavior] BT::behaviortree_cpp ready")


# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  5. GoogleTest  (仅测试构建)                                             ║
# ║                                                                          ║
# ║  zip 打包来源：                                                          ║
# ║    https://github.com/google/googletest/archive/refs/tags/v1.16.0.zip   ║
# ║  放置为：third_party/googletest.zip                                     ║
# ║                                                                          ║
# ║  注意：使用 v1.16.0 以对齐 BehaviorTree.CPP 依赖的系统 GTest ABI。     ║
# ║  v1.14→v1.16 存在 MakeAndRegisterTestInfo 签名变化（string vs char*）。║
# ╚══════════════════════════════════════════════════════════════════════════╝
if(SIMBEHAVIOR_BUILD_TESTS AND NOT TARGET GTest::gtest)
  _dep_add(googletest googletest
    https://github.com/google/googletest.git v1.16.0
    "INSTALL_GTEST=OFF" "BUILD_GMOCK=ON"
  )
  message(STATUS "[sim-behavior] GoogleTest ready")
endif()
