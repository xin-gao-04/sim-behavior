add_library(simbehavior_log STATIC
  sim_bt_log.cpp
)

target_include_directories(simbehavior_log
  PUBLIC
    ${SIMBEHAVIOR_INCLUDE_DIR}
)

target_link_libraries(simbehavior_log
  PUBLIC
    corekit   # 暴露 ILogManager* 类型，消费方无需再单独 link corekit 即可使用宏
)

target_compile_features(simbehavior_log PUBLIC cxx_std_17)
target_link_libraries(simbehavior_log PRIVATE simbehavior_compile_warnings)
