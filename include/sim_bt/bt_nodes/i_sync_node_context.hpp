#pragma once

#include <memory>

#include "sim_bt/adapters/i_command_bus.hpp"
#include "sim_bt/common/types.hpp"
#include "sim_bt/domain/entity/i_entity_context.hpp"
#include "sim_bt/domain/group/i_group_context.hpp"
#include "sim_bt/domain/world/i_world_snapshot.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// ISyncNodeContext
//
// 同步节点（ConditionBase / SyncActionBase）的运行时上下文 Facade。
//
// 设计原则与 IAsyncActionContext 对称：
//   - 同步节点只依赖此接口，不持有具体实现或仿真对象指针。
//   - 接口最小化：同步节点不需要 TBB/uvw，仅需读写实体状态和派发命令。
//   - 可在测试中注入 mock 实现，节点逻辑可单元测试。
//
// 同步节点的执行约束：
//   - 必须在 BT Tick Domain（主线程）内完成，不得调用阻塞操作。
//   - 不得启动 TBB 任务或与 uvw loop 交互。
//   - 典型耗时 < 1μs（条件检查）或 < 10μs（瞬时动作）。
// ─────────────────────────────────────────────────────────────────────────────
class ISyncNodeContext {
 public:
  virtual ~ISyncNodeContext() = default;

  // ── 实体状态访问 ──────────────────────────────────────────────────────────

  // 当前实体的私有上下文（跨帧持久，标志位/整型/浮点型/当前目标）。
  virtual IEntityContext& Entity() = 0;
  virtual const IEntityContext& Entity() const = 0;

  // ── 全局只读快照 ──────────────────────────────────────────────────────────

  // 本帧全局只读态势快照。可能为 nullptr（快照尚未初始化时）。
  // BT 节点只读取，不修改。
  virtual const IWorldSnapshot* World() const = 0;

  // ── 命令派发 ──────────────────────────────────────────────────────────────

  // 命令总线，用于向仿真宿主派发动作命令（move_to, cease_fire 等）。
  virtual ICommandBus& CommandBus() = 0;

  // ── 编队共享上下文 ─────────────────────────────────────────────────────────

  // 当前实体所属编队的共享上下文。如实体未加入任何编队则返回 nullptr。
  // BT 节点只在 BT Tick Domain 内访问，编队成员 tick 串行化，无需加锁。
  virtual IGroupContext* Group() = 0;
  virtual const IGroupContext* Group() const = 0;

  // ── 上下文查询 ────────────────────────────────────────────────────────────

  virtual EntityId OwnerEntity() const = 0;
  virtual SimTimeMs CurrentSimTime() const = 0;
};

using SyncNodeContextPtr = std::shared_ptr<ISyncNodeContext>;

}  // namespace sim_bt
