find_package(GTest REQUIRED)

add_executable(sim_behavior_tests
  test_main.cpp
  test_cancellation_token.cpp
  test_result_mailbox.cpp
  test_entity_context.cpp
  test_async_action_base.cpp
)

target_include_directories(sim_behavior_tests
  PRIVATE
    ${SIMBEHAVIOR_INCLUDE_DIR}
)

target_link_libraries(sim_behavior_tests
  PRIVATE
    simbehavior_bt_nodes
    simbehavior_bt_runtime
    simbehavior_async_runtime
    simbehavior_compute_runtime
    simbehavior_domain
    simbehavior_adapters
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(sim_behavior_tests)
