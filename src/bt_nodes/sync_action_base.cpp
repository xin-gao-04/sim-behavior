#include "sim_bt/bt_nodes/sync_action_base.hpp"

namespace sim_bt {

SyncActionBase::SyncActionBase(const std::string& name,
                               const BT::NodeConfig& config,
                               SyncNodeContextPtr ctx)
    : BT::SyncActionNode(name, config), ctx_(std::move(ctx)) {}

BT::NodeStatus SyncActionBase::tick() {
  NodeStatus s = Execute();
  return static_cast<BT::NodeStatus>(s);
}

EntityId SyncActionBase::OwnerEntity() const {
  return ctx_->OwnerEntity();
}

}  // namespace sim_bt
