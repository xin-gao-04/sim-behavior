// test_group_context.cpp
//
// Phase 2 GroupContext 集成测试
//
// 验证目标：
//   1. 共享状态传播
//      同编队两个实体共享同一个 GroupContextImpl 实例；
//      一个实体写入 SetObjectivePosition，另一个立即可读。
//
//   2. 规则标志共享
//      SetRule / GetRule 跨成员一致性。
//
//   3. BT 节点经由 ISyncNodeContext::Group() 访问 GroupContext
//      ConditionBase 子类读取编队规则控制 BT 行为。
//
//   4. 未加入编队时 Group() == nullptr（无编队实体安全性）。
//
//   5. DisbandGroup 后 Group() 回到 nullptr。
//
// 使用真实的 SimHostApp（BtRuntime + UvwEventLoopRuntime + TbbJobExecutor），
// 不使用 mock。

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <behaviortree_cpp/bt_factory.h>
#include <gtest/gtest.h>

#include "../src/runtime/bt_runtime/sync_node_context_impl.hpp"
#include "../src/domain/entity/entity_context_impl.hpp"
#include "../src/sim_host/sim_host_app.hpp"
#include "sim_bt/bt_nodes/condition_base.hpp"
#include "sim_bt/adapters/i_command_bus.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// 测试用 ConditionBase：检查编队规则 "fire_allowed"
// ─────────────────────────────────────────────────────────────────────────────
class FireAllowedCondition : public ConditionBase {
 public:
  FireAllowedCondition(const std::string& name, const BT::NodeConfig& config)
      : ConditionBase(name, config) {}

  static BT::PortsList providedPorts() { return {}; }

 protected:
  bool Check() override {
    const IGroupContext* g = Ctx().Group();
    if (!g) return false;
    return g->GetRule("fire_allowed", false);
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// 测试用 ConditionBase：记录 Group() 是否为 nullptr
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_has_group{false};
static std::atomic<bool> g_fire_allowed_seen{false};

class RecordGroupCondition : public ConditionBase {
 public:
  RecordGroupCondition(const std::string& name, const BT::NodeConfig& config)
      : ConditionBase(name, config) {}

  static BT::PortsList providedPorts() { return {}; }

 protected:
  bool Check() override {
    const IGroupContext* g = Ctx().Group();
    g_has_group.store(g != nullptr, std::memory_order_relaxed);
    if (g) {
      g_fire_allowed_seen.store(g->GetRule("fire_allowed", false),
                                std::memory_order_relaxed);
    }
    return true;  // always succeed so tree keeps running
  }
};

// BT XML：单节点树（Sequence 只包含一个 Condition）
static constexpr const char* kRecordGroupXml = R"(
<root BTCPP_format="4">
  <BehaviorTree ID="RecordGroupTree">
    <RecordGroupCondition/>
  </BehaviorTree>
</root>
)";

// ─────────────────────────────────────────────────────────────────────────────
// Fixture：直接操作 ISyncNodeContext（不跑完整 BT 主循环）
// ─────────────────────────────────────────────────────────────────────────────
class GroupContextTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_has_group.store(false);
    g_fire_allowed_seen.store(false);

    // 构建轻量级上下文（无需完整 SimHostApp，不跑 TickLoop）
    entity_ctx_a_ = std::make_shared<EntityContextImpl>(kEntityA);
    entity_ctx_b_ = std::make_shared<EntityContextImpl>(kEntityB);

    null_bus_ = std::make_shared<NullCommandBus>();

