cmake_minimum_required(VERSION 3.22)

# ─────────────────────────────────────────────────────────────────────────────
# Squad Patrol 示例项目 CMakeLists.txt
#
# 构建方式（推荐）：通过父项目 -DSIMBEHAVIOR_BUILD_EXAMPLES=ON 一起构建
#
#   cmake -S /path/to/sim-behavior -B build -DSIMBEHAVIOR_BUILD_EXAMPLES=ON
#   cmake --build build --target squad_patrol_example -j$(nproc)
#   cd build/Release && ./squad_patrol_example
#
# 也可以用独立构建（需要 sim-behavior 已经 build 完成）：
#   cmake -S . -B build_standalone
#   cmake --build build_standalone
# ─────────────────────────────────────────────────────────────────────────────

# ── 可执行文件 ────────────────────────────────────────────────────────────────
add_executable(squad_patrol_example
    src/main.cpp
    # nodes.hpp 和 config_loader.hpp 是 header-only，不需要单独列出 .cpp
)

# ── C++17 标准（required for inline static 和 std::any）──────────────────────
target_compile_features(squad_patrol_example PRIVATE cxx_std_17)

# ── 头文件搜索路径 ─────────────────────────────────────────────────────────────
target_include_directories(squad_patrol_example
    PRIVATE
        # sim_bt 公共接口（IBtRuntime, AsyncActionBase, ConditionBase 等）
        ${SIMBEHAVIOR_INCLUDE_DIR}

        # 内部头文件（SimHostApp 位于 src/sim_host/，不在公共 include/ 目录）
        # 等同于测试中的 ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_SOURCE_DIR}/src

        # 示例自身的头文件（nodes.hpp, config_loader.hpp）
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ── 链接库 ────────────────────────────────────────────────────────────────────
#
# 链接层次说明：
#   simbehavior_sim_host      — SimHostApp（最高层，内部已 PUBLIC 依赖以下所有层）
#   simbehavior_bt_nodes      — AsyncActionBase, ConditionBase, SyncActionBase
#   simbehavior_bt_runtime    — IBtRuntime interface + BtRuntimeImpl
#   simbehavior_adapters      — ICommandBus, InProcessCommandBus
#   simbehavior_domain        — IEntityContext, IGroupContext
#   simbehavior_compute_runtime — IJobExecutor, JobPriority, JobResult, IResultMailbox
#   simbehavior_async_runtime — IEventLoopRuntime, IWakeupBridge
#
target_link_libraries(squad_patrol_example
    PRIVATE
        simbehavior_sim_host
        simbehavior_bt_nodes
        simbehavior_bt_runtime
        simbehavior_adapters
        simbehavior_domain
        simbehavior_compute_runtime
        simbehavior_async_runtime
)

# ── 将运行时资源（config + trees）复制到可执行文件目录 ───────────────────────
#
# 这样从构建目录直接运行可执行文件时，相对路径 "config/simulation.json"
# 和 "trees/squad_tree.xml" 能够正常访问。
# ─────────────────────────────────────────────────────────────────────────────
add_custom_command(TARGET squad_patrol_example POST_BUILD
    # 复制 config/ 目录
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/config
        $<TARGET_FILE_DIR:squad_patrol_example>/config
    # 复制 trees/ 目录
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/trees
        $<TARGET_FILE_DIR:squad_patrol_example>/trees
    COMMENT "复制 config 和 trees 资源到构建目录..."
)

# ── 提示信息 ──────────────────────────────────────────────────────────────────
message(STATUS "[sim-behavior] 示例目标已添加: squad_patrol_example")
message(STATUS "  运行方式: cd <build>/<config> && ./squad_patrol_example")
message(STATUS "  配置文件: config/simulation.json (复制到构建目录后可直接编辑)")
