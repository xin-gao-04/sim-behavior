add_library(simbehavior_bt_runtime STATIC
  bt_runtime_impl.cpp
  tree_instance_impl.cpp
  async_action_context_impl.cpp
)

target_include_directories(simbehavior_bt_runtime
  PUBLIC
    ${SIMBEHAVIOR_INCLUDE_DIR}
)

target_link_libraries(simbehavior_bt_runtime
  PUBLIC
    BT::behaviortree_cpp
    simbehavior_compute_runtime
    simbehavior_async_runtime
  PRIVATE
    corekit
)

target_compile_features(simbehavior_bt_runtime PUBLIC cxx_std_17)
