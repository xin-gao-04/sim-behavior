# ── sim_host 实现库（供测试和可执行文件共用）─────────────────────────────────
add_library(simbehavior_sim_host STATIC
  sim_host_app.cpp
)

target_include_directories(simbehavior_sim_host
  PUBLIC
    ${SIMBEHAVIOR_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..   # 允许包含 src/ 内部头文件
)

target_link_libraries(simbehavior_sim_host
  PUBLIC
    simbehavior_bt_nodes
    simbehavior_bt_runtime
    simbehavior_async_runtime
    simbehavior_compute_runtime
    simbehavior_domain
    simbehavior_adapters
  PRIVATE
    corekit
    simbehavior_log
)

target_compile_features(simbehavior_sim_host PUBLIC cxx_std_17)
target_link_libraries(simbehavior_sim_host PRIVATE simbehavior_compile_warnings)

# ── 可执行入口 ──────────────────────────────────────────────────────────────
add_executable(sim_host main.cpp)

target_link_libraries(sim_host PRIVATE simbehavior_sim_host)
target_compile_features(sim_host PRIVATE cxx_std_17)
target_link_libraries(sim_host PRIVATE simbehavior_compile_warnings)
