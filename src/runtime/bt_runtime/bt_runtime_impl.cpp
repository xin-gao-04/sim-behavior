// ─────────────────────────────────────────────────────────────────────────────
// BtRuntimeImpl — IBtRuntime 的 BehaviorTree.CPP 实现
// ─────────────────────────────────────────────────────────────────────────────
#include "sim_bt/runtime/bt_runtime/i_bt_runtime.hpp"

#include <behaviortree_cpp/bt_factory.h>
#ifdef BTCPP_SQLITE_LOGGING
#include <behaviortree_cpp/loggers/bt_sqlite_logger.h>
#endif

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

#include "sim_bt/common/sim_bt_log.hpp"

// Blackboard key 常量：节点通过这两个 key 从 Blackboard 取运行时上下文
static constexpr const char* kBlackboardKeyAsyncCtx = "__async_ctx__";
static constexpr const char* kBlackboardKeySyncCtx  = "__sync_ctx__";

namespace sim_bt {

// ── 内部 TreeInstance ─────────────────────────────────────────────────────────

class BtTreeInstance : public ITreeInstance {
 public:
  BtTreeInstance(EntityId owner, BT::Tree tree)
      : owner_(owner), tree_(std::move(tree)) {}

  EntityId    OwnerEntity() const override { return owner_; }
  const char* TreeName() const override { return tree_.rootNode()->name().c_str(); }

  NodeStatus Tick() override {
    auto status = tree_.tickOnce();
    return static_cast<NodeStatus>(status);
  }

  void Halt() override { tree_.haltTree(); }

  void Reset() override {
    tree_.haltTree();
    // clear() is deprecated in BTCPP 4.7+; use Backup/Restore pattern:
    // backup the original empty state before first tick, restore it here.
    // For a simple reset, just halt — blackboard state is owned by the caller.
  }

  bool HasRunningNodes() const override {
    return tree_.rootNode()->status() == BT::NodeStatus::RUNNING;
  }

  // 暴露内部 BT::Tree 引用（仅供 BtRuntimeImpl 使用，如 SqliteLogger 附加）。
  BT::Tree& GetBtTree() { return tree_; }

  // ┌── Phase 5 TODO [P0] 方案 A — destroyed 标记 ────────────────────────────┐
  // │ 新增 std::atomic<bool> destroyed_{false};                                │
  // │ 新增 bool IsDestroyed() const { return destroyed_.load(acquire); }       │
  // │ 新增 void MarkDestroyed() { destroyed_.store(true, release); }           │
  // │ DestroyTree() 中先 MarkDestroyed() 再 Halt()，                          │
  // │ TickAll 锁外 Tick 前检查 IsDestroyed()，跳过已销毁的树。               │
  // │ 这是方案 A（锁外 Tick）的安全保障。                                     │
  // └─────────────────────────────────────────────────────────────────────────────┘

 private:
  EntityId owner_;
  BT::Tree tree_;
};

// ── BtRuntimeImpl ─────────────────────────────────────────────────────────────

class BtRuntimeImpl : public IBtRuntime {
 public:
  BtRuntimeImpl() = default;
  ~BtRuntimeImpl() override { Shutdown(); }

  void RegisterNodeBuilder(const BT::TreeNodeManifest& manifest,
                           BT::NodeBuilder             builder) override {
    factory_.registerBuilder(manifest, std::move(builder));
    SIMBT_LOG_INFO_S("BtRuntime: registered node type \""
        << manifest.registration_ID << "\"");
  }

  SimStatus Initialize() override {
    initialized_ = true;
    SIMBT_LOG_INFO("BtRuntime: initialized");
    return SimStatus::Ok();
  }

  void Shutdown() override {
    SIMBT_LOG_INFO("BtRuntime: shutdown");
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& kv : trees_) {
      if (kv.second) kv.second->Halt();
    }
    trees_.clear();
  }

  SimStatus LoadTreeFromXml(const std::string& xml) override {
    try {
      // BTCPP 4.7+: loadFromText renamed to registerBehaviorTreeFromText
      factory_.registerBehaviorTreeFromText(xml);
      return SimStatus::Ok();
    } catch (const std::exception& ex) {
      return SimStatus::Err(1, std::string("LoadTreeFromXml failed: ") + ex.what());
    }
  }

