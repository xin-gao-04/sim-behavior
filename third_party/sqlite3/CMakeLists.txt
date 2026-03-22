cmake_minimum_required(VERSION 3.14)
project(sqlite3 VERSION 3.47.2 LANGUAGES C)

# SQLite3 单文件 amalgamation 静态库
# 禁用 loadable extension（减少依赖和攻击面）
add_library(sqlite3 STATIC sqlite3.c)
target_include_directories(sqlite3 PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:include>
)
target_compile_definitions(sqlite3 PUBLIC
  SQLITE_OMIT_LOAD_EXTENSION=1
)
set_target_properties(sqlite3 PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON
)

# 提供 CMake 标准 SQLite::SQLite3 别名，供 find_package(SQLite3) 使用
if(NOT TARGET SQLite::SQLite3)
  add_library(SQLite::SQLite3 ALIAS sqlite3)
endif()

# BehaviorTree.CPP 将 sqlite3 以 PUBLIC 方式链接，因此它的
# install(EXPORT behaviortree_cppTargets ...) 会要求 sqlite3 也在
# 某个 export set 中。这里添加最小 install 规则以通过 CMake 验证。
# 实际项目无需安装 sqlite3，但必须存在该规则否则 cmake Generate 报错。
include(GNUInstallDirs)
install(TARGETS sqlite3
  EXPORT sqlite3Targets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/sqlite3.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/sqlite3ext.h"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
