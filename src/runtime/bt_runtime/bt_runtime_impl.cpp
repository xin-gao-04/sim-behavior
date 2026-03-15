// ─────────────────────────────────────────────────────────────────────────────
// BtRuntimeImpl — IBtRuntime 的 BehaviorTree.CPP 实现
// ─────────────────────────────────────────────────────────────────────────────
#include "sim_bt/runtime/bt_runtime/i_bt_runtime.hpp"

#include <behaviortree_cpp/bt_factory.h>

#include <atomic>
#include <mutex>
#include <queue>
#include <unordered_map>

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

 private:
  EntityId owner_;
  BT::Tree tree_;
};

// ── BtRuntimeImpl ─────────────────────────────────────────────────────────────

class BtRuntimeImpl : public IBtRuntime {
 public:
  BtRuntimeImpl() = default;
  ~BtRuntimeImpl() override { Shutdown(); }

  SimStatus Initialize() override {
    initialized_ = true;
    return SimStatus::Ok();
  }

  void Shutdown() override {
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

  SimResult<TreeInstancePtr> CreateTree(EntityId entity_id,
                                         const std::string& tree_name) override {
    try {
      BT::Tree bt_tree = factory_.createTree(tree_name);
      auto inst = std::make_shared<BtTreeInstance>(entity_id, std::move(bt_tree));
      {
        std::lock_guard<std::mutex> lock(mu_);
        trees_[entity_id] = inst;
      }
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
      if (it->second) it->second->Halt();
      trees_.erase(it);
    }
  }

  TreeInstancePtr GetTree(EntityId entity_id) const override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = trees_.find(entity_id);
    return (it != trees_.end()) ? it->second : nullptr;
  }

  void TickAll(SimTimeMs /*sim_time_ms*/) override {
    // 先处理 wakeup 队列，确保外部请求在本帧生效
    std::queue<EntityId> wakeups;
    {
      std::lock_guard<std::mutex> lock(mu_);
      std::swap(wakeups, wakeup_queue_);
    }
    while (!wakeups.empty()) {
      EntityId id = wakeups.front();
      wakeups.pop();
      TickEntityLocked(id);
    }

    // 逐实体 tick 所有活跃树
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& kv : trees_) {
      kv.second->Tick();
    }
  }

  void TickEntity(EntityId entity_id) override {
    TickEntityLocked(entity_id);
  }

  void RequestWakeup(EntityId entity_id) override {
    std::lock_guard<std::mutex> lock(mu_);
    wakeup_queue_.push(entity_id);
  }

  size_t ActiveTreeCount() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return trees_.size();
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
};

// ── 工厂函数 ─────────────────────────────────────────────────────────────────

std::shared_ptr<IBtRuntime> CreateBtRuntime() {
  return std::make_shared<BtRuntimeImpl>();
}

}  // namespace sim_bt
