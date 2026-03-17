#include "sim_bt/domain/world/i_world_snapshot.hpp"

#include <algorithm>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace sim_bt {

class WorldSnapshotImpl : public IWorldSnapshot {
 public:
  explicit WorldSnapshotImpl(SimTimeMs ts) : timestamp_(ts) {}

  void AddTarget(TargetEntry entry) {
    index_[entry.id] = targets_.size();
    targets_.push_back(entry);
  }

  SimTimeMs Timestamp() const override { return timestamp_; }

  bool HasEntity(EntityId id) const override {
    return index_.find(id) != index_.end();
  }

  const std::vector<TargetEntry>& Targets() const override { return targets_; }

  std::optional<TargetEntry> FindTarget(EntityId id) const override {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return targets_[it->second];
  }

 private:
  SimTimeMs                            timestamp_;
  std::vector<TargetEntry>             targets_;
  std::unordered_map<EntityId, size_t> index_;
};

// ─────────────────────────────────────────────────────────────────────────────
// SimpleWorldSnapshotProvider
//
// 轻量快照提供者，外部通过 Build() 注入新快照，Refresh() 替换当前版本。
// ─────────────────────────────────────────────────────────────────────────────
class SimpleWorldSnapshotProvider : public IWorldSnapshotProvider {
 public:
  void Refresh(SimTimeMs sim_time_ms) override {
    // 默认行为：创建空快照（实际场景中由仿真宿主注入真实数据）
    std::lock_guard<std::mutex> lock(mu_);
    current_ = std::make_shared<WorldSnapshotImpl>(sim_time_ms);
  }

  // 注入一个已构建好的快照（供仿真宿主使用）
  void InjectSnapshot(std::shared_ptr<WorldSnapshotImpl> snap) {
    std::lock_guard<std::mutex> lock(mu_);
    current_ = std::move(snap);
  }

  WorldSnapshotPtr Current() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
  }

 private:
  mutable std::mutex              mu_;
  std::shared_ptr<WorldSnapshotImpl> current_;
};

}  // namespace sim_bt

// ── 工厂函数（跨模块可见）─────────────────────────────────────────────────────
std::shared_ptr<sim_bt::IWorldSnapshotProvider> CreateSimpleWorldSnapshotProvider() {
  return std::make_shared<sim_bt::SimpleWorldSnapshotProvider>();
}