  SimStatus LoadTreeFromFile(const std::string& path) override {
    try {
      // BTCPP 4.7+: loadFromFile renamed to registerBehaviorTreeFromFile
      factory_.registerBehaviorTreeFromFile(path);
      return SimStatus::Ok();
    } catch (const std::exception& ex) {
      return SimStatus::Err(1, std::string("LoadTreeFromFile failed: ") + ex.what());
    }
  }

  SimResult<TreeInstancePtr> CreateTree(
      EntityId entity_id,
      const std::string& tree_name,
      AsyncActionContextPtr async_ctx = nullptr,
      SyncNodeContextPtr    sync_ctx  = nullptr) override {
    try {
      // 为每棵树创建私有 Blackboard，并将 per-entity 上下文注入其中。
      // 节点在构造时（或首次 tick 前）从 Blackboard 读取 ctx，
      // 无需在 registerBuilder 里逐节点捕获实体特定数据。
      auto blackboard = BT::Blackboard::create();
      if (async_ctx) {
        blackboard->set<AsyncActionContextPtr>(kBlackboardKeyAsyncCtx, async_ctx);
      }
      if (sync_ctx) {
        blackboard->set<SyncNodeContextPtr>(kBlackboardKeySyncCtx, sync_ctx);
      }

      BT::Tree bt_tree = factory_.createTree(tree_name, blackboard);
      auto inst = std::make_shared<BtTreeInstance>(entity_id, std::move(bt_tree));
      {
        std::lock_guard<std::mutex> lock(mu_);
        trees_[entity_id] = inst;
      }
      // ┌── Phase 5 TODO [P1] 方案 B ────────────────────────────────────────┐
      // │ 新树创建后加入 active_set_：active_set_.insert(entity_id);        │
      // │ 新树首帧必须被 Tick，不能被 skip。                                │
      // └───────────────────────────────────────────────────────────────────┘
      SIMBT_LOG_INFO_S("BtRuntime: created tree \"" << tree_name
          << "\" for entity=" << entity_id
          << " async_ctx=" << (async_ctx ? "yes" : "no")
          << " sync_ctx="  << (sync_ctx  ? "yes" : "no"));
      return SimResult<TreeInstancePtr>(std::static_pointer_cast<ITreeInstance>(inst));
    } catch (const std::exception& ex) {
      return SimResult<TreeInstancePtr>(
          SimStatus::Err(1, std::string("CreateTree failed: ") + ex.what()));
    }
  }

