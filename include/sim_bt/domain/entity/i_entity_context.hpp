#pragma once

#include <memory>
#include <optional>
#include <string>

#include "sim_bt/common/types.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// IEntityContext
//
// 实体级行为上下文（Domain State Layer — Entity 层）。
//
// 每个仿真实体拥有独立的 EntityContext，存放该实体在本帧决策所需的
// 短生命周期数据（当前目标、局部行为状态、最近规划结果等）。
//
// 生命周期：
//   - 随实体创建，随实体销毁。
//   - 在 BT Tick Domain 内单线程访问（所有 tick 在同一线程序列化）。
//   - TBB worker 绝不直接读写 EntityContext；只能通过 ResultMailbox。
//
// 此接口定义最小公共集合，业务层继承扩展具体域数据。
// ─────────────────────────────────────────────────────────────────────────────
class IEntityContext {
 public:
  virtual ~IEntityContext() = default;

  // 实体标识符。
  virtual EntityId Id() const = 0;

  // ── 当前目标 ────────────────────────────────────────────────────────────────

  // 设置/获取当前追踪目标。无目标时为 kInvalidEntityId。
  virtual void SetCurrentTarget(EntityId target_id) = 0;
  virtual EntityId CurrentTarget() const = 0;

  // ── 状态标签 ─────────────────────────────────────────────────────────────────
  // 供条件节点快速检查实体本帧状态，无需访问完整仿真状态。

  virtual void SetFlag(const std::string& key, bool value) = 0;
  virtual bool GetFlag(const std::string& key, bool default_value = false) const = 0;

  virtual void SetInt(const std::string& key, int64_t value) = 0;
  virtual int64_t GetInt(const std::string& key, int64_t default_value = 0) const = 0;

  virtual void SetFloat(const std::string& key, double value) = 0;
  virtual double GetFloat(const std::string& key, double default_value = 0.0) const = 0;

  // ── 时间 ────────────────────────────────────────────────────────────────────

  // 上次成功 tick 的仿真时间。
  virtual SimTimeMs LastTickTime() const = 0;
  virtual void UpdateLastTickTime(SimTimeMs t) = 0;
};

using EntityContextPtr = std::shared_ptr<IEntityContext>;

}  // namespace sim_bt
