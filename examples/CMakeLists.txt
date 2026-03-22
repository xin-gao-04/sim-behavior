cmake_minimum_required(VERSION 3.22)

# ─────────────────────────────────────────────────────────────────────────────
# examples/CMakeLists.txt
#
# 所有示例项目的聚合入口。
# 通过父项目 -DSIMBEHAVIOR_BUILD_EXAMPLES=ON 触发。
# ─────────────────────────────────────────────────────────────────────────────

add_subdirectory(squad_patrol)
