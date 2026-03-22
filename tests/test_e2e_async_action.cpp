// test_e2e_async_action.cpp
//
// 端到端异步 Action 集成测试（Phase 5 方案 F）
//
// 验证完整链路：
//   OnStart → SubmitCpuJob → TBB执行 → Post → DrainAll
//     → RequestWakeup → Phase1 Tick → OnRunning → PeekResult → ConsumeResult
//
// 使用真实 SimHostApp（TBB + uvw + BtRuntime），不使用 mock。
//
// 测试用例：
//   1. 单实体正常完成：OnRunning 收到结果，返回 SUCCESS
//   2. 超时竞态：job 完成前超时，节点返回 FAILURE
//   3. Halt 取消：in-flight job 被取消，结果到达后不被消费
//   4. 并发多实体：各自独立完成，结果不串扰

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <behaviortree_cpp/bt_factory.h>
#include <gtest/gtest.h>

#include "../src/sim_host/sim_host_app.hpp"
#include "sim_bt/bt_nodes/async_action_base.hpp"
#include "sim_bt/runtime/bt_runtime/i_bt_runtime.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// 共享计数器（跨测试用例，因 GTest 同进程运行）
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<int>  g_on_start_count{0};
static std::atomic<int>  g_on_running_success_count{0};
static std::atomic<int>  g_on_halted_count{0};
static std::atomic<int>  g_job_delay_ms{0};

// ─────────────────────────────────────────────────────────────────────────────
// TestAsyncAction — 可配置的异步动作节点
//
// 行为：
//   OnStart:   提交 TBB job（可选延迟），开始可选超时
//   OnRunning: 等待结果，成功则消费并返回 SUCCESS
//   OnHalted:  取消 job 和超时
// ─────────────────────────────────────────────────────────────────────────────
class TestAsyncAction : public AsyncActionBase {
 public:
  TestAsyncAction(const std::string& name, const BT::NodeConfig& config)
      : AsyncActionBase(name, config) {}

  static BT::PortsList providedPorts() { return {}; }

  NodeStatus OnStart() override {
    g_on_start_count.fetch_add(1, std::memory_order_relaxed);
    const int delay_ms = g_job_delay_ms.load(std::memory_order_relaxed);

    job_id_ = Ctx().SubmitCpuJob(
        JobPriority::kNormal,
        [delay_ms](CancellationTokenPtr tok, JobResult& out) {
          if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
          }
          if (tok->IsCancelled()) {
            out.succeeded = false;
            return;
          }
          out.succeeded = true;
          out.payload   = int(42);  // 携带简单 payload
        }
    )->JobId();

    return NodeStatus::kRunning;
  }

  NodeStatus OnRunning() override {
    auto r = Ctx().PeekResult(job_id_);
    if (!r) return NodeStatus::kRunning;
    Ctx().ConsumeResult(job_id_);
    if (r->succeeded) {
      g_on_running_success_count.fetch_add(1, std::memory_order_relaxed);
      return NodeStatus::kSuccess;
    }
    return NodeStatus::kFailure;
  }

  void OnHalted() override {
    g_on_halted_count.fetch_add(1, std::memory_order_relaxed);
    Ctx().CancelJob(job_id_);
  }

 private:
  uint64_t job_id_ = 0;
};

// 带超时的变体节点
class TestAsyncActionWithTimeout : public AsyncActionBase {
 public:
  TestAsyncActionWithTimeout(const std::string& name, const BT::NodeConfig& config)
      : AsyncActionBase(name, config) {}

  static BT::PortsList providedPorts() { return {}; }

  NodeStatus OnStart() override {
    g_on_start_count.fetch_add(1, std::memory_order_relaxed);
    const int delay_ms = g_job_delay_ms.load(std::memory_order_relaxed);

    job_id_ = Ctx().SubmitCpuJob(
        JobPriority::kNormal,
        [delay_ms](CancellationTokenPtr tok, JobResult& out) {
          std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
          out.succeeded = !tok->IsCancelled();
        }
    )->JobId();

    // 设置比 job 完成时间短的超时（5ms，job 需要 delay_ms = 100ms）
    Ctx().StartTimeout(std::chrono::milliseconds(5));
    return NodeStatus::kRunning;
  }

  NodeStatus OnRunning() override {
    auto r = Ctx().PeekResult(job_id_);
    if (!r) return NodeStatus::kRunning;
    Ctx().ConsumeResult(job_id_);
    Ctx().CancelTimeout();
    return r->succeeded ? NodeStatus::kSuccess : NodeStatus::kFailure;
  }

  void OnHalted() override {
    g_on_halted_count.fetch_add(1, std::memory_order_relaxed);
    Ctx().CancelJob(job_id_);
    Ctx().CancelTimeout();
  }

 private:
  uint64_t job_id_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// BT XML 模板
// ─────────────────────────────────────────────────────────────────────────────
static const char* kTestAsyncXml = R"(
<root BTCPP_format="4">
  <BehaviorTree ID="E2EAsyncTree">
    <TestAsync/>
  </BehaviorTree>
</root>
)";

static const char* kTestAsyncTimeoutXml = R"(
<root BTCPP_format="4">
  <BehaviorTree ID="E2EAsyncTimeoutTree">
    <TestAsyncTimeout/>
  </BehaviorTree>
</root>
)";

