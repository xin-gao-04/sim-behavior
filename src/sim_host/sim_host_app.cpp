#include "sim_host_app.hpp"

#include <chrono>
#include <thread>
#include <vector>

// 具体实现引用（内部 include，不暴露到 public interface）
#include "../runtime/compute_runtime/tbb_job_executor.hpp"
#include "../runtime/async_runtime/uvw_event_loop_runtime.hpp"
#include "../runtime/bt_runtime/async_action_context_impl.hpp"
#include "../runtime/bt_runtime/sync_node_context_impl.hpp"
#include "../domain/entity/entity_context_impl.hpp"
#include "sim_bt/common/sim_bt_log.hpp"

// WorldSnapshot 具体实现（从 world_snapshot_impl.cpp 中定义）
// 通过前向声明使用 SimpleWorldSnapshotProvider 工厂函数
std::shared_ptr<sim_bt::IWorldSnapshotProvider> CreateSimpleWorldSnapshotProvider();

namespace sim_bt {

// 工厂函数声明（defined in their respective .cpp files）
std::shared_ptr<IBtRuntime>     CreateBtRuntime();
std::shared_ptr<ICommandBus>    CreateInProcessCommandBus();
std::shared_ptr<IGroupContext>  CreateGroupContext(GroupId id);

SimHostApp::SimHostApp() {
  // 尽早初始化进程级 logger，使后续所有 SIMBT_LOG_* 调用立即生效
  InitSimBtLog("sim_host");
}

SimHostApp::~SimHostApp() {
  RequestStop();
  ShutdownSimBtLog();
}

SimStatus SimHostApp::Initialize() {
  SIMBT_LOG_INFO("SimHostApp: initializing");
  // ── 创建各运行时 ──────────────────────────────────────────────────────────

  // Compute Runtime
  ArenaConfig arena_cfg;
  arena_cfg.high_concurrency   = 2;
  arena_cfg.normal_concurrency = 4;
  arena_cfg.low_concurrency    = 2;
  auto executor = std::make_shared<TbbJobExecutor>(arena_cfg);
  job_executor_ = executor;

  // Async Runtime (uvw)
  auto event_loop = std::make_shared<UvwEventLoopRuntime>();
  event_loop_ = event_loop;

  // BT Runtime（需在 wakeup callback 设置前创建，否则 weak_ptr 为空）
  bt_runtime_ = CreateBtRuntime();
  auto status = bt_runtime_->Initialize();
  if (!status) return status;

  // 将 TBB 结果写入 mailbox 后触发完整 wakeup 链路：
  //   TBB worker → mailbox.Post() → notify_cb
  //   → PostToLoop（投入 uvw loop 线程）
  //   → DrainAll（incoming_ → ready_ 并逐条回调）
  //   → RequestWakeup(owner_entity)
  //   → 下一帧 TickAll Phase 1 优先 tick 该实体
  auto mailbox_ptr   = executor->Mailbox_shared();
  auto bt_runtime_wk = std::weak_ptr<IBtRuntime>(bt_runtime_);
  executor->SetWakeupCallback(
      [event_loop_ptr = event_loop.get(),
       mailbox_ptr,
       bt_runtime_wk]() {
        event_loop_ptr->PostToLoop(
            [mailbox_ptr, bt_runtime_wk]() {
              // 在 uvw loop 线程：批量 drain，逐条 RequestWakeup
              mailbox_ptr->DrainAll([bt_runtime_wk](JobResult r) {
                if (auto bt = bt_runtime_wk.lock()) {
                  if (r.owner_entity != kInvalidEntityId) {
                    bt->RequestWakeup(r.owner_entity);
                  }
                }
              });
            });
      });

  // Command Bus（进程内同步实现）
  command_bus_ = CreateInProcessCommandBus();

  // WorldSnapshot Provider（空快照，每帧 Refresh 生成新帧快照）
  snapshot_provider_ = CreateSimpleWorldSnapshotProvider();

  // 启动 EventLoop
  status = event_loop_->Start();
  if (!status) return status;

  SIMBT_LOG_INFO("SimHostApp: initialization complete");
  return SimStatus::Ok();
}

SimStatus SimHostApp::SpawnEntity(EntityId entity_id,
                                  const std::string& tree_name) {
  SIMBT_LOG_INFO_S("SimHostApp: spawning entity=" << entity_id
      << " tree=" << tree_name);

  // 1. EntityContext — 实体私有状态（跨帧持久）
  auto entity_ctx = std::make_shared<EntityContextImpl>(entity_id);

  // 2. AsyncActionContext — 连接 TBB executor + uvw event loop + BT runtime
  auto async_ctx = std::make_shared<AsyncActionContextImpl>(
      entity_id,
      job_executor_,
      event_loop_,
      bt_runtime_,
      &current_sim_time_);

  // 3. SyncNodeContext — 连接 EntityContext + WorldSnapshot + CommandBus
  //    WorldSnapshot 取当前帧快照（裸指针，非拥有；每帧由 TickLoop 刷新）
  const IWorldSnapshot* world_ptr =
      snapshot_provider_ ? snapshot_provider_->Current().get() : nullptr;
  auto sync_ctx = std::make_shared<SyncNodeContextImpl>(
      entity_id,
      entity_ctx,
      command_bus_,
      world_ptr,
      &current_sim_time_);

  // 4. 创建行为树，通过 Blackboard 注入两个上下文
  auto result = bt_runtime_->CreateTree(entity_id, tree_name, async_ctx, sync_ctx);
  if (!result.ok()) {
    return result.status;
  }

  // 5. 保存上下文套件（DespawnEntity 时清理）
  entity_bundles_[entity_id] = EntityBundle{
      std::move(entity_ctx),
      std::move(async_ctx),
      std::move(sync_ctx)
  };

  SIMBT_LOG_INFO_S("SimHostApp: entity=" << entity_id << " spawned successfully");
  return SimStatus::Ok();
}

void SimHostApp::DespawnEntity(EntityId entity_id) {
  SIMBT_LOG_INFO_S("SimHostApp: despawning entity=" << entity_id);
  bt_runtime_->DestroyTree(entity_id);
  entity_bundles_.erase(entity_id);
}

SimStatus SimHostApp::AssignGroup(GroupId group_id,
                                   const std::vector<EntityId>& members) {
  SIMBT_LOG_INFO_S("SimHostApp: assigning group=" << group_id
      << " members=" << members.size());

  // 取已有编队或新建
  auto it = group_bundles_.find(group_id);
  if (it == group_bundles_.end()) {
    GroupBundle bundle;
    bundle.group_ctx = CreateGroupContext(group_id);
    group_bundles_[group_id] = std::move(bundle);
    it = group_bundles_.find(group_id);
  }
  auto& bundle = it->second;

  for (EntityId eid : members) {
    auto eb_it = entity_bundles_.find(eid);
    if (eb_it == entity_bundles_.end()) {
      SIMBT_LOG_WARN_S("SimHostApp: AssignGroup entity=" << eid << " not found, skipped");
      continue;
    }
    bundle.group_ctx->AddMember(eid);
    bundle.members.push_back(eid);
    // 注入到 SyncNodeContextImpl（向下转型是安全的，SimHostApp 创建的是 SyncNodeContextImpl）
    auto* sync_impl =
        static_cast<SyncNodeContextImpl*>(eb_it->second.sync_ctx.get());
    sync_impl->SetGroupContext(bundle.group_ctx);
  }

  return SimStatus::Ok();
}

void SimHostApp::DisbandGroup(GroupId group_id) {
  SIMBT_LOG_INFO_S("SimHostApp: disbanding group=" << group_id);
  auto it = group_bundles_.find(group_id);
  if (it == group_bundles_.end()) return;

  // 清除所有成员的 group_ctx
  for (EntityId eid : it->second.members) {
    auto eb_it = entity_bundles_.find(eid);
    if (eb_it == entity_bundles_.end()) continue;
    auto* sync_impl =
        static_cast<SyncNodeContextImpl*>(eb_it->second.sync_ctx.get());
    sync_impl->SetGroupContext(nullptr);
  }
  group_bundles_.erase(it);
}

// ── Phase 4：性能治理 ──────────────────────────────────────────────────────────

SimStatus SimHostApp::EnableTreeDebugLogger(const std::string& db_path) {
  return bt_runtime_->EnableSqliteLogger(db_path);
}

void SimHostApp::SetTickPolicy(IBtRuntime::TickPolicy policy) {
  bt_runtime_->SetTickPolicy(policy);
}

IBtRuntime::TickStats SimHostApp::LastTickStats() const {
  return bt_runtime_->LastTickStats();
}

void SimHostApp::Run() {
  TickLoop();
}

void SimHostApp::RequestStop() {
  SIMBT_LOG_INFO_S("SimHostApp: stop requested at sim_time=" << current_sim_time_ << "ms");
  stop_requested_ = true;
  if (event_loop_) event_loop_->Stop();
  if (job_executor_) job_executor_->Shutdown();
  if (bt_runtime_) bt_runtime_->Shutdown();
}

void SimHostApp::TickLoop() {
  // 固定帧率：20 Hz（50ms/frame），可配置
  constexpr auto kFrameInterval = std::chrono::milliseconds(50);

  while (!stop_requested_) {
    auto frame_start = std::chrono::steady_clock::now();

    current_sim_time_ += 50;  // 每帧推进 50ms

    // 1. 刷新世界快照（新帧开始时生成一致性快照）
    if (snapshot_provider_) {
      snapshot_provider_->Refresh(current_sim_time_);
    }

    // 2. Tick 所有活跃实体的行为树
    bt_runtime_->TickAll(current_sim_time_);

    // 3. 等到下一帧
    auto elapsed = std::chrono::steady_clock::now() - frame_start;
    auto sleep_time = kFrameInterval - elapsed;
    if (sleep_time > std::chrono::milliseconds(0)) {
      std::this_thread::sleep_for(sleep_time);
    }
  }
}

}  // namespace sim_bt
