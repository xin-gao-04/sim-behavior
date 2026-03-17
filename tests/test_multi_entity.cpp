// test_multi_entity.cpp
//
// Phase 2 多实体集成测试
//
// 验证目标：
//   1. Blackboard 注入正确性
//      每个实体的 AsyncActionBase 节点拿到的 ctx.OwnerEntity() 与该实体 ID 一致，
//      不出现实体间 ctx 错乱。
//
//   2. 实体隔离性
//      N 个实体各自提交 TBB Job，每条 JobResult 通过 owner_entity 精确路由
//      回对应实体的 RequestWakeup，不出现跨实体交叉投递。
//
//   3. 并发完成
//      多实体的 TBB 任务在同一 worker pool 中真实并行，
//      全部结果在合理超时内到达（无串行阻塞）。
//
//   4. ConditionBase Blackboard 注入
//      SyncNodeContext 同样通过 Blackboard 正确注入，
//      Check() 能访问到对应实体的 EntityContext 数据。
//
// 使用真实的 TbbJobExecutor + UvwEventLoopRuntime + BtRuntimeImpl，不使用 mock。

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <behaviortree_cpp/bt_factory.h>
#include <gtest/gtest.h>

#include "../src/runtime/bt_runtime/async_action_context_impl.hpp"
#include "../src/runtime/bt_runtime/sync_node_context_impl.hpp"
#include "../src/runtime/async_runtime/uvw_event_loop_runtime.hpp"
#include "../src/runtime/compute_runtime/tbb_job_executor.hpp"
#include "../src/domain/entity/entity_context_impl.hpp"
#include "sim_bt/bt_nodes/async_action_base.hpp"
#include "sim_bt/bt_nodes/condition_base.hpp"
#include "sim_bt/runtime/bt_runtime/i_bt_runtime.hpp"
#include "sim_bt/adapters/i_command_bus.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// NullCommandBus — 测试用空实现（丢弃所有命令）
// ─────────────────────────────────────────────────────────────────────────────
class NullCommandBus : public ICommandBus {
 public:
  SimStatus Dispatch(ActionCommand) override { return SimStatus::Ok(); }
  void RegisterHandler(const std::string&, CommandHandler) override {}
  void ClearHandlers() override {}
};

// ─────────────────────────────────────────────────────────────────────────────
// 跨实体结果记录表（用于验证隔离性）
// ─────────────────────────────────────────────────────────────────────────────
struct CompletionRecord {
  EntityId owner_from_ctx;  // 节点 Ctx().OwnerEntity()
  uint64_t job_id;
};

static std::mutex                    g_mu;
static std::vector<CompletionRecord> g_completions;
static std::atomic<int>              g_success_count{0};

// ─────────────────────────────────────────────────────────────────────────────
// RecordingAsyncAction
//   - Blackboard 注入 ctx（不显式传入）
//   - OnStart   → 提交 CPU job
//   - OnRunning → PeekResult → SUCCESS，写入 g_completions
// ─────────────────────────────────────────────────────────────────────────────
class RecordingAsyncAction : public AsyncActionBase {
 public:
  RecordingAsyncAction(const std::string& name, const BT::NodeConfig& config)
      : AsyncActionBase(name, config) /* ctx = nullptr → 从 Blackboard 读取 */ {}

  static BT::PortsList providedPorts() { return {}; }

 protected:
  NodeStatus OnStart() override {
    job_handle_ = Ctx().SubmitCpuJob(
        JobPriority::kNormal,
        [](CancellationTokenPtr, JobResult& out) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          out.succeeded = true;
        });
    job_id_ = job_handle_ ? job_handle_->JobId() : 0;
    return NodeStatus::kRunning;
  }

  NodeStatus OnRunning() override {
    auto result = Ctx().PeekResult(job_id_);
    if (!result.has_value()) return NodeStatus::kRunning;

    // Count each entity at most once: BT.CPP resets the tree after SUCCESS
    // and the sequence could re-run, incrementing the counter a second time.
    {
      std::lock_guard<std::mutex> lk(g_mu);
      const EntityId owner = Ctx().OwnerEntity();
      bool already_recorded = false;
      for (const auto& rec : g_completions) {
        if (rec.owner_from_ctx == owner) { already_recorded = true; break; }
      }
      if (!already_recorded) {
        g_completions.push_back({owner, job_id_});
        g_success_count.fetch_add(1, std::memory_order_relaxed);
      }
    }
    Ctx().ConsumeResult(job_id_);
    return NodeStatus::kSuccess;
  }

  void OnHalted() override {
    if (job_handle_) job_handle_->Cancel();
  }

 private:
  JobHandlePtr job_handle_;
  uint64_t     job_id_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// ReadyFlagCondition
