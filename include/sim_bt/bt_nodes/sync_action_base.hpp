#pragma once

#include <behaviortree_cpp/action_node.h>

#include "sim_bt/bt_nodes/i_sync_node_context.hpp"
#include "sim_bt/common/types.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// SyncActionBase
//
// 所有同步瞬时动作节点的基类，继承自 BT::SyncActionNode。
//
// BehaviorTree.CPP 的 SyncActionNode 每次被 tick 都完整执行，
// 在同一帧内完成并返回 SUCCESS 或 FAILURE，不允许返回 RUNNING。
//
// SyncActionBase 在此基础上增加：
//   1. 持有 ISyncNodeContext，提供 EntityContext / CommandBus 访问
//   2. 将 BT::NodeStatus tick() 转发给纯虚 Execute()
//   3. 统一 NodeStatus ↔ BT::NodeStatus 枚举映射
//
// 典型使用场景：
//   - 写入 EntityContext 标志（SetTargetLocked, ClearFlag 等）
//   - 向 CommandBus 派发瞬时命令（EmitCeaseFire, NotifyGroupReady 等）
//   - 更新仿真参数（SetSpeed, ChangeFormation 等）
//
// 子类实现示例：
//   class SetFlagAction : public SyncActionBase {
//    public:
//     SetFlagAction(const std::string& name,
//                   const BT::NodeConfig& config,
//                   SyncNodeContextPtr ctx)
//         : SyncActionBase(name, config, std::move(ctx)) {}
//
//     NodeStatus Execute() override {
//       Ctx().Entity().SetFlag("engaged", true);
//       return NodeStatus::kSuccess;
//     }
//   };
//
// 执行约束：
//   Execute() 必须在主线程内完成，不得阻塞或调用 TBB/uvw。
//   若需要异步计算，应改用 AsyncActionBase。
// ─────────────────────────────────────────────────────────────────────────────
class SyncActionBase : public BT::SyncActionNode {
 public:
  // ctx 可选（默认 nullptr）：
  //   若不传，构造时自动从 Blackboard["__sync_ctx__"] 读取（多实体场景）。
  SyncActionBase(const std::string& name,
                 const BT::NodeConfig& config,
                 SyncNodeContextPtr ctx = nullptr);

  virtual ~SyncActionBase() = default;

  // BT::SyncActionNode 接口（final，不可再覆盖，转发到 Execute()）
  BT::NodeStatus tick() final;

 protected:
  // 子类实现：执行瞬时动作，返回执行结果。
  // 只允许返回 kSuccess 或 kFailure，不得返回 kRunning。
  virtual NodeStatus Execute() = 0;

  // ── 便捷访问器 ─────────────────────────────────────────────────────────────

  ISyncNodeContext&       Ctx()       { return *ctx_; }
  const ISyncNodeContext& Ctx() const { return *ctx_; }

  EntityId OwnerEntity() const;

 private:
  SyncNodeContextPtr ctx_;
};

}  // namespace sim_bt
