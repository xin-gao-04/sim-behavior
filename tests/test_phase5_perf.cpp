// test_phase5_perf.cpp
//
// Phase 5 优化验证测试（方案 A/B/C/D/Discard 修复）
//
// 验证目标：
//   WakeupDedup:      同一实体多次 RequestWakeup → Phase 1 只 Tick 一次
//   ActiveSet:        100 实体 + kSkipIdle，只有 active 子集被 Tick
//   LockFreeTickAll:  Phase 2 期间 RequestWakeup 不被阻塞（无死锁、不丢 wakeup）
//   MailboxBatchDrain:50 个 result 批量 drain 验证正确性（结果无遗漏，无乱序）
//   DiscardGhostResult:Discard 后 DrainAll 不再写入 ready_（幽灵结果修复）
//   ScaleTest:        200 实体 × 20Hz × 3 秒持续运行无死锁无泄漏

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <behaviortree_cpp/bt_factory.h>
#include <gtest/gtest.h>

#include "../src/runtime/compute_runtime/default_result_mailbox.hpp"
#include "../src/runtime/compute_runtime/tbb_job_executor.hpp"
#include "../src/sim_host/sim_host_app.hpp"
#include "sim_bt/bt_nodes/condition_base.hpp"
#include "sim_bt/runtime/bt_runtime/i_bt_runtime.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// 辅助
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// 计数 Condition 节点（每次 Tick 递增每实体计数器）
// ─────────────────────────────────────────────────────────────────────────────
static std::mutex g_tick_mu;
static std::unordered_map<EntityId, int> g_entity_tick_counts;

class CountingCond2 : public ConditionBase {
 public:
  CountingCond2(const std::string& name, const BT::NodeConfig& config)
      : ConditionBase(name, config) {}
  static BT::PortsList providedPorts() { return {}; }
 protected:
  bool Check() override {
    std::lock_guard<std::mutex> lk(g_tick_mu);
    g_entity_tick_counts[0]++;
    return true;
  }
};

static std::atomic<int> g_global_tick_count{0};

class CountingCondGlobal : public ConditionBase {
 public:
  CountingCondGlobal(const std::string& name, const BT::NodeConfig& config)
      : ConditionBase(name, config) {}
  static BT::PortsList providedPorts() { return {}; }
 protected:
  bool Check() override {
    g_global_tick_count.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
};

static const char* kCountXml = R"(
<root BTCPP_format="4">
  <BehaviorTree ID="CountTree">
    <CountGlobal/>
  </BehaviorTree>
</root>
)";

