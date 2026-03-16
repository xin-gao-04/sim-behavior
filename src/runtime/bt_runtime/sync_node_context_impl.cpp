#include "sync_node_context_impl.hpp"

namespace sim_bt {

SyncNodeContextImpl::SyncNodeContextImpl(
    EntityId                         owner,
    std::shared_ptr<IEntityContext>  entity_ctx,
    std::shared_ptr<ICommandBus>     command_bus,
    const IWorldSnapshot*            world,
    SimTimeMs*                       sim_time_ptr)
    : owner_(owner),
      entity_ctx_(std::move(entity_ctx)),
      command_bus_(std::move(command_bus)),
      world_(world),
      sim_time_ptr_(sim_time_ptr) {}

}  // namespace sim_bt
