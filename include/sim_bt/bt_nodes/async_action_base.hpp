#pragma once

// NOTE: 本文件依赖 BehaviorTree.CPP（behaviortree_cpp）。
// 在 CMake 中须链接 BT::behaviortree_cpp_v4 或同等目标。
#include <behaviortree_cpp/action_node.h>

#include <chrono>
#include <string>

#include "sim_bt/bt_nodes/i_async_action_context.hpp"
#include "sim_bt/common/types.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// AsyncActionBase
//
// 所有异步动作节点的基类，继承自 BT::StatefulActionNode。
//
// BehaviorTree.CPP 的 StatefulActionNode 为 request-reply 异步动作
// 提供了标准模型：
//   onStart()   — 首次 tick，通常在这里提交任务，返回 RUNNING
//   onRunning() — 后续 tick，检查结果，返回 RUNNING/SUCCESS/FAILURE
//   onHalted()  — 树主动 halt（父节点取消、树重置等），做清理工作
//
// AsyncActionBase 在此基础上增加：
//   1. 持有 IAsyncActionContext，屏蔽 uvw/TBB 细节
//   2. 提供默认的超时处理（IsTimedOut() 检查）
//   3. 标准化 OnStart/OnRunning/OnHalted 命名（Pascal case）供子类覆盖
//
// 子类实现示例：
//   class PathPlanAction : public AsyncActionBase {
//    public:
//     PathPlanAction(const std::string& name,
//                    const BT::NodeConfig& config,
//                    AsyncActionContextPtr ctx)
//         : AsyncActionBase(name, config, std::move(ctx)) {}
//
//     NodeStatus OnStart() override { ... }
//     NodeStatus OnRunning() override { ... }
//     void OnHalted() override { ... }
//   };
// ─────────────────────────────────────────────────────────────────────────────
class AsyncActionBase : public BT::StatefulActionNode {
 public:
  // ctx 可选：
  //   - 若显式传入，则使用该 ctx（适合 registerBuilder 捕获 ctx 的场景）。
  //   - 若为 nullptr，构造时自动从 Blackboard["__async_ctx__"] 读取
  //     （需由 BtRuntimeImpl::CreateTree 提前注入，适合多实体场景）。
  AsyncActionBase(const std::string& name,
                  const BT::NodeConfig& config,
                  AsyncActionContextPtr ctx = nullptr);

  virtual ~AsyncActionBase() = default;

  // BT::StatefulActionNode 接口（final，不可再覆盖，转发到 On* 方法）
  BT::NodeStatus onStart()   final;
  BT::NodeStatus onRunning() final;
  void           onHalted()  final;

 protected:
  // ── 子类实现接口 ─────────────────────────────────────────────────────────────

  // 首次 tick：提交任务、注册定时器、返回 kRunning 或立即完成。
  virtual NodeStatus OnStart() = 0;

  // 后续 tick：检查结果，返回 kRunning/kSuccess/kFailure。
  // 若 ctx_->IsTimedOut() 返回 true，通常应返回 kFailure。
  virtual NodeStatus OnRunning() = 0;

  // 清理：取消 CPU 任务、取消超时定时器。
  virtual void OnHalted() = 0;

  // ── 便捷访问器 ───────────────────────────────────────────────────────────────

  IAsyncActionContext& Ctx() { return *ctx_; }
  const IAsyncActionContext& Ctx() const { return *ctx_; }

  // 当前实体 ID（从 ctx_ 获取）。
  EntityId OwnerEntity() const;

 private:
  AsyncActionContextPtr ctx_;
};

}  // namespace sim_bt
