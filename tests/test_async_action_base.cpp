// test_async_action_base.cpp
//
// AsyncActionBase 单元测试
//
// 测试策略：
//   - 用轻量 Mock 替代真实 uvw/TBB 依赖，专注节点状态机逻辑
//   - 验证 onStart/onRunning/onHalted 的正确委托与超时路径
//   - 使用真实 BT::StatefulActionNode 基础设施（需要 BT::NodeConfig）

#include <gtest/gtest.h>
#include <behaviortree_cpp/action_node.h>
#include <behaviortree_cpp/bt_factory.h>
#include <behaviortree_cpp/blackboard.h>

#include "sim_bt/bt_nodes/async_action_base.hpp"
#include "sim_bt/runtime/compute_runtime/i_job_handle.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// Mock: IJobHandle 存根（SubmitCpuJob 的返回值）
// ─────────────────────────────────────────────────────────────────────────────
class StubJobHandle : public IJobHandle {
 public:
  explicit StubJobHandle(uint64_t id) : id_(id) {}
  JobState   State()  const override { return JobState::kCompleted; }
  void       Cancel()       override {}
  uint64_t   JobId()  const override { return id_; }
  CancellationTokenPtr Token() const override { return nullptr; }
 private:
  uint64_t id_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Mock: IAsyncActionContext — 完全可控的测试替身
// ─────────────────────────────────────────────────────────────────────────────
class MockAsyncActionContext : public IAsyncActionContext {
 public:
  // ── 可配置行为 ──────────────────────────────────────────────────────────────
  bool                         is_timed_out_       = false;
  EntityId                     owner_entity_       = 42;
  SimTimeMs                    current_sim_time_   = 1000;
  std::optional<JobResult>     peek_result_value_  = std::nullopt;
  uint64_t                     next_job_id_        = 1;

  // ── 调用计数（用于断言） ────────────────────────────────────────────────────
  int submit_job_calls_      = 0;
  int cancel_job_calls_      = 0;
  mutable int peek_result_calls_     = 0;
  int consume_result_calls_  = 0;
  int start_timeout_calls_   = 0;
  int cancel_timeout_calls_  = 0;
  int emit_wakeup_calls_     = 0;

  // ── 记录参数 ────────────────────────────────────────────────────────────────
  uint64_t                     last_cancelled_job_id_ = 0;
  mutable uint64_t             last_peeked_job_id_    = 0;
  uint64_t                     last_consumed_job_id_  = 0;
  std::chrono::milliseconds    last_timeout_ms_       {0};

  // ── IAsyncActionContext 实现 ────────────────────────────────────────────────

  JobHandlePtr SubmitCpuJob(
      JobPriority /*priority*/,
      std::function<void(CancellationTokenPtr, JobResult&)> /*task*/) override {
    ++submit_job_calls_;
    return std::make_shared<StubJobHandle>(next_job_id_++);
  }

  void CancelJob(uint64_t job_id) override {
    ++cancel_job_calls_;
    last_cancelled_job_id_ = job_id;
  }

  std::optional<JobResult> PeekResult(uint64_t job_id) const override {
    ++peek_result_calls_;
    last_peeked_job_id_ = job_id;
    return peek_result_value_;
  }

  void ConsumeResult(uint64_t job_id) override {
    ++consume_result_calls_;
    last_consumed_job_id_ = job_id;
  }

  void StartTimeout(std::chrono::milliseconds ms) override {
    ++start_timeout_calls_;
    last_timeout_ms_ = ms;
  }

  void CancelTimeout() override { ++cancel_timeout_calls_; }

  bool IsTimedOut() const override { return is_timed_out_; }

  void EmitWakeup() override { ++emit_wakeup_calls_; }

