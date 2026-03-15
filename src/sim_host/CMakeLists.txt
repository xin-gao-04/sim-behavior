add_executable(sim_host
  main.cpp
  sim_host_app.cpp
)

target_include_directories(sim_host
  PRIVATE
    ${SIMBEHAVIOR_INCLUDE_DIR}
)

target_link_libraries(sim_host
  PRIVATE
    simbehavior_bt_nodes
    simbehavior_bt_runtime
    simbehavior_async_runtime
    simbehavior_compute_runtime
    simbehavior_domain
    simbehavior_adapters
    corekit
)

target_compile_features(sim_host PRIVATE cxx_std_17)
target_link_libraries(sim_host PRIVATE simbehavior_compile_warnings)
