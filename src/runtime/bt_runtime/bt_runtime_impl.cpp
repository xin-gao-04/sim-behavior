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
#include <unordered_map>
#include <unordered_set>
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
  }

  bool HasRunningNodes() const override {
    return tree_.rootNode()->status() == BT::NodeStatus::RUNNING;
  }

  // 暴露内部 BT::Tree 引用（仅供 BtRuntimeImpl 使用，如 SqliteLogger 附加）。
  BT::Tree& GetBtTree() { return tree_; }

  // 方案 A — destroyed 标记：DestroyTree 先 MarkDestroyed，再从 map 移除。
  // Phase 2 在锁外 Tick 前检查此标记，防止对已 Halt 的树重复 Tick。
  void MarkDestroyed() { destroyed_.store(true, std::memory_order_release); }
  bool IsDestroyed()  const { return destroyed_.load(std::memory_order_acquire); }

 private:
  EntityId owner_;
  BT::Tree tree_;
  std::atomic<bool> destroyed_{false};
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
    active_set_.clear();
    wakeup_set_.clear();
  }

  SimStatus LoadTreeFromXml(const std::string& xml) override {
    try {
      factory_.registerBehaviorTreeFromText(xml);
      return SimStatus::Ok();
    } catch (const std::exception& ex) {
      return SimStatus::Err(1, std::string("LoadTreeFromXml failed: ") + ex.what());
    }
  }

  SimStatus LoadTreeFromFile(const std::string& path) override {
    try {
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
        // 方案 B：新建树加入 active_set_，确保首帧被 Tick
        active_set_.insert(entity_id);
      }
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

  void DestroyTree(EntityId entity_id) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = trees_.find(entity_id);
    if (it != trees_.end()) {
      // 方案 A：先标记 destroyed，再 Halt，防止锁外 Tick 读到已释放的树
      if (it->second) {
        it->second->MarkDestroyed();
        it->second->Halt();
      }
      trees_.erase(it);
      // 方案 B：从 active_set_ 移除
      active_set_.erase(entity_id);
      wakeup_set_.erase(entity_id);
    }
  }

  TreeInstancePtr GetTree(EntityId entity_id) const override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = trees_.find(entity_id);
    return (it != trees_.end()) ? it->second : nullptr;
  }

  void TickAll(SimTimeMs sim_time_ms) override {
    auto t0 = std::chrono::steady_clock::now();

    // ── Phase 1：处理 wakeup 队列 ────────────────────────────────────────────
    // 方案 C：wakeup_set_ 天然去重，同一实体多次 RequestWakeup 只 Tick 一次。
    // 方案 A：锁内只做 swap + 指针收集，锁外执行 Tick()。
    std::unordered_set<EntityId> wakeups;
    std::vector<std::shared_ptr<BtTreeInstance>> phase1_trees;
    {
      std::lock_guard<std::mutex> lock(mu_);
      std::swap(wakeups, wakeup_set_);
      phase1_trees.reserve(wakeups.size());
      for (EntityId id : wakeups) {
        auto it = trees_.find(id);
        if (it != trees_.end() && it->second) {
          phase1_trees.push_back(it->second);
        }
      }
    }

    const size_t wakeup_count = wakeups.size();
    if (wakeup_count > 0) {
      SIMBT_LOG_INFO_S("BtRuntime: tick_all sim_time=" << sim_time_ms
          << "ms trees=" << trees_.size()
          << " wakeups=" << wakeup_count);
    }

    // 锁外执行 Phase 1 Tick（方案 A）
    for (auto& tree : phase1_trees) {
      if (tree->IsDestroyed()) continue;
      auto status = tree->Tick();
      // Phase 1 wakeup Tick 后，根据结果更新 active_set_（方案 B）
      EntityId eid = tree->OwnerEntity();
      if (status != NodeStatus::kRunning) {
        std::lock_guard<std::mutex> lock(mu_);
        // 仅当实体仍在 trees_ 中且未被新 wakeup 时移出 active_set_
        // 注意：RequestWakeup 可能在此期间再次将其加入 active_set_
        // unordered_set::erase 是幂等的，所以直接 erase 安全
        if (trees_.count(eid) && !wakeup_set_.count(eid)) {
          active_set_.erase(eid);
        }
      }
    }

    // ── Phase 2：遍历活跃集合 Tick ───────────────────────────────────────────
    // 方案 B + A：锁内 snapshot active_set_ 对应的 tree 指针，锁外 Tick。
    // kTickAll：遍历所有 trees_（保持原语义）。
    // kSkipIdle：只遍历 active_set_（仅 RUNNING 树）。
    std::vector<std::shared_ptr<BtTreeInstance>> phase2_trees;
    size_t total_trees = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      total_trees = trees_.size();
      if (tick_policy_ == TickPolicy::kSkipIdle) {
        // 只遍历 active_set_
        phase2_trees.reserve(active_set_.size());
        for (EntityId eid : active_set_) {
          auto it = trees_.find(eid);
          if (it != trees_.end() && it->second) {
            phase2_trees.push_back(it->second);
          }
        }
      } else {
        // kTickAll：遍历所有树
        phase2_trees.reserve(trees_.size());
        for (auto& kv : trees_) {
          if (kv.second) phase2_trees.push_back(kv.second);
        }
      }
    }

    size_t ticked = 0;
    // 已在 Phase 1 wakeup 中被 Tick 的实体，Phase 2 跳过（避免同帧双 Tick）
    for (auto& tree : phase2_trees) {
      if (tree->IsDestroyed()) continue;
      // Phase 1 已处理的 wakeup 实体在本帧跳过
      if (wakeups.count(tree->OwnerEntity())) continue;

      auto status = tree->Tick();
      ++ticked;

      // 方案 B：Tick 完成后，非 RUNNING 树从 active_set_ 移除
      if (status != NodeStatus::kRunning) {
        EntityId eid = tree->OwnerEntity();
        std::lock_guard<std::mutex> lock(mu_);
        if (trees_.count(eid) && !wakeup_set_.count(eid)) {
          active_set_.erase(eid);
        }
      }
    }

    // 统计 skipped_count：kSkipIdle 下 = 总树数 - 本帧实际 ticked（不含 Phase 1）
    const size_t skipped = (tick_policy_ == TickPolicy::kSkipIdle)
                           ? (total_trees > ticked ? total_trees - ticked : 0)
                           : 0;

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
    // 方案 A：锁内取指针，锁外执行 Tick
    std::shared_ptr<BtTreeInstance> tree_ptr;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = trees_.find(entity_id);
      if (it != trees_.end()) tree_ptr = it->second;
    }
    if (tree_ptr && !tree_ptr->IsDestroyed()) {
      auto status = tree_ptr->Tick();
      if (status != NodeStatus::kRunning) {
        std::lock_guard<std::mutex> lock(mu_);
        if (trees_.count(entity_id) && !wakeup_set_.count(entity_id)) {
          active_set_.erase(entity_id);
        }
      }
    }
  }

  void RequestWakeup(EntityId entity_id) override {
    SIMBT_LOG_INFO_S("BtRuntime: wakeup requested entity=" << entity_id);
    std::lock_guard<std::mutex> lock(mu_);
    // 方案 C：set insert 自动去重，O(1)
    wakeup_set_.insert(entity_id);
    // 方案 B：唤醒的实体加入 active_set_
    active_set_.insert(entity_id);
  }

  void RequestWakeupBatch(const std::vector<EntityId>& ids) override {
    // 一次加锁批量 insert，避免 N 次单独 RequestWakeup 的 N 次锁竞争
    std::lock_guard<std::mutex> lock(mu_);
    for (EntityId id : ids) {
      wakeup_set_.insert(id);
      active_set_.insert(id);
    }
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

 private:
  BT::BehaviorTreeFactory factory_;

  mutable std::mutex mu_;
  std::unordered_map<EntityId, std::shared_ptr<BtTreeInstance>> trees_;

  // 方案 C：wakeup 去重 — unordered_set 替代 queue，insert 自动幂等
  std::unordered_set<EntityId> wakeup_set_;

  // 方案 B：Active Set — 只记录根节点 RUNNING 或等待首帧 Tick 的实体
  std::unordered_set<EntityId> active_set_;

  bool initialized_ = false;

  // Phase 4
  TickPolicy tick_policy_   = TickPolicy::kTickAll;
  TickStats  last_tick_stats_;

#ifdef BTCPP_SQLITE_LOGGING
  std::vector<std::unique_ptr<BT::SqliteLogger>> sqlite_loggers_;
#endif
};

// ── 工厂函数 ─────────────────────────────────────────────────────────────────

std::shared_ptr<IBtRuntime> CreateBtRuntime() {
  return std::make_shared<BtRuntimeImpl>();
}

}  // namespace sim_bt