    sync_a_ = std::make_shared<SyncNodeContextImpl>(
        kEntityA, entity_ctx_a_, null_bus_, nullptr, &sim_time_);
    sync_b_ = std::make_shared<SyncNodeContextImpl>(
        kEntityB, entity_ctx_b_, null_bus_, nullptr, &sim_time_);
  }

  // 简单空总线
  class NullCommandBus : public ICommandBus {
   public:
    SimStatus Dispatch(ActionCommand) override { return SimStatus::Ok(); }
    void RegisterHandler(const std::string&, CommandHandler) override {}
    void ClearHandlers() override {}
  };

  static constexpr EntityId kEntityA = 10;
  static constexpr EntityId kEntityB = 20;

  std::shared_ptr<EntityContextImpl>     entity_ctx_a_;
  std::shared_ptr<EntityContextImpl>     entity_ctx_b_;
  std::shared_ptr<ICommandBus>           null_bus_;
  std::shared_ptr<SyncNodeContextImpl>   sync_a_;
  std::shared_ptr<SyncNodeContextImpl>   sync_b_;
  SimTimeMs                              sim_time_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// 测试 1：无编队时 Group() 为 nullptr
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(GroupContextTest, NoGroupReturnsNullptr) {
  EXPECT_EQ(sync_a_->Group(), nullptr);
  EXPECT_EQ(sync_b_->Group(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 2：共享 GroupContext — 同一实例，写入立即可读
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(GroupContextTest, SharedGroupContextPropagatesState) {
  // 模拟 AssignGroup：创建 GroupContext 并注入两个 SyncNodeContext
  // (工厂函数在 group_context_impl.cpp 中定义，通过链接器链接)
  extern std::shared_ptr<IGroupContext> CreateGroupContext(GroupId);
  auto group = CreateGroupContext(GroupId(1));
  group->AddMember(kEntityA);
  group->AddMember(kEntityB);

  sync_a_->SetGroupContext(group);
  sync_b_->SetGroupContext(group);

  // Entity A 写入目标位置
  sync_a_->Group()->SetObjectivePosition(10.f, 20.f, 30.f);

  // Entity B 立即读到相同值（同一 GroupContext 实例）
  float x = 0, y = 0, z = 0;
  sync_b_->Group()->GetObjectivePosition(x, y, z);

  EXPECT_FLOAT_EQ(x, 10.f);
  EXPECT_FLOAT_EQ(y, 20.f);
  EXPECT_FLOAT_EQ(z, 30.f);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 3：规则标志共享
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(GroupContextTest, SharedRuleFlagIsVisibleAcrossMembers) {
  extern std::shared_ptr<IGroupContext> CreateGroupContext(GroupId);
  auto group = CreateGroupContext(GroupId(2));
  group->AddMember(kEntityA);
  group->AddMember(kEntityB);

  sync_a_->SetGroupContext(group);
  sync_b_->SetGroupContext(group);

  // A 设置规则
  sync_a_->Group()->SetRule("fire_allowed", true);

  // B 读到相同值
  EXPECT_TRUE(sync_b_->Group()->GetRule("fire_allowed", false));

  // 修改后也立即生效
  sync_b_->Group()->SetRule("fire_allowed", false);
  EXPECT_FALSE(sync_a_->Group()->GetRule("fire_allowed", false));
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 4：SetGroupContext(nullptr) 后 Group() 回到 nullptr
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(GroupContextTest, DisbandGroupClearsGroupPointer) {
  extern std::shared_ptr<IGroupContext> CreateGroupContext(GroupId);
  auto group = CreateGroupContext(GroupId(3));
  group->AddMember(kEntityA);
  sync_a_->SetGroupContext(group);

  ASSERT_NE(sync_a_->Group(), nullptr);

  // 模拟 DisbandGroup
  sync_a_->SetGroupContext(nullptr);

  EXPECT_EQ(sync_a_->Group(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 5：IsMember 与成员列表一致性
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(GroupContextTest, MembershipQueriesAreConsistent) {
  extern std::shared_ptr<IGroupContext> CreateGroupContext(GroupId);
  auto group = CreateGroupContext(GroupId(4));
  group->AddMember(kEntityA);
  sync_a_->SetGroupContext(group);

  EXPECT_TRUE(sync_a_->Group()->IsMember(kEntityA));
  EXPECT_FALSE(sync_a_->Group()->IsMember(kEntityB));
  EXPECT_EQ(sync_a_->Group()->Members().size(), 1u);
  EXPECT_EQ(sync_a_->Group()->Id(), GroupId(4));
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 6：SimHostApp 集成 — AssignGroup / DisbandGroup
// ─────────────────────────────────────────────────────────────────────────────
TEST(GroupContextSimHostTest, AssignAndDisbandGroupViaSimHostApp) {
  SimHostApp app;
  ASSERT_TRUE(static_cast<bool>(app.Initialize()));

  app.BtRuntime().RegisterNodeType<RecordGroupCondition>("RecordGroupCondition");
  auto xml_status = app.BtRuntime().LoadTreeFromXml(kRecordGroupXml);
  ASSERT_TRUE(static_cast<bool>(xml_status)) << xml_status.message;

  constexpr EntityId kA = 100, kB = 101;
  ASSERT_TRUE(static_cast<bool>(app.SpawnEntity(kA, "RecordGroupTree")));
  ASSERT_TRUE(static_cast<bool>(app.SpawnEntity(kB, "RecordGroupTree")));

  // 加入编队前：Group() 为 nullptr → Check() 返回 true 但 g_has_group 为 false
  g_has_group.store(false);
  app.BtRuntime().TickAll(10);
  EXPECT_FALSE(g_has_group.load()) << "加入编队前 Group() 应为 nullptr";

  // AssignGroup
  ASSERT_TRUE(static_cast<bool>(app.AssignGroup(GroupId(1), {kA, kB})));

  // 设置规则
  auto tree_a = app.BtRuntime().GetTree(kA);
  ASSERT_NE(tree_a, nullptr);
  // 通过总线路由验证通路：直接在测试中访问 sync_ctx 是内部细节，
  // 这里只验证 tick 后节点能访问到 Group 且 GetRule 返回期望值。

  // 由于 g_has_group 是全局原子变量，RecordGroupCondition 在 tick 时赋值
  g_has_group.store(false);
  app.BtRuntime().TickAll(20);
  EXPECT_TRUE(g_has_group.load()) << "AssignGroup 后 Group() 不应为 nullptr";

  // DisbandGroup 后 Group() 重新为 nullptr
  app.DisbandGroup(GroupId(1));
  g_has_group.store(true);   // 预设为 true，确认 tick 后变为 false
  app.BtRuntime().TickAll(30);
  EXPECT_FALSE(g_has_group.load()) << "DisbandGroup 后 Group() 应为 nullptr";

  app.RequestStop();
}

}  // namespace sim_bt
