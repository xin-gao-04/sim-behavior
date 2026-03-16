add_library(simbehavior_bt_runtime STATIC
  bt_runtime_impl.cpp
  tree_instance_impl.cpp
  async_action_context_impl.cpp
  sync_node_context_impl.cpp
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
    simbehavior_domain    # SyncNodeContextImpl 需要 IEntityContext / IWorldSnapshot
    simbehavior_adapters  # SyncNodeContextImpl 需要 ICommandBus
  PRIVATE
    corekit
    simbehavior_log  # 进程级日志（SIMBT_LOG_* 宏）
)

target_compile_features(simbehavior_bt_runtime PUBLIC cxx_std_17)
target_link_libraries(simbehavior_bt_runtime PRIVATE simbehavior_compile_warnings)
