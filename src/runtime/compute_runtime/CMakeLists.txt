add_library(simbehavior_compute_runtime STATIC
  tbb_job_executor.cpp
  default_result_mailbox.cpp
)

target_include_directories(simbehavior_compute_runtime
  PUBLIC
    ${SIMBEHAVIOR_INCLUDE_DIR}
)

target_link_libraries(simbehavior_compute_runtime
  PUBLIC
    TBB::tbb
  PRIVATE
    corekit  # 使用 corekit 内存分配器（可选）
)

target_compile_features(simbehavior_compute_runtime PUBLIC cxx_std_17)
target_link_libraries(simbehavior_compute_runtime PRIVATE simbehavior_compile_warnings)
