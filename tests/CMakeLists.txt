# GTest 已在 cmake/Dependencies.cmake 中通过 find_package 或 FetchContent 引入
add_executable(sim_behavior_tests
  test_main.cpp
  test_cancellation_token.cpp
  test_result_mailbox.cpp
  test_entity_context.cpp
  test_async_action_base.cpp
  # 跨库交互边界测试：TbbJobExecutor ↔ DefaultResultMailbox ↔ UvwEventLoopRuntime
  test_cross_library_integration.cpp
  # Phase 2 多实体：Blackboard ctx 注入 + 实体隔离性 + 并发完成
  test_multi_entity.cpp
  # Phase 2 GroupContext：编队共享状态 + AssignGroup/DisbandGroup
  test_group_context.cpp
  # Phase 3 BusAdapter：UDP 回环 + CommandBus 出站转发
  test_bus_adapter.cpp
  # Phase 4 性能治理：arena 隔离 + TickStats + kSkipIdle
  test_phase4_perf.cpp
  # ┌── Phase 5 TODO [P1] 方案 F — 新增测试文件 ────────────────────────────┐
  # │  下列测试文件需新建，用于覆盖当前盲区：                                │
  # │                                                                        │
  # │  test_e2e_async_action.cpp                                             │
  # │    端到端异步 action 集成测试：                                        │
  # │    SimHostApp + 真实 TBB + 真实 uvw + 自定义 AsyncActionBase 子类      │
  # │    验证完整链路：OnStart→SubmitCpuJob→TBB执行→Post→DrainAll            │
  # │    →RequestWakeup→Phase1 Tick→OnRunning→PeekResult→ConsumeResult       │
  # │    应包含以下用例：                                                    │
  # │    - 单实体 job 正常完成，OnRunning 收到 result                        │
  # │    - 超时与 job 完成的竞态（job 1ms 完成 vs 2ms timeout）              │
  # │    - Halt 取消 in-flight job，result 到达后不被消费                     │
  # │    - 并发多实体各自独立完成，结果不串扰                                │
  # │                                                                        │
  # │  test_phase5_perf.cpp                                                  │
  # │    Phase 5 优化验证测试：                                              │
  # │    - WakeupDedup: 同一实体多次 RequestWakeup → Phase 1 只 Tick 一次    │
  # │    - ActiveSet: 100 实体 + kSkipIdle，只有 active 子集被 Tick          │
  # │    - LockFreeTickAll: Phase 2 期间 RequestWakeup 不被阻塞             │
  # │      （测量 RequestWakeup 延迟 < 10μs，即使 200 棵树在 Tick）         │
  # │    - MailboxBatchDrain: 50 个 result 批量 drain 只加锁 2 次            │
  # │    - DiscardGhostResult: Discard 后 DrainAll 不再写入 ready_          │
  # │    - ScaleTest: 200 实体 × 20Hz × 3 秒持续运行无死锁无泄漏           │
  # └───────────────────────────────────────────────────────────────────────┘
)

target_include_directories(sim_behavior_tests
  PRIVATE
    ${SIMBEHAVIOR_INCLUDE_DIR}
    # 允许测试直接包含 src/ 内部头文件（如 default_result_mailbox.hpp）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(sim_behavior_tests
  PRIVATE
    simbehavior_sim_host          # Phase 2/3: SimHostApp（含 AssignGroup/DisbandGroup）
    simbehavior_bt_nodes
    simbehavior_bt_runtime
    simbehavior_async_runtime
    simbehavior_compute_runtime
    simbehavior_domain
    simbehavior_adapters
    GTest::gtest
    GTest::gtest_main
    simbehavior_compile_warnings
)

include(GoogleTest)
gtest_discover_tests(sim_behavior_tests)
