#pragma once

#include <memory>

#include "sim_bt/bt_nodes/i_sync_node_context.hpp"
#include "sim_bt/adapters/i_command_bus.hpp"
#include "sim_bt/domain/entity/i_entity_context.hpp"
#include "sim_bt/domain/group/i_group_context.hpp"
#include "sim_bt/domain/world/i_world_snapshot.hpp"
#include "sim_bt/common/types.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// SyncNodeContextImpl
//
// ISyncNodeContext 的标准实现。
// 每个同步节点（ConditionBase / SyncActionBase 子类）持有一个实例，
// 通过构造函数注入所需的共享对象。
//
// 生命周期说明：
//   - entity_ctx  与实体共生命周期
//   - world       每帧由 SimHostApp 刷新，此处持只读指针（裸指针，不拥有）
//   - command_bus 进程共享，与 SimHostApp 相同生命周期
// ─────────────────────────────────────────────────────────────────────────────
class SyncNodeContextImpl : public ISyncNodeContext {
 public:
  SyncNodeContextImpl(EntityId                         owner,
                      std::shared_ptr<IEntityContext>  entity_ctx,
                      std::shared_ptr<ICommandBus>     command_bus,
                      const IWorldSnapshot*            world,
                      SimTimeMs*                       sim_time_ptr,
                      std::shared_ptr<IGroupContext>   group_ctx = nullptr);

  // ── ISyncNodeContext ───────────────────────────────────────────────────────

  IEntityContext&       Entity()       override { return *entity_ctx_; }
  const IEntityContext& Entity() const override { return *entity_ctx_; }

  const IWorldSnapshot* World() const override { return world_; }

  ICommandBus& CommandBus() override { return *command_bus_; }

  IGroupContext*       Group()       override { return group_ctx_.get(); }
  const IGroupContext* Group() const override { return group_ctx_.get(); }

  EntityId  OwnerEntity()    const override { return owner_; }
  SimTimeMs CurrentSimTime() const override {
    return sim_time_ptr_ ? *sim_time_ptr_ : 0;
  }

  // ── 运行时更新 ─────────────────────────────────────────────────────────────

  // 每帧由 SimHostApp 更新全局快照指针（快照本身是只读的，不拷贝）。
  void SetWorldSnapshot(const IWorldSnapshot* world) { world_ = world; }

  // 加入编队时由 SimHostApp 设置（编队解散时设为 nullptr）。
  void SetGroupContext(std::shared_ptr<IGroupContext> group_ctx) {
    group_ctx_ = std::move(group_ctx);
  }

 private:
  EntityId                        owner_;
  std::shared_ptr<IEntityContext> entity_ctx_;
  std::shared_ptr<ICommandBus>    command_bus_;
  std::shared_ptr<IGroupContext>  group_ctx_;   // 可空，未加入编队时为 nullptr
  const IWorldSnapshot*           world_;      // 非拥有，每帧由外部刷新
  SimTimeMs*                      sim_time_ptr_;
};

}  // namespace sim_bt
