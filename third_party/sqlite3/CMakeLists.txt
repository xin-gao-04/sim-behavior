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
