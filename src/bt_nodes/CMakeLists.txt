add_library(simbehavior_bt_nodes STATIC
  async_action_base.cpp
  # 以下节点在后续迭代中添加：
  # condition/has_target_condition.cpp
  # action/sync/move_to_action.cpp
  # action/async/path_plan_action.cpp
  # action/async/threat_assess_action.cpp
  # action/io/wait_bus_event_action.cpp
)

target_include_directories(simbehavior_bt_nodes
  PUBLIC
    ${SIMBEHAVIOR_INCLUDE_DIR}
)

target_link_libraries(simbehavior_bt_nodes
  PUBLIC
    BT::behaviortree_cpp
    simbehavior_bt_runtime
    simbehavior_domain
    simbehavior_adapters
)

target_compile_features(simbehavior_bt_nodes PUBLIC cxx_std_17)
target_link_libraries(simbehavior_bt_nodes PRIVATE simbehavior_compile_warnings)