// ─────────────────────────────────────────────────────────────────────────────
// Fixture：每个测试独立的 SimHostApp
// ─────────────────────────────────────────────────────────────────────────────
class E2EAsyncActionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_on_start_count.store(0);
    g_on_running_success_count.store(0);
    g_on_halted_count.store(0);
    g_job_delay_ms.store(0);

    app_ = std::make_unique<SimHostApp>();
    ASSERT_TRUE(app_->Initialize());
    app_->BtRuntime().RegisterNodeType<TestAsyncAction>("TestAsync");
    app_->BtRuntime().RegisterNodeType<TestAsyncActionWithTimeout>("TestAsyncTimeout");
    ASSERT_TRUE(app_->BtRuntime().LoadTreeFromXml(kTestAsyncXml));
    ASSERT_TRUE(app_->BtRuntime().LoadTreeFromXml(kTestAsyncTimeoutXml));
  }

  void TearDown() override {
    app_.reset();  // 析构时 Shutdown 所有运行时
  }

  // 持续调用 TickAll 直到条件满足或超时。
  // BT 节点的 OnRunning 只在 TickAll 期间被调用，所以测试必须持续 tick。
  bool TickUntil(std::function<bool()> pred,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      current_time_ += 20;
      app_->BtRuntime().TickAll(current_time_);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
  }

  std::unique_ptr<SimHostApp> app_;
  SimTimeMs current_time_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// 测试 1 — 单实体正常完成：完整链路验证
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(E2EAsyncActionTest, SingleEntityJobCompletesSuccessfully) {
  g_job_delay_ms.store(0);  // 立即完成的 job

  ASSERT_TRUE(app_->SpawnEntity(1, "E2EAsyncTree"));

  // 第 1 帧：OnStart 执行，job 提交到 TBB
  current_time_ = 50;
  app_->BtRuntime().TickAll(current_time_);
  EXPECT_EQ(g_on_start_count.load(), 1);

  // 持续 Tick 直到 OnRunning 消费到结果（wakeup → Phase1 → OnRunning → SUCCESS）
  EXPECT_TRUE(TickUntil([&]{ return g_on_running_success_count.load() >= 1; }))
      << "OnRunning 未在 3s 内消费到结果";

  EXPECT_EQ(g_on_running_success_count.load(), 1);
  EXPECT_EQ(g_on_halted_count.load(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 2 — 超时竞态：超时先于 job 完成触发 FAILURE
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(E2EAsyncActionTest, TimeoutTriggersBeforeJobCompletion) {
  g_job_delay_ms.store(200);  // job 需要 200ms，timeout 设置为 5ms

  ASSERT_TRUE(app_->SpawnEntity(2, "E2EAsyncTimeoutTree"));

  // 第 1 帧：OnStart → job 提交 + 5ms 超时注册
  current_time_ = 50;
  app_->BtRuntime().TickAll(current_time_);
  EXPECT_EQ(g_on_start_count.load(), 1);

  // 持续 Tick：超时到达后 uvw 发出 RequestWakeup，下一帧 OnRunning 检测 timed_out_ → OnHalted
  EXPECT_TRUE(TickUntil([&]{ return g_on_halted_count.load() >= 1; }))
      << "超时未在 3s 内触发 OnHalted";

  EXPECT_EQ(g_on_halted_count.load(), 1);
  EXPECT_EQ(g_on_running_success_count.load(), 0);  // 超时路径不走 success
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 3 — Halt 取消：树被 Halt 后 job 结果不被消费
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(E2EAsyncActionTest, HaltCancelsInflightJob) {
  g_job_delay_ms.store(100);  // job 需要 100ms

  ASSERT_TRUE(app_->SpawnEntity(3, "E2EAsyncTree"));

  // 第 1 帧：OnStart
  current_time_ = 50;
  app_->BtRuntime().TickAll(current_time_);
  EXPECT_EQ(g_on_start_count.load(), 1);

  // 立即 Halt 树，job 仍在 TBB 中运行
  auto tree = app_->BtRuntime().GetTree(3);
  ASSERT_NE(tree, nullptr);
  tree->Halt();

  // 持续 Tick 让 Halt 在下帧被 BT 处理，触发 OnHalted
  EXPECT_TRUE(TickUntil([&]{ return g_on_halted_count.load() >= 1; },
                        std::chrono::milliseconds(2000)))
      << "Halt 后 OnHalted 未被调用";

  // 等待 job 完成（job 在 cancel 后仍会 Post 结果）
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // 即使结果到达，OnHalted 中已调用 CancelJob/Discard，结果不应被消费
  EXPECT_EQ(g_on_running_success_count.load(), 0)
      << "Halt 后不应消费 job 结果";
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 4 — 并发多实体：各自独立完成，结果不串扰
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(E2EAsyncActionTest, MultipleEntitiesCompleteIndependently) {
  constexpr int kEntityCount = 5;
  g_job_delay_ms.store(0);

  for (int i = 1; i <= kEntityCount; ++i) {
    ASSERT_TRUE(app_->SpawnEntity(static_cast<EntityId>(i * 10), "E2EAsyncTree"));
  }

  // 第 1 帧：所有实体 OnStart
  current_time_ = 50;
  app_->BtRuntime().TickAll(current_time_);
  EXPECT_EQ(g_on_start_count.load(), kEntityCount);

  // 持续 Tick 直到所有实体完成（结果各自通过 owner_entity 路由，互不干扰）
  EXPECT_TRUE(TickUntil(
      [&]{ return g_on_running_success_count.load() >= kEntityCount; },
      std::chrono::milliseconds(5000)))
      << "并发 " << kEntityCount << " 实体未在 5s 内全部完成";

  EXPECT_EQ(g_on_running_success_count.load(), kEntityCount);
  EXPECT_EQ(g_on_halted_count.load(), 0);
  // g_on_start_count 已在第 1 帧断言过（== kEntityCount），
  // 此后 kTickAll 模式会持续 tick 所有树导致 BT 复位，不在此重复断言。
}

}  // namespace sim_bt
