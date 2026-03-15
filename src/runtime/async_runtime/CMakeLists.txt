add_library(simbehavior_async_runtime STATIC
  uvw_event_loop_runtime.cpp
  uvw_wakeup_bridge.cpp
  uvw_timer_handle.cpp
)

target_include_directories(simbehavior_async_runtime
  PUBLIC
    ${SIMBEHAVIOR_INCLUDE_DIR}
)

target_link_libraries(simbehavior_async_runtime
  PUBLIC
    uvw::uvw
  PRIVATE
    simbehavior_compute_runtime
)

target_compile_features(simbehavior_async_runtime PUBLIC cxx_std_17)
