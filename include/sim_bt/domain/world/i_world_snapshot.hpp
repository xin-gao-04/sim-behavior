#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "sim_bt/common/types.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// IWorldSnapshot
//
// 全局只读态势快照（Domain State Layer — World 层）。
//
// 每仿真帧开始时由 WorldSnapshotProvider 刷新生成，行为树只读取它，
// 不直接修改。这确保 BT tick 中观察到的世界状态在整帧内保持一致。
//
// 接口采用纯虚方式定义，具体域对象由业务层扩展。
// 这里只定义运行时和节点通用所需的最小集合。
// ─────────────────────────────────────────────────────────────────────────────

// 目标条目（从态势快照中提取的目标摘要）
struct TargetEntry {
  EntityId    id          = kInvalidEntityId;
  float       pos_x       = 0.0f;
  float       pos_y       = 0.0f;
  float       pos_z       = 0.0f;
  float       threat_level = 0.0f;  // 0.0 ~ 1.0
  bool        is_hostile  = false;
};

class IWorldSnapshot {
 public:
  virtual ~IWorldSnapshot() = default;

  // 当前快照对应的仿真时间戳（毫秒）。
  virtual SimTimeMs Timestamp() const = 0;

  // 查询实体是否存在于此快照。
  virtual bool HasEntity(EntityId id) const = 0;

  // 获取快照中所有目标列表（只读引用，生命周期与 snapshot 相同）。
  virtual const std::vector<TargetEntry>& Targets() const = 0;

  // 查询单个目标。不存在时返回 std::nullopt。
  virtual std::optional<TargetEntry> FindTarget(EntityId id) const = 0;
};

using WorldSnapshotPtr = std::shared_ptr<const IWorldSnapshot>;

// ─────────────────────────────────────────────────────────────────────────────
// IWorldSnapshotProvider
//
// 快照提供者。每帧开始由 SimHost / BtRuntime 调用 Refresh()
// 生成新快照；BT 节点通过 Current() 获取当前帧快照。
// ─────────────────────────────────────────────────────────────────────────────
class IWorldSnapshotProvider {
 public:
  virtual ~IWorldSnapshotProvider() = default;

  // 生成新快照并替换当前快照。线程安全（通常在 BT tick 开始前调用）。
  virtual void Refresh(SimTimeMs sim_time_ms) = 0;

  // 返回当前帧快照。BT 节点在整帧内持有此指针安全。
  virtual WorldSnapshotPtr Current() const = 0;
};

using WorldSnapshotProviderPtr = std::shared_ptr<IWorldSnapshotProvider>;

}  // namespace sim_bt
