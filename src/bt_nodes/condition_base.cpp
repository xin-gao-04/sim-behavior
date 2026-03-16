#include "sim_bt/bt_nodes/condition_base.hpp"

namespace sim_bt {

ConditionBase::ConditionBase(const std::string& name,
                             const BT::NodeConfig& config,
                             SyncNodeContextPtr ctx)
    : BT::ConditionNode(name, config), ctx_(std::move(ctx)) {}

BT::NodeStatus ConditionBase::tick() {
  return Check() ? BT::NodeStatus::SUCCESS : BT::NodeStatus::FAILURE;
}

EntityId ConditionBase::OwnerEntity() const {
  return ctx_->OwnerEntity();
}

}  // namespace sim_bt
