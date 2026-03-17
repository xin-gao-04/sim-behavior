// test_phase4_perf.cpp
//
// Phase 4 性能治理集成测试
//
// 验证目标：
//   1. 多 Arena 隔离性
//      提交到 high/normal/low 三个 arena 的任务互相隔离；
//      high arena 满负荷时不阻塞 normal arena 的提交与完成。
//
//   2. TickStats 每帧耗时采样
//      TickAll 执行后 LastTickStats() 正确记录仿真时间、耗时和树数量。
//
//   3. kSkipIdle 批量 Tick 优化
//      设置 kSkipIdle 策略后，根节点非 RUNNING 的树被计入 skipped_count，
//      实际 tick 数量为 tree_count（不含跳过部分）。
//
//   4. Arena 并发完成计数
//      向三个 arena 并发提交多批任务，所有任务均成功投递且结果被 mailbox 接收。

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <behaviortree_cpp/bt_factory.h>
#include <gtest/gtest.h>

#include "../src/runtime/compute_runtime/tbb_job_executor.hpp"
#include "../src/sim_host/sim_host_app.hpp"
#include "sim_bt/bt_nodes/condition_base.hpp"
#include "sim_bt/runtime/bt_runtime/i_bt_runtime.hpp"
#include "sim_bt/runtime/compute_runtime/i_job_executor.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// 辅助：自旋等待原子条件，带超时
// ─────────────────────────────────────────────────────────────────────────────
static bool WaitFor(std::function<bool()> pred,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试用 Condition 节点：每次 Check() 都返回 true（树永远 RUNNING 需要 Sequence 包装）
// 用于 TickStats 和 SkipIdle 测试
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<int> g_condition_tick_count{0};

class CountingCondition : public ConditionBase {
 public:
  CountingCondition(const std::string& name, const BT::NodeConfig& config)
      : ConditionBase(name, config) {}
  static BT::PortsList providedPorts() { return {}; }
 protected:
  bool Check() override {
    g_condition_tick_count.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 — Arena 隔离性：向三个 arena 并发提交任务，所有任务均完成
// ─────────────────────────────────────────────────────────────────────────────
TEST(ArenaIsolationTest, AllArenasCompleteIndependently) {
  TbbJobExecutor executor(ArenaConfig{2, 4, 2});

  constexpr int kJobsPerArena = 10;
  std::atomic<int> completed{0};

  auto make_job = [&](JobPriority priority, int owner_id) {
    JobDescriptor desc;
    desc.priority     = priority;
    desc.owner_entity = static_cast<EntityId>(owner_id);
    desc.task = [&completed](std::shared_ptr<ICancellationToken>, JobResult& r) {
      r.succeeded = true;
      completed.fetch_add(1, std::memory_order_relaxed);
    };
    return desc;
  };

  // 向三个 arena 各提交 kJobsPerArena 个任务
  for (int i = 0; i < kJobsPerArena; ++i) {
    executor.Submit(make_job(JobPriority::kHigh,   1000 + i));
    executor.Submit(make_job(JobPriority::kNormal, 2000 + i));
    executor.Submit(make_job(JobPriority::kLow,    3000 + i));
  }

  // 等待所有任务完成（最多 5 秒）
  EXPECT_TRUE(WaitFor([&]{ return completed.load() == kJobsPerArena * 3; }));
  EXPECT_EQ(completed.load(), kJobsPerArena * 3);

  executor.Shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 — Arena 隔离性：low arena 饱和时 high arena 任务仍可完成
// ─────────────────────────────────────────────────────────────────────────────
TEST(ArenaIsolationTest, HighArenaNotBlockedByLowArenaSaturation) {
  // low arena 2 个 worker，normal arena 4 个，high arena 2 个
  TbbJobExecutor executor(ArenaConfig{2, 4, 2});

  std::atomic<bool>  low_release{false};
  std::atomic<int>   high_done{0};

  // 先用 2 个阻塞 job 填满 low arena
  for (int i = 0; i < 2; ++i) {
    JobDescriptor desc;
    desc.priority     = JobPriority::kLow;
    desc.owner_entity = 900 + i;
    desc.task = [&low_release](std::shared_ptr<ICancellationToken>, JobResult& r) {
      // 阻塞直到 low_release 置 true
      while (!low_release.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      r.succeeded = true;
    };
    executor.Submit(desc);
  }

  // 再向 high arena 提交 3 个快速任务
  for (int i = 0; i < 3; ++i) {
    JobDescriptor desc;
    desc.priority     = JobPriority::kHigh;
    desc.owner_entity = 800 + i;
    desc.task = [&high_done](std::shared_ptr<ICancellationToken>, JobResult& r) {
      r.succeeded = true;
      high_done.fetch_add(1, std::memory_order_relaxed);
    };
    executor.Submit(desc);
  }

  // high arena 任务应在 1 秒内完成（不被 low arena 阻塞）
  EXPECT_TRUE(WaitFor([&]{ return high_done.load() == 3; },
                      std::chrono::milliseconds(1000)));

  // 解除 low arena 阻塞，让 executor 干净关闭
  low_release.store(true, std::memory_order_relaxed);
  executor.Shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 — TickStats：TickAll 正确记录仿真时间与树数量
// ─────────────────────────────────────────────────────────────────────────────

// 最简单的 BT XML：一个 Condition 节点作为根
static const char* kSimpleCondXml = R"(
<root BTCPP_format="4">
  <BehaviorTree ID="SimpleCond">
    <CountingCond/>
  </BehaviorTree>
</root>
)";

TEST(TickStatsTest, StatsRecordedAfterTickAll) {
  g_condition_tick_count.store(0);

  SimHostApp app;
  ASSERT_TRUE(app.Initialize());
  app.BtRuntime().RegisterNodeType<CountingCondition>("CountingCond");
  ASSERT_TRUE(app.BtRuntime().LoadTreeFromXml(kSimpleCondXml));

  // Spawn 3 个实体
  for (EntityId id = 1; id <= 3; ++id) {
    ASSERT_TRUE(app.SpawnEntity(id, "SimpleCond"));
  }

  // 手动调用一次 TickAll
  constexpr SimTimeMs kSimTime = 100;
  app.BtRuntime().TickAll(kSimTime);

  auto stats = app.LastTickStats();
  EXPECT_EQ(stats.sim_time_ms, kSimTime);
  EXPECT_GE(stats.duration_us, 0);       // 耗时非负
  EXPECT_EQ(stats.tree_count, 3u);       // 3 棵树被 tick
  EXPECT_EQ(stats.skipped_count, 0u);    // kTickAll 下无跳过
  EXPECT_EQ(stats.wakeup_count, 0u);     // 无 wakeup

  // CountingCondition 被 tick 了 3 次
  EXPECT_EQ(g_condition_tick_count.load(), 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 — TickStats：多帧累积，sim_time 每帧递增
// ─────────────────────────────────────────────────────────────────────────────
TEST(TickStatsTest, SimTimeIncreasesAcrossFrames) {
  SimHostApp app;
  ASSERT_TRUE(app.Initialize());
  app.BtRuntime().RegisterNodeType<CountingCondition>("CountingCond");
  ASSERT_TRUE(app.BtRuntime().LoadTreeFromXml(kSimpleCondXml));
  ASSERT_TRUE(app.SpawnEntity(1, "SimpleCond"));

  app.BtRuntime().TickAll(50);
  EXPECT_EQ(app.LastTickStats().sim_time_ms, 50u);

  app.BtRuntime().TickAll(100);
  EXPECT_EQ(app.LastTickStats().sim_time_ms, 100u);

  app.BtRuntime().TickAll(150);
  EXPECT_EQ(app.LastTickStats().sim_time_ms, 150u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 — kSkipIdle：根节点非 RUNNING 时计入 skipped_count
//
// CountingCondition::Check() 返回 true → 树立刻 SUCCESS（根节点不会是 RUNNING）。
// kSkipIdle 策略下，Phase 2 tick 循环应该跳过这些树。
// ─────────────────────────────────────────────────────────────────────────────
TEST(SkipIdleTest, IdleTreesSkippedUnderSkipIdlePolicy) {
  g_condition_tick_count.store(0);

  SimHostApp app;
  ASSERT_TRUE(app.Initialize());
  app.BtRuntime().RegisterNodeType<CountingCondition>("CountingCond");
  ASSERT_TRUE(app.BtRuntime().LoadTreeFromXml(kSimpleCondXml));

  for (EntityId id = 1; id <= 4; ++id) {
    ASSERT_TRUE(app.SpawnEntity(id, "SimpleCond"));
  }

  // 先用 kTickAll 跑一帧，让树执行到 SUCCESS 状态
  app.BtRuntime().TickAll(50);
  EXPECT_EQ(app.LastTickStats().tree_count, 4u);
  EXPECT_EQ(app.LastTickStats().skipped_count, 0u);

  int ticks_after_first = g_condition_tick_count.load();

  // 切换到 kSkipIdle 策略
  app.SetTickPolicy(IBtRuntime::TickPolicy::kSkipIdle);
  g_condition_tick_count.store(0);

  // 再 tick 一帧：4 棵树根节点均为 SUCCESS，应全部被跳过
  app.BtRuntime().TickAll(100);
  auto stats = app.LastTickStats();
  EXPECT_EQ(stats.skipped_count, 4u);
  EXPECT_EQ(stats.tree_count, 0u);      // 无树被实际 tick
  EXPECT_EQ(g_condition_tick_count.load(), 0);  // Check() 未被调用

  (void)ticks_after_first;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6 — kSkipIdle：wakeup 仍强制 tick（绕过 skip 逻辑）
//
// wakeup 队列在 Phase 1 处理，早于 Phase 2 的 skip-idle 检查，
// 因此被唤醒的实体在 Phase 1 会被 tick（即使根节点非 RUNNING）。
// Phase 2 遇到同一实体仍会因 kSkipIdle 跳过（符合预期）。
// ─────────────────────────────────────────────────────────────────────────────
TEST(SkipIdleTest, WakeupStillTickedInPhaseOne) {
  g_condition_tick_count.store(0);

  SimHostApp app;
  ASSERT_TRUE(app.Initialize());
  app.BtRuntime().RegisterNodeType<CountingCondition>("CountingCond");
  ASSERT_TRUE(app.BtRuntime().LoadTreeFromXml(kSimpleCondXml));

  // Spawn 2 个实体
  ASSERT_TRUE(app.SpawnEntity(10, "SimpleCond"));
  ASSERT_TRUE(app.SpawnEntity(20, "SimpleCond"));

  // 跑一帧让树进入 SUCCESS
  app.BtRuntime().TickAll(50);

  // 切换 kSkipIdle
  app.SetTickPolicy(IBtRuntime::TickPolicy::kSkipIdle);
  g_condition_tick_count.store(0);

  // 对实体 10 发起 wakeup
  app.BtRuntime().RequestWakeup(10);

  app.BtRuntime().TickAll(100);
  auto stats = app.LastTickStats();

  // wakeup_count == 1（实体 10 从 wakeup 队列处理）
  EXPECT_EQ(stats.wakeup_count, 1u);
  // Phase 2 两棵树均跳过（roots 不是 RUNNING）
  EXPECT_EQ(stats.skipped_count, 2u);
  // Check() 在 Phase 1 被调用了 1 次（实体 10 的 wakeup tick）
  EXPECT_EQ(g_condition_tick_count.load(), 1);
}

}  // namespace sim_bt