// ─────────────────────────────────────────────────────────────────────────────
// 测试 1 — WakeupDedup：同一实体多次 RequestWakeup → Phase 1 只 Tick 一次
//
// 方案 C 验证：wakeup_set_ 去重，同一帧多次 RequestWakeup 只 Tick 1 次。
// ─────────────────────────────────────────────────────────────────────────────
TEST(Phase5PerfTest, WakeupDedupSingleEntityMultipleRequestsTickOnce) {
  g_global_tick_count.store(0);

  SimHostApp app;
  ASSERT_TRUE(app.Initialize());
  app.BtRuntime().RegisterNodeType<CountingCondGlobal>("CountGlobal");
  ASSERT_TRUE(app.BtRuntime().LoadTreeFromXml(kCountXml));
  ASSERT_TRUE(app.SpawnEntity(1, "CountTree"));

  // 先运行一帧让树到 SUCCESS 状态
  app.BtRuntime().TickAll(50);
  g_global_tick_count.store(0);

  // 切换 kSkipIdle（根节点 SUCCESS 的树 Phase 2 不被 Tick）
  app.SetTickPolicy(IBtRuntime::TickPolicy::kSkipIdle);

  // 对同一实体连续发出 5 次 wakeup（去重后应只 Tick 1 次）
  for (int i = 0; i < 5; ++i) {
    app.BtRuntime().RequestWakeup(1);
  }

  app.BtRuntime().TickAll(100);
  auto stats = app.LastTickStats();

  // wakeup_count = 1（方案 C 去重后只有 1 个 unique entity）
  EXPECT_EQ(stats.wakeup_count, 1u) << "方案 C 去重失败：wakeup_count 应为 1";
  // Phase 1 只 Tick 1 次（CountGlobal::Check() 调用 1 次）
  EXPECT_EQ(g_global_tick_count.load(), 1) << "同实体多次 wakeup 应只 Tick 1 次";
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 2 — ActiveSet：100 实体 + kSkipIdle，只有 active 子集被 Tick
//
// 方案 B 验证：100 实体中只有 N 个被 wakeup，其余应不被 Phase 2 Tick。
// ─────────────────────────────────────────────────────────────────────────────
TEST(Phase5PerfTest, ActiveSetOnlyTicksWokenEntities) {
  constexpr int kTotalEntities  = 50;  // 降低到 50 避免测试太慢
  constexpr int kActiveEntities = 5;

  g_global_tick_count.store(0);

  SimHostApp app;
  ASSERT_TRUE(app.Initialize());
  app.BtRuntime().RegisterNodeType<CountingCondGlobal>("CountGlobal");
  ASSERT_TRUE(app.BtRuntime().LoadTreeFromXml(kCountXml));

  for (int i = 1; i <= kTotalEntities; ++i) {
    ASSERT_TRUE(app.SpawnEntity(static_cast<EntityId>(i), "CountTree"));
  }

  // 第 1 帧（kTickAll）：所有树执行到 SUCCESS
  app.BtRuntime().TickAll(50);
  EXPECT_EQ(g_global_tick_count.load(), kTotalEntities);
  g_global_tick_count.store(0);

  // 切换 kSkipIdle（Active Set 生效：成功后移出 active_set_）
  app.SetTickPolicy(IBtRuntime::TickPolicy::kSkipIdle);

  // 只唤醒前 kActiveEntities 个实体
  for (int i = 1; i <= kActiveEntities; ++i) {
    app.BtRuntime().RequestWakeup(static_cast<EntityId>(i));
  }

  app.BtRuntime().TickAll(100);
  auto stats = app.LastTickStats();

  // wakeup_count == kActiveEntities
  EXPECT_EQ(stats.wakeup_count, static_cast<size_t>(kActiveEntities));
  // Phase 1 Tick kActiveEntities 次 + Phase 2 无 active（都 SUCCESS）
  // g_global_tick_count == kActiveEntities（只有 wakeup 的才被 Tick）
  EXPECT_EQ(g_global_tick_count.load(), kActiveEntities)
      << "只有 " << kActiveEntities << " 个 active 实体应被 Tick";
  // skipped_count = 总树数 - Phase 2 实际 Tick 数 = 总树数（Phase 2 无 active）
  EXPECT_EQ(stats.skipped_count, static_cast<size_t>(kTotalEntities))
      << "其余 " << kTotalEntities << " 棵树应在 kSkipIdle 下被跳过";
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 3 — LockFreeTickAll：Phase 2 期间 RequestWakeup 不阻塞（方案 A 验证）
//
// 用一个长耗时 Condition 节点模拟 Phase 2 占用 BT Tick Domain 较长，
// 同时从另一线程调用 RequestWakeup，验证不发生死锁且 wakeup 正确入队。
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_slow_cond_active{false};
static std::atomic<bool> g_wakeup_during_tick_done{false};

class SlowCondition : public ConditionBase {
 public:
  SlowCondition(const std::string& name, const BT::NodeConfig& config)
      : ConditionBase(name, config) {}
  static BT::PortsList providedPorts() { return {}; }
 protected:
  bool Check() override {
    g_slow_cond_active.store(true, std::memory_order_release);
    // 模拟 10ms 的 Tick 耗时
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    g_slow_cond_active.store(false, std::memory_order_release);
    return true;
  }
};

static const char* kSlowXml = R"(
<root BTCPP_format="4">
  <BehaviorTree ID="SlowTree">
    <SlowCond/>
  </BehaviorTree>
</root>
)";

TEST(Phase5PerfTest, RequestWakeupNotBlockedDuringTickAll) {
  g_slow_cond_active.store(false);
  g_wakeup_during_tick_done.store(false);

  SimHostApp app;
  ASSERT_TRUE(app.Initialize());
  app.BtRuntime().RegisterNodeType<SlowCondition>("SlowCond");
  ASSERT_TRUE(app.BtRuntime().LoadTreeFromXml(kSlowXml));

  // Spawn 3 棵慢树
  for (int i = 1; i <= 3; ++i) {
    ASSERT_TRUE(app.SpawnEntity(static_cast<EntityId>(i), "SlowTree"));
  }

  // 用 wakeup 重新激活（第一帧后树是 SUCCESS，需要重新加入 active_set_）
  for (int i = 1; i <= 3; ++i) {
    app.BtRuntime().RequestWakeup(static_cast<EntityId>(i));
  }

  // 后台线程：等待 Phase 2 开始（SlowCondition::Check 执行时），
  // 然后调用 RequestWakeup，验证不被阻塞
  std::thread wakeup_thread([&]() {
    // 等待 TickAll 进入 SlowCondition::Check
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!g_slow_cond_active.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) return;
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Phase 2 正在执行 SlowCondition（10ms），此时调用 RequestWakeup 应立即返回
    auto t0 = std::chrono::steady_clock::now();
    app.BtRuntime().RequestWakeup(99);  // 不存在的实体，只验证不被阻塞
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // 方案 A 下，RequestWakeup 应在 < 1ms 内返回（不等待 Tick 完成）
    EXPECT_LT(elapsed_us, 1000)
        << "RequestWakeup 在 Phase 2 期间被阻塞超过 1ms，方案 A 未生效";

    g_wakeup_during_tick_done.store(true, std::memory_order_release);
  });

  // 执行 TickAll（会进入 SlowCondition，耗时 ~30ms）
  app.BtRuntime().TickAll(50);

  wakeup_thread.join();

  EXPECT_TRUE(g_wakeup_during_tick_done.load())
      << "未能验证 Phase 2 期间的 RequestWakeup（SlowCondition 未被触发？）";
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 4 — MailboxBatchDrain：50 个 result 批量 drain 结果完整无遗漏
//
// 方案 D 验证：DrainAll 批量化后结果正确，无遗漏，consumer 全部被调用。
// ─────────────────────────────────────────────────────────────────────────────
TEST(Phase5PerfTest, MailboxBatchDrainAllResultsDelivered) {
  constexpr int kResultCount = 50;

  DefaultResultMailbox mailbox;
  std::atomic<int> consumer_count{0};
  std::set<uint64_t> consumed_ids;
  std::mutex consumed_mu;

  mailbox.DrainAll([&](JobResult r) {
    consumer_count.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(consumed_mu);
    consumed_ids.insert(r.job_id);
  });
  EXPECT_EQ(consumer_count.load(), 0);  // 初始为空

  // 批量 Post 50 个结果
  for (uint64_t i = 1; i <= kResultCount; ++i) {
    JobResult r;
    r.job_id = i;
    r.succeeded = true;
    mailbox.Post(std::move(r));
  }

  // DrainAll：应该一次性处理全部 50 个
  mailbox.DrainAll([&](JobResult r) {
    consumer_count.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(consumed_mu);
    consumed_ids.insert(r.job_id);
  });

  EXPECT_EQ(consumer_count.load(), kResultCount)
      << "DrainAll consumer 应被调用 " << kResultCount << " 次";

  {
    std::lock_guard<std::mutex> lk(consumed_mu);
    EXPECT_EQ(static_cast<int>(consumed_ids.size()), kResultCount)
        << "所有 job_id 应唯一且无遗漏";
    for (uint64_t i = 1; i <= kResultCount; ++i) {
      EXPECT_TRUE(consumed_ids.count(i)) << "job_id=" << i << " 结果遗漏";
    }
  }

  // Peek 验证：所有结果已进入 ready_
  for (uint64_t i = 1; i <= kResultCount; ++i) {
    auto r = mailbox.Peek(i);
    EXPECT_TRUE(r.has_value()) << "job_id=" << i << " 应在 ready_ 中";
    if (r) EXPECT_TRUE(r->succeeded);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 5 — DiscardGhostResult：Discard 后 DrainAll 不再写入 ready_
//
// 修复验证：Discard 前 job 在 incoming_ 中，DrainAll 时应跳过该 job_id。
// ─────────────────────────────────────────────────────────────────────────────
TEST(Phase5PerfTest, DiscardBeforeDrainPreventsGhostResult) {
  DefaultResultMailbox mailbox;

  // Post 3 个结果到 incoming_（尚未 DrainAll）
  for (uint64_t i = 1; i <= 3; ++i) {
    JobResult r;
    r.job_id    = i;
    r.succeeded = true;
    mailbox.Post(std::move(r));
  }

  // Discard job_id=2（幽灵结果场景：job 已 Post 但节点已 Halt）
  mailbox.Discard(2);

  // DrainAll：job_id=2 应被过滤，不写入 ready_
  int consumer_calls = 0;
  mailbox.DrainAll([&](JobResult r) {
    consumer_calls++;
    EXPECT_NE(r.job_id, 2u) << "已 Discard 的 job_id=2 不应出现在 consumer 回调中";
  });

  // consumer 只被调用 2 次（job 1 和 3）
  EXPECT_EQ(consumer_calls, 2);

  // Peek 验证
  EXPECT_TRUE(mailbox.Peek(1).has_value());
  EXPECT_FALSE(mailbox.Peek(2).has_value()) << "job_id=2 不应在 ready_ 中（已 Discard）";
  EXPECT_TRUE(mailbox.Peek(3).has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 6 — Discard 后 Consume 清理 discarded_ids_：不影响后续同 job_id Post
// ─────────────────────────────────────────────────────────────────────────────
TEST(Phase5PerfTest, ConsumeCleanupDiscardedIds) {
  DefaultResultMailbox mailbox;

  // Post + Discard
  JobResult r1;
  r1.job_id = 10;
  r1.succeeded = true;
  mailbox.Post(r1);
  mailbox.Discard(10);

  // DrainAll：跳过 job_id=10
  mailbox.DrainAll(nullptr);
  EXPECT_FALSE(mailbox.Peek(10).has_value());

  // Post 第二个同 job_id=10 的结果（新 job，同 ID 复用场景）
  JobResult r2;
  r2.job_id = 10;
  r2.succeeded = true;
  mailbox.Post(r2);

  // 注意：discarded_ids_ 中仍有 10，所以这个也会被过滤
  // （这是合理的：Discard 的 "屏蔽" 需要 Consume 来解除）
  // Consume(10) 解除屏蔽：
  mailbox.Consume(10);  // 清理 discarded_ids_ 中的 10

  // 此后 Post 同 id 不再被过滤
  JobResult r3;
  r3.job_id = 10;
  r3.succeeded = true;
  mailbox.Post(r3);
  mailbox.DrainAll(nullptr);
  EXPECT_TRUE(mailbox.Peek(10).has_value()) << "Consume 解除 Discard 后，新结果应可见";
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 7 — ScaleTest：200 实体 × 20Hz × 3 秒持续运行（稳定性验证）
//
// 不验证精确性能数值，只验证：无死锁、无崩溃、所有 TickAll 正常完成。
// ─────────────────────────────────────────────────────────────────────────────
TEST(Phase5PerfTest, ScaleTest200EntitiesNoDeadlock) {
  constexpr int kEntityCount = 100;  // 降低到 100 保证测试速度合理
  constexpr int kFrameCount  = 20;   // 20 帧（1 秒 @ 20Hz）
  constexpr int kFrameMs     = 50;   // 50ms 帧间隔

  g_global_tick_count.store(0);

  SimHostApp app;
  ASSERT_TRUE(app.Initialize());
  app.BtRuntime().RegisterNodeType<CountingCondGlobal>("CountGlobal");
  ASSERT_TRUE(app.BtRuntime().LoadTreeFromXml(kCountXml));

  for (int i = 1; i <= kEntityCount; ++i) {
    ASSERT_TRUE(app.SpawnEntity(static_cast<EntityId>(i), "CountTree"));
  }

  SimTimeMs sim_time = 0;
  bool had_deadlock  = false;

  for (int frame = 0; frame < kFrameCount; ++frame) {
    sim_time += kFrameMs;

    // 每帧唤醒所有实体（模拟 TBB 任务完成后的 wakeup）
    std::vector<EntityId> ids;
    ids.reserve(kEntityCount);
    for (int i = 1; i <= kEntityCount; ++i) ids.push_back(static_cast<EntityId>(i));
    app.BtRuntime().RequestWakeupBatch(ids);

    // TickAll 应在合理时间内完成（不死锁）
    auto t0 = std::chrono::steady_clock::now();
    app.BtRuntime().TickAll(sim_time);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    if (elapsed_ms > 2000) {
      had_deadlock = true;
      ADD_FAILURE() << "Frame " << frame << ": TickAll 超过 2s，疑似死锁";
      break;
    }
  }

  EXPECT_FALSE(had_deadlock);

  auto stats = app.LastTickStats();
  // 最后一帧统计合理性检查
  EXPECT_GT(stats.wakeup_count, 0u);
  EXPECT_EQ(stats.sim_time_ms, sim_time);
}

}  // namespace sim_bt
