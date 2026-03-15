#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sim_bt/common/types.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// IGroupContext
//
// 编队/分队共享上下文（Domain State Layer — Group 层）。
//
// 编队内多个实体共享同一 GroupContext，用于协调编队级战术意图，
// 例如集结点、编队目标区、协调规则、编队行动状态。
//
// 访问约束：
//   - GroupContext 在 BT Tick Domain 内访问，编队内所有实体 tick 串行化。
//   - 不允许在 TBB worker 中直接读写 GroupContext。
// ─────────────────────────────────────────────────────────────────────────────
class IGroupContext {
 public:
  virtual ~IGroupContext() = default;

  virtual GroupId Id() const = 0;

  // ── 成员管理 ────────────────────────────────────────────────────────────────

  virtual const std::vector<EntityId>& Members() const = 0;
  virtual bool IsMember(EntityId id) const = 0;

  // ── 共享战术状态 ─────────────────────────────────────────────────────────────

  // 编队目标区坐标（地形坐标系）
  virtual void SetObjectivePosition(float x, float y, float z) = 0;
  virtual void GetObjectivePosition(float& x, float& y, float& z) const = 0;

  // 集结点
  virtual void SetRallyPoint(float x, float y, float z) = 0;
  virtual void GetRallyPoint(float& x, float& y, float& z) const = 0;

  // 编队规则标志（如"允许主动开火""维持编队队形"）
  virtual void SetRule(const std::string& key, bool value) = 0;
  virtual bool GetRule(const std::string& key, bool default_value = false) const = 0;

  // ── 编队决策结果 ─────────────────────────────────────────────────────────────

  // 最近一次编队级规划结果引用 ID（与 ResultMailbox 中的 job_id 对应）
  virtual void SetLastPlanJobId(uint64_t job_id) = 0;
  virtual uint64_t LastPlanJobId() const = 0;
};

using GroupContextPtr = std::shared_ptr<IGroupContext>;

}  // namespace sim_bt