  // ┌── Phase 5 TODO [P0/P1] 方案 A+B ────────────────────────────────────────┐
  // │ 方案 A：在 erase 前调用 it->second->MarkDestroyed() 防止锁外 Tick。     │
  // │ 方案 B：同时从 active_set_ 中移除 entity_id。                           │
  // └─────────────────────────────────────────────────────────────────────────────┘
  void DestroyTree(EntityId entity_id) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = trees_.find(entity_id);
    if (it != trees_.end()) {
      if (it->second) it->second->Halt();
      trees_.erase(it);
    }
  }

  TreeInstancePtr GetTree(EntityId entity_id) const override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = trees_.find(entity_id);
    return (it != trees_.end()) ? it->second : nullptr;
  }

  void TickAll(SimTimeMs sim_time_ms) override {
    auto t0 = std::chrono::steady_clock::now();

    // Phase 1：处理 wakeup 队列，确保外部请求在本帧生效
    // ┌── Phase 5 TODO [P0] 方案 C ──────────────────────────────────────────┐
    // │ 将 std::queue → std::unordered_set 后，此处改为：                     │
    // │   std::unordered_set<EntityId> wakeups;                               │
    // │   { lock(mu_); std::swap(wakeups, wakeup_set_); }                    │
    // │   for (EntityId id : wakeups) TickEntityLocked(id);                   │
    // │ 自动去重，避免同一实体一帧内被 Tick 多次。                            │
    // └───────────────────────────────────────────────────────────────────────┘
    // ┌── Phase 5 TODO [P0] 方案 A ──────────────────────────────────────────┐
    // │ Phase 1 的 TickEntityLocked 当前在 mu_ 内执行 Tick()。               │
    // │ 优化后改为：                                                          │
    // │   1. 锁内只做查找：根据 wakeup id 从 trees_ 取 shared_ptr 到 local   │
    // │   2. 释放 mu_                                                         │
    // │   3. 遍历 local，逐棵 Tick()（需检查 destroyed_ 标记）               │
    // └───────────────────────────────────────────────────────────────────────┘
    std::queue<EntityId> wakeups;
    {
      std::lock_guard<std::mutex> lock(mu_);
      std::swap(wakeups, wakeup_queue_);
    }
    const size_t wakeup_count = wakeups.size();
    if (wakeup_count > 0) {
      SIMBT_LOG_INFO_S("BtRuntime: tick_all sim_time=" << sim_time_ms
          << "ms trees=" << trees_.size()
          << " wakeups=" << wakeup_count);
    }
    while (!wakeups.empty()) {
      EntityId id = wakeups.front();
      wakeups.pop();
      TickEntityLocked(id);
    }

    // Phase 2：逐实体 tick（kSkipIdle 策略下跳过根节点非 RUNNING 的树）
    // ┌── Phase 5 TODO [P1] 方案 B ──────────────────────────────────────────┐
    // │ 引入 active_set_ 后，Phase 2 改为：                                   │
    // │   std::vector<std::shared_ptr<BtTreeInstance>> snapshot;               │
    // │   { lock(mu_);                                                        │
    // │     for (EntityId eid : active_set_) {                                │
    // │       auto it = trees_.find(eid);                                     │
    // │       if (it != trees_.end()) snapshot.push_back(it->second);         │
    // │     }                                                                 │
    // │   } // unlock                                                         │
    // │   for (auto& tree : snapshot) {                                       │
    // │     if (tree->IsDestroyed()) { ++skipped; continue; }                 │
    // │     auto st = tree->Tick();                                           │
    // │     if (st != NodeStatus::kRunning) {                                 │
    // │       lock(mu_); active_set_.erase(tree->OwnerEntity());              │
    // │     }                                                                 │
    // │     ++ticked;                                                         │
    // │   }                                                                   │
    // │ 这同时实现了方案 A（锁外 Tick）和方案 B（仅遍历活跃集合）。         │
    // └───────────────────────────────────────────────────────────────────────┘
    size_t ticked = 0, skipped = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      for (auto& kv : trees_) {
        if (tick_policy_ == TickPolicy::kSkipIdle &&
            !kv.second->HasRunningNodes()) {
          ++skipped;
          continue;
        }
        kv.second->Tick();
        ++ticked;
      }
    }

    // 记录本帧统计
    auto dur_us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0).count();
    last_tick_stats_.sim_time_ms   = sim_time_ms;
    last_tick_stats_.duration_us   = dur_us;
    last_tick_stats_.tree_count    = ticked;
    last_tick_stats_.skipped_count = skipped;
    last_tick_stats_.wakeup_count  = wakeup_count;
  }

  void TickEntity(EntityId entity_id) override {
    TickEntityLocked(entity_id);
  }

  // ┌── Phase 5 TODO [P0] 方案 C — wakeup 去重 ──────────────────────────────┐
  // │ 改为 wakeup_set_.insert(entity_id)，自动去重。                         │
  // │ 同时在 [P1] 方案 B 下需 active_set_.insert(entity_id) 标记活跃。      │
  // └───────────────────────────────────────────────────────────────────────────┘
  void RequestWakeup(EntityId entity_id) override {
    SIMBT_LOG_INFO_S("BtRuntime: wakeup requested entity=" << entity_id);
    std::lock_guard<std::mutex> lock(mu_);
    wakeup_queue_.push(entity_id);
  }

  size_t ActiveTreeCount() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return trees_.size();
  }

  // ── Phase 4 ─────────────────────────────────────────────────────────────────

  void SetTickPolicy(TickPolicy policy) override {
    tick_policy_ = policy;
  }

  TickStats LastTickStats() const override {
    return last_tick_stats_;
  }

  SimStatus EnableSqliteLogger(const std::string& db_path) override {
#ifdef BTCPP_SQLITE_LOGGING
    std::lock_guard<std::mutex> lock(mu_);
    bool first = true;
    for (auto& kv : trees_) {
      try {
        auto logger = std::make_unique<BT::SqliteLogger>(
            kv.second->GetBtTree(), db_path, /*append=*/!first);
        first = false;
        sqlite_loggers_.push_back(std::move(logger));
      } catch (const std::exception& ex) {
        return SimStatus::Err(1,
            std::string("EnableSqliteLogger failed for entity ")
            + std::to_string(kv.first) + ": " + ex.what());
      }
    }
    SIMBT_LOG_INFO_S("BtRuntime: attached SqliteLogger to "
        << sqlite_loggers_.size() << " trees → " << db_path);
    return SimStatus::Ok();
#else
    (void)db_path;
    return SimStatus::Err(1,
        "EnableSqliteLogger: not available (BTCPP_SQLITE_LOGGING=OFF)");
#endif
  }

  // 向工厂注册节点（供外部调用）
  BT::BehaviorTreeFactory& Factory() { return factory_; }

 private:
  void TickEntityLocked(EntityId entity_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = trees_.find(entity_id);
    if (it != trees_.end() && it->second) {
      it->second->Tick();
    }
  }

  BT::BehaviorTreeFactory factory_;

  mutable std::mutex mu_;
  std::unordered_map<EntityId, std::shared_ptr<BtTreeInstance>> trees_;
  std::queue<EntityId> wakeup_queue_;

  bool initialized_ = false;

  // Phase 4
  TickPolicy tick_policy_   = TickPolicy::kTickAll;
  TickStats  last_tick_stats_;

  // ┌─────────────────────────────────────────────────────────────────────────┐
  // │  Phase 5 TODO — 性能深度优化（数据结构重构）                            │
  // │                                                                         │
  // │  [P0] 方案 C — wakeup 去重                                              │
  // │  将 wakeup_queue_ 从 std::queue<EntityId> 改为                          │
  // │  std::unordered_set<EntityId> wakeup_set_。                             │
  // │  RequestWakeup() 使用 insert（自动去重），                               │
  // │  TickAll Phase 1 消费时 swap 到 local set 后遍历。                      │
  // │  改动极小，避免同一实体一帧内被多次 Tick。                               │
  // │                                                                         │
  // │  [P1] 方案 B — Active Set                                               │
  // │  新增 active_set_: std::unordered_set<EntityId>                         │
  // │  Phase 2 只遍历 active_set_ 而非全部 trees_。                           │
  // │  维护规则：                                                              │
  // │    - Tick() 返回 RUNNING → 加入 active_set_                             │
  // │    - Tick() 返回 SUCCESS/FAILURE → 从 active_set_ 移除                  │
  // │    - RequestWakeup(eid) → 无条件加入 active_set_                        │
  // │    - CreateTree(eid) → 加入 active_set_（新树首帧必须 tick）             │
  // │    - DestroyTree(eid) → 从 active_set_ 移除                             │
  // │  收益：200 棵树 10 棵 active → Phase 2 遍历从 200 降至 10。             │
  // │                                                                         │
  // │  [P0] 方案 A — TickAll 锁粒度优化                                       │
  // │  Phase 2 当前在 mu_ 锁内执行所有 Tick()，阻塞 RequestWakeup。           │
  // │  优化方案：                                                              │
  // │    1. 锁内只做 snapshot：拷贝 active_set_ 对应的                        │
  // │       vector<shared_ptr<BtTreeInstance>> 到 local_snapshot              │
  // │    2. 释放 mu_                                                           │
  // │    3. 遍历 local_snapshot，逐棵 Tick()（锁外执行）                      │
  // │  注意事项：                                                              │
  // │    - Tick 期间 DestroyTree 可能被调用 → shared_ptr 保证不释放            │
  // │    - 但 BT::Tree 在 Halt 后再 Tick 不安全 → BtTreeInstance 新增         │
  // │      std::atomic<bool> destroyed_ 标记，Tick() 前检查                    │
  // │    - TickEntityLocked (Phase 1) 同理需要改为锁外执行                    │
  // │  收益：mu_ 持锁从 O(N×Tick_cost) 降至 O(N×sizeof(shared_ptr))。        │
  // └─────────────────────────────────────────────────────────────────────────┘
#ifdef BTCPP_SQLITE_LOGGING
  std::vector<std::unique_ptr<BT::SqliteLogger>> sqlite_loggers_;
#endif
};

// ── 工厂函数 ─────────────────────────────────────────────────────────────────

std::shared_ptr<IBtRuntime> CreateBtRuntime() {
  return std::make_shared<BtRuntimeImpl>();
}

}  // namespace sim_bt
