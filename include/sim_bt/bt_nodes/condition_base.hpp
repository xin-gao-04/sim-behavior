#pragma once

#include <behaviortree_cpp/condition_node.h>

#include "sim_bt/bt_nodes/i_sync_node_context.hpp"
#include "sim_bt/common/types.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// ConditionBase
//
// 所有同步条件节点的基类，继承自 BT::ConditionNode。
//
// BehaviorTree.CPP 的 ConditionNode 每帧都会被 tick（无论上次状态如何），
// 调用 tick() 方法，返回 SUCCESS 或 FAILURE，不允许返回 RUNNING。
//
// ConditionBase 在此基础上增加：
//   1. 持有 ISyncNodeContext，提供 EntityContext / WorldSnapshot 访问
//   2. 将 BT::NodeStatus tick() 转发给纯虚 Check()，子类只需返回 bool
//   3. 统一 false → FAILURE、true → SUCCESS 映射
//
// 子类实现示例：
//   class HasTargetCondition : public ConditionBase {
//    public:
//     HasTargetCondition(const std::string& name,
//                        const BT::NodeConfig& config,
//                        SyncNodeContextPtr ctx)
//         : ConditionBase(name, config, std::move(ctx)) {}
//
//     bool Check() override {
//       return Ctx().Entity().CurrentTarget() != kInvalidEntityId;
//     }
//   };
//
// 执行约束：
//   Check() 必须在主线程内微秒级完成，不得阻塞或调用 TBB/uvw。
// ─────────────────────────────────────────────────────────────────────────────
class ConditionBase : public BT::ConditionNode {
 public:
  // ctx 可选（默认 nullptr）：
  //   若不传，构造时自动从 Blackboard["__sync_ctx__"] 读取（多实体场景）。
  ConditionBase(const std::string& name,
                const BT::NodeConfig& config,
                SyncNodeContextPtr ctx = nullptr);

  virtual ~ConditionBase() = default;

  // BT::ConditionNode 接口（final，不可再覆盖，转发到 Check()）
  BT::NodeStatus tick() final;

 protected:
  // 子类实现：当前帧条件是否成立。
  // 返回 true  → BT::NodeStatus::SUCCESS
  // 返回 false → BT::NodeStatus::FAILURE
  virtual bool Check() = 0;

  // ── 便捷访问器 ─────────────────────────────────────────────────────────────

  ISyncNodeContext&       Ctx()       { return *ctx_; }
  const ISyncNodeContext& Ctx() const { return *ctx_; }

  EntityId OwnerEntity() const;

 private:
  SyncNodeContextPtr ctx_;
};

}  // namespace sim_bt