  EntityId   OwnerEntity()    const override { return owner_entity_; }
  SimTimeMs  CurrentSimTime() const override { return current_sim_time_; }
};

// ─────────────────────────────────────────────────────────────────────────────
// 测试用具体节点 —— 完全可配置的 AsyncActionBase 子类
// ─────────────────────────────────────────────────────────────────────────────
class ConfigurableAsyncAction : public AsyncActionBase {
 public:
  using AsyncActionBase::AsyncActionBase;

  // 可配置的返回值
  NodeStatus on_start_return_   = NodeStatus::kRunning;
  NodeStatus on_running_return_ = NodeStatus::kRunning;

  // 调用计数
  int on_start_calls_   = 0;
  int on_running_calls_ = 0;
  int on_halted_calls_  = 0;

  NodeStatus OnStart()   override { ++on_start_calls_;   return on_start_return_;   }
  NodeStatus OnRunning() override { ++on_running_calls_; return on_running_return_; }
  void       OnHalted()  override { ++on_halted_calls_;                              }

  // 公开代理：绕过 protected 访问限制，供测试调用
  EntityId PublicOwnerEntity() const { return OwnerEntity(); }

  static BT::PortsList providedPorts() { return {}; }
};

// ─────────────────────────────────────────────────────────────────────────────
// 辅助函数：构造独立节点（不需要完整 BT 工厂和 XML）
// ─────────────────────────────────────────────────────────────────────────────
static std::pair<std::unique_ptr<ConfigurableAsyncAction>,
                 std::shared_ptr<MockAsyncActionContext>>
MakeNode(const std::string& name = "TestAction") {
  auto ctx = std::make_shared<MockAsyncActionContext>();
  BT::NodeConfig config;
  config.blackboard = BT::Blackboard::create();
  auto node = std::make_unique<ConfigurableAsyncAction>(name, config, ctx);
  return {std::move(node), ctx};
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试：onStart() 正确委托到 OnStart()，返回值透传
// ─────────────────────────────────────────────────────────────────────────────
TEST(AsyncActionBase, OnStartDelegation_ReturnsRunning) {
  auto [node, ctx] = MakeNode();
  node->on_start_return_ = NodeStatus::kRunning;

  BT::NodeStatus result = node->executeTick();  // 首次 tick → onStart()

  EXPECT_EQ(result, BT::NodeStatus::RUNNING);
  EXPECT_EQ(node->on_start_calls_,   1);
  EXPECT_EQ(node->on_running_calls_, 0);
  EXPECT_EQ(node->on_halted_calls_,  0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试：OnStart 可以直接返回 SUCCESS（同步完成场景）
// ─────────────────────────────────────────────────────────────────────────────
TEST(AsyncActionBase, OnStartDelegation_ImmediateSuccess) {
  auto [node, ctx] = MakeNode();
  node->on_start_return_ = NodeStatus::kSuccess;

  BT::NodeStatus result = node->executeTick();

  EXPECT_EQ(result, BT::NodeStatus::SUCCESS);
  EXPECT_EQ(node->on_start_calls_, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试：第二次 tick 进入 onRunning() → 委托到 OnRunning()
// ─────────────────────────────────────────────────────────────────────────────
TEST(AsyncActionBase, OnRunningDelegation_CalledOnSubsequentTick) {
  auto [node, ctx] = MakeNode();
  node->on_start_return_   = NodeStatus::kRunning;
  node->on_running_return_ = NodeStatus::kRunning;

  node->executeTick();                          // 首次 → onStart
  BT::NodeStatus result = node->executeTick();  // 第二次 → onRunning

  EXPECT_EQ(result, BT::NodeStatus::RUNNING);
  EXPECT_EQ(node->on_start_calls_,   1);
  EXPECT_EQ(node->on_running_calls_, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试：OnRunning 返回 SUCCESS 时正确传递
// ─────────────────────────────────────────────────────────────────────────────
TEST(AsyncActionBase, OnRunningDelegation_ReturnsSuccess) {
  auto [node, ctx] = MakeNode();
  node->on_start_return_   = NodeStatus::kRunning;
  node->on_running_return_ = NodeStatus::kSuccess;

  node->executeTick();
  BT::NodeStatus result = node->executeTick();

  EXPECT_EQ(result, BT::NodeStatus::SUCCESS);
  EXPECT_EQ(node->on_running_calls_, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试：onHalted() 调用 CancelTimeout() 再调用 OnHalted()
// ─────────────────────────────────────────────────────────────────────────────
TEST(AsyncActionBase, OnHaltedDelegation_CancelsTimeoutAndCallsOnHalted) {
  auto [node, ctx] = MakeNode();
  node->on_start_return_ = NodeStatus::kRunning;

  node->executeTick();  // → RUNNING 状态
  node->haltNode();         // 显式 halt

  EXPECT_EQ(node->on_halted_calls_,     1);
  EXPECT_GE(ctx->cancel_timeout_calls_, 1);  // onHalted() 内部调用 CancelTimeout()
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试：IsTimedOut()==true 时 onRunning() 应返回 FAILURE 并调用 OnHalted()
// ─────────────────────────────────────────────────────────────────────────────
TEST(AsyncActionBase, TimeoutTriggersFailureAndCallsOnHalted) {
  auto [node, ctx] = MakeNode();
  node->on_start_return_   = NodeStatus::kRunning;
  node->on_running_return_ = NodeStatus::kRunning;  // 无超时时应返回 RUNNING

  node->executeTick();  // 进入 RUNNING

  ctx->is_timed_out_ = true;  // 模拟超时触发
  BT::NodeStatus result = node->executeTick();

  EXPECT_EQ(result, BT::NodeStatus::FAILURE);
  // 超时路径：OnHalted() 被调用（由 onRunning 内部处理）
  EXPECT_EQ(node->on_halted_calls_,  1);
  // 超时路径下 OnRunning() 不应被调用
  EXPECT_EQ(node->on_running_calls_, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试：OwnerEntity() 从 ctx 正确透传
// ─────────────────────────────────────────────────────────────────────────────
TEST(AsyncActionBase, OwnerEntityProxiesToContext) {
  auto [node, ctx] = MakeNode();
  ctx->owner_entity_ = 99;

  EXPECT_EQ(node->PublicOwnerEntity(), static_cast<EntityId>(99));
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试：多次 tick RUNNING → 最终 SUCCESS 完整状态机
// ─────────────────────────────────────────────────────────────────────────────
TEST(AsyncActionBase, FullLifecycle_RunningThenSuccess) {
  auto [node, ctx] = MakeNode();
  node->on_start_return_   = NodeStatus::kRunning;
  node->on_running_return_ = NodeStatus::kRunning;

  EXPECT_EQ(node->executeTick(), BT::NodeStatus::RUNNING);  // onStart
  EXPECT_EQ(node->executeTick(), BT::NodeStatus::RUNNING);  // onRunning #1
  EXPECT_EQ(node->executeTick(), BT::NodeStatus::RUNNING);  // onRunning #2

  node->on_running_return_ = NodeStatus::kSuccess;
  EXPECT_EQ(node->executeTick(), BT::NodeStatus::SUCCESS);  // onRunning #3 → done

  EXPECT_EQ(node->on_start_calls_,   1);
  EXPECT_EQ(node->on_running_calls_, 3);
  EXPECT_EQ(node->on_halted_calls_,  0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试：RUNNING 途中 halt → OnHalted 被调用一次
// ─────────────────────────────────────────────────────────────────────────────
TEST(AsyncActionBase, HaltDuringRunning_OnHaltedCalledOnce) {
  auto [node, ctx] = MakeNode();
  node->on_start_return_   = NodeStatus::kRunning;
  node->on_running_return_ = NodeStatus::kRunning;

  node->executeTick();
  node->executeTick();
  node->haltNode();

  EXPECT_EQ(node->on_halted_calls_, 1);
  EXPECT_EQ(node->on_start_calls_,  1);
}

}  // namespace sim_bt
