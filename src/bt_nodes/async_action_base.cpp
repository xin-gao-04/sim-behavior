#include "sim_bt/bt_nodes/async_action_base.hpp"

namespace sim_bt {

AsyncActionBase::AsyncActionBase(const std::string& name,
                                 const BT::NodeConfig& config,
                                 AsyncActionContextPtr ctx)
    : BT::StatefulActionNode(name, config), ctx_(std::move(ctx)) {}

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
