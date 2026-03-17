#include "sim_bt/bt_nodes/condition_base.hpp"

static constexpr const char* kBlackboardKeySyncCtx = "__sync_ctx__";

namespace sim_bt {

ConditionBase::ConditionBase(const std::string& name,
                             const BT::NodeConfig& config,
                             SyncNodeContextPtr ctx)
    : BT::ConditionNode(name, config), ctx_(std::move(ctx)) {
  // Blackboard 回退：多实体场景下由 BtRuntimeImpl::CreateTree 注入 per-entity ctx
  if (!ctx_ && config.blackboard) {
    try {
      ctx_ = config.blackboard->get<SyncNodeContextPtr>(kBlackboardKeySyncCtx);
    } catch (...) {}
  }
}

BT::NodeStatus ConditionBase::tick() {
  return Check() ? BT::NodeStatus::SUCCESS : BT::NodeStatus::FAILURE;
}

EntityId ConditionBase::OwnerEntity() const {
  return ctx_->OwnerEntity();
}

}  // namespace sim_bt
