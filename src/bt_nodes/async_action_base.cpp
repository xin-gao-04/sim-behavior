#include "sim_bt/bt_nodes/async_action_base.hpp"

// Blackboard key（与 bt_runtime_impl.cpp 中的常量保持一致）
static constexpr const char* kBlackboardKeyAsyncCtx = "__async_ctx__";

namespace sim_bt {

AsyncActionBase::AsyncActionBase(const std::string& name,
                                 const BT::NodeConfig& config,
                                 AsyncActionContextPtr ctx)
    : BT::StatefulActionNode(name, config), ctx_(std::move(ctx)) {
  // 若调用方未显式传入 ctx（工厂 builder 未捕获 ctx 的场景），
  // 尝试从 Blackboard 读取 per-entity 上下文（由 BtRuntimeImpl::CreateTree 注入）。
  if (!ctx_ && config.blackboard) {
    try {
      ctx_ = config.blackboard->get<AsyncActionContextPtr>(kBlackboardKeyAsyncCtx);
    } catch (...) {
      // Blackboard 中也没有，节点将在首次使用 Ctx() 时断言失败
    }
  }
}

BT::NodeStatus AsyncActionBase::onStart() {
  NodeStatus s = OnStart();
  return static_cast<BT::NodeStatus>(s);
}

BT::NodeStatus AsyncActionBase::onRunning() {
  if (ctx_->IsTimedOut()) {
    OnHalted();
    return BT::NodeStatus::FAILURE;
  }
  NodeStatus s = OnRunning();
  return static_cast<BT::NodeStatus>(s);
}

void AsyncActionBase::onHalted() {
  ctx_->CancelTimeout();
  OnHalted();
}

EntityId AsyncActionBase::OwnerEntity() const {
  return ctx_->OwnerEntity();
}

}  // namespace sim_bt