//   - Blackboard 注入 sync_ctx
//   - Check() → 读取 EntityContext::GetFlag("ready")
// ─────────────────────────────────────────────────────────────────────────────
class ReadyFlagCondition : public ConditionBase {
 public:
  ReadyFlagCondition(const std::string& name, const BT::NodeConfig& config)
      : ConditionBase(name, config) /* ctx = nullptr → 从 Blackboard 读取 */ {}

  static BT::PortsList providedPorts() { return {}; }

 protected:
  bool Check() override {
    return Ctx().Entity().GetFlag("ready", false);
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// BT XML：Sequence(ReadyFlagCondition → RecordingAsyncAction)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr const char* kTestTreeXml = R"(
<root BTCPP_format="4">
  <BehaviorTree ID="MultiEntityTree">
    <Sequence>
      <ReadyFlagCondition/>
      <RecordingAsyncAction/>
    </Sequence>
  </BehaviorTree>
</root>
)";

// ─────────────────────────────────────────────────────────────────────────────
// 工厂函数声明（defined in bt_runtime_impl.cpp）
// ─────────────────────────────────────────────────────────────────────────────
std::shared_ptr<IBtRuntime> CreateBtRuntime();

// ─────────────────────────────────────────────────────────────────────────────
// Fixture
// ─────────────────────────────────────────────────────────────────────────────
class MultiEntityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // 重置全局状态
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_completions.clear();
    }
    g_success_count.store(0);

    event_loop_ = std::make_shared<UvwEventLoopRuntime>();
    ASSERT_TRUE(static_cast<bool>(event_loop_->Start()));

    executor_    = std::make_shared<TbbJobExecutor>();
    command_bus_ = std::make_shared<NullCommandBus>();

    bt_runtime_ = CreateBtRuntime();
    ASSERT_TRUE(static_cast<bool>(bt_runtime_->Initialize()));

    // Wakeup 链路：TBB → mailbox → PostToLoop → DrainAll → RequestWakeup
    auto mailbox  = executor_->Mailbox_shared();
    auto bt_wk    = std::weak_ptr<IBtRuntime>(bt_runtime_);
    auto loop_ptr = event_loop_.get();
    executor_->SetWakeupCallback(
        [loop_ptr, mailbox, bt_wk]() {
          loop_ptr->PostToLoop([mailbox, bt_wk]() {
            mailbox->DrainAll([bt_wk](JobResult r) {
              if (auto bt = bt_wk.lock())
                if (r.owner_entity != kInvalidEntityId)
                  bt->RequestWakeup(r.owner_entity);
            });
          });
        });

    // 通过 IBtRuntime 接口注册节点（无需访问内部 BT::BehaviorTreeFactory）
    bt_runtime_->RegisterNodeType<RecordingAsyncAction>("RecordingAsyncAction");
    bt_runtime_->RegisterNodeType<ReadyFlagCondition>("ReadyFlagCondition");

    auto xml_status = bt_runtime_->LoadTreeFromXml(kTestTreeXml);
    ASSERT_TRUE(static_cast<bool>(xml_status))
        << "LoadTreeFromXml failed: " << xml_status.message;
  }

  void TearDown() override {
    executor_->Shutdown();
    event_loop_->Stop();
  }

  // 为实体创建完整上下文套件并注册到 BtRuntime
  void SpawnEntity(EntityId id, bool ready_flag = true) {
    auto entity_ctx = std::make_shared<EntityContextImpl>(id);
    entity_ctx->SetFlag("ready", ready_flag);

    auto async_ctx = std::make_shared<AsyncActionContextImpl>(
        id, executor_, event_loop_, bt_runtime_, &sim_time_);

    auto sync_ctx = std::make_shared<SyncNodeContextImpl>(
        id, entity_ctx, command_bus_, nullptr, &sim_time_);

    auto res = bt_runtime_->CreateTree(id, "MultiEntityTree", async_ctx, sync_ctx);
    ASSERT_TRUE(res.ok()) << "CreateTree failed for entity " << id
                          << ": " << res.status.message;

    entity_ctxs_[id] = entity_ctx;
    async_ctxs_[id]  = async_ctx;
  }

  // 驱动 TickAll 直到 pred() 为 true 或超时
  bool TickUntil(std::function<bool()> pred,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      sim_time_ += 10;
      bt_runtime_->TickAll(sim_time_);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
  }

  std::shared_ptr<UvwEventLoopRuntime>  event_loop_;
  std::shared_ptr<TbbJobExecutor>       executor_;
  std::shared_ptr<ICommandBus>          command_bus_;
  std::shared_ptr<IBtRuntime>           bt_runtime_;

  std::unordered_map<EntityId, std::shared_ptr<EntityContextImpl>>       entity_ctxs_;
  std::unordered_map<EntityId, std::shared_ptr<AsyncActionContextImpl>>  async_ctxs_;

  SimTimeMs sim_time_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// 测试 1：单实体 Blackboard 注入正确性
