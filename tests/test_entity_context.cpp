#include <gtest/gtest.h>
#include "../src/domain/entity/entity_context_impl.hpp"

namespace sim_bt {

TEST(EntityContext, DefaultState) {
  EntityContextImpl ctx(42);
  EXPECT_EQ(ctx.Id(), 42u);
  EXPECT_EQ(ctx.CurrentTarget(), kInvalidEntityId);
  EXPECT_EQ(ctx.LastTickTime(), 0u);
}

TEST(EntityContext, SetCurrentTarget) {
  EntityContextImpl ctx(1);
  ctx.SetCurrentTarget(100);
  EXPECT_EQ(ctx.CurrentTarget(), 100u);
  ctx.SetCurrentTarget(kInvalidEntityId);
  EXPECT_EQ(ctx.CurrentTarget(), kInvalidEntityId);
}

TEST(EntityContext, FlagStorage) {
  EntityContextImpl ctx(1);
  EXPECT_FALSE(ctx.GetFlag("weapon_loaded", false));
  ctx.SetFlag("weapon_loaded", true);
  EXPECT_TRUE(ctx.GetFlag("weapon_loaded", false));
  ctx.SetFlag("weapon_loaded", false);
  EXPECT_FALSE(ctx.GetFlag("weapon_loaded", true));
}

TEST(EntityContext, IntStorage) {
  EntityContextImpl ctx(1);
  EXPECT_EQ(ctx.GetInt("ammo", -1), -1);
  ctx.SetInt("ammo", 30);
  EXPECT_EQ(ctx.GetInt("ammo", 0), 30);
}

TEST(EntityContext, FloatStorage) {
  EntityContextImpl ctx(1);
  EXPECT_DOUBLE_EQ(ctx.GetFloat("health", 100.0), 100.0);
  ctx.SetFloat("health", 75.5);
  EXPECT_DOUBLE_EQ(ctx.GetFloat("health", 0.0), 75.5);
}

TEST(EntityContext, LastTickTime) {
  EntityContextImpl ctx(1);
  ctx.UpdateLastTickTime(1234);
  EXPECT_EQ(ctx.LastTickTime(), 1234u);
}

}  // namespace sim_bt