//   RecordingAsyncAction 的 Ctx().OwnerEntity() == entity_id
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(MultiEntityTest, BlackboardCtxOwnerMatchesEntityId) {
  constexpr EntityId kEntity = 42;
  SpawnEntity(kEntity);

  ASSERT_TRUE(TickUntil([&] {
    return g_success_count.load() >= 1;
  })) << "单实体任务未在 3s 内完成";

  std::lock_guard<std::mutex> lk(g_mu);
  ASSERT_EQ(g_completions.size(), 1u);
  EXPECT_EQ(g_completions[0].owner_from_ctx, kEntity)
      << "Blackboard 注入的 ctx 与实体 ID 不匹配";
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 2：三实体并发 — 所有结果到达，owner_entity 各自正确
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(MultiEntityTest, ThreeEntitiesAllCompleteWithCorrectOwner) {
  constexpr int kCount = 3;
  const EntityId kEntities[kCount] = {101, 202, 303};

  for (auto id : kEntities) SpawnEntity(id);

  ASSERT_TRUE(TickUntil([&] {
    return g_success_count.load() >= kCount;
  })) << "三实体任务未在 3s 内全部完成";

  std::lock_guard<std::mutex> lk(g_mu);
  ASSERT_EQ(static_cast<int>(g_completions.size()), kCount);

  // 用 std::set 避免需要 unordered_set 哈希特化
  std::set<EntityId> expected_ids(std::begin(kEntities), std::end(kEntities));
  std::set<EntityId> seen_ids;
  for (auto& rec : g_completions) {
    EXPECT_TRUE(expected_ids.count(rec.owner_from_ctx))
        << "收到未知实体的结果: " << rec.owner_from_ctx;
    seen_ids.insert(rec.owner_from_ctx);
  }
  EXPECT_EQ(seen_ids.size(), static_cast<size_t>(kCount))
      << "存在实体未完成或重复完成";
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 3：ConditionBase Blackboard 注入
//   ready=false → Condition FAILURE → Action 不执行
//   设置 ready=true → Action 执行成功
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(MultiEntityTest, ConditionBlocksActionWhenFlagFalse) {
  constexpr EntityId kEntity = 77;
  SpawnEntity(kEntity, /*ready_flag=*/false);

  for (int i = 0; i < 10; ++i) {
    sim_time_ += 10;
    bt_runtime_->TickAll(sim_time_);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(g_success_count.load(), 0)
      << "ready=false 时 RecordingAsyncAction 不应执行";

  entity_ctxs_[kEntity]->SetFlag("ready", true);

  ASSERT_TRUE(TickUntil([&] {
    return g_success_count.load() >= 1;
  })) << "ready=true 后任务未在 3s 内完成";

  std::lock_guard<std::mutex> lk(g_mu);
  ASSERT_EQ(g_completions.size(), 1u);
  EXPECT_EQ(g_completions[0].owner_from_ctx, kEntity);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 4：10 个实体并发 — 验证无死锁、无结果丢失
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(MultiEntityTest, TenEntitiesNoneDropped) {
  constexpr int kCount = 10;
  for (int i = 0; i < kCount; ++i) SpawnEntity(EntityId(1000 + i));

  ASSERT_TRUE(TickUntil(
      [&] { return g_success_count.load() >= kCount; },
      std::chrono::milliseconds(5000)))
      << kCount << " 个实体未在 5s 内全部完成";

  EXPECT_EQ(g_success_count.load(), kCount) << "存在结果丢失";

  std::lock_guard<std::mutex> lk(g_mu);
  for (auto& rec : g_completions) {
    EXPECT_GE(rec.owner_from_ctx, EntityId(1000));
    EXPECT_LT(rec.owner_from_ctx, EntityId(1000 + kCount));
  }
}

}  // namespace sim_bt
