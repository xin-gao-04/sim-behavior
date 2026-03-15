#include <gtest/gtest.h>
#include "sim_bt/runtime/compute_runtime/i_cancellation_token.hpp"

namespace sim_bt {

TEST(CancellationToken, InitiallyNotCancelled) {
  DefaultCancellationToken token;
  EXPECT_FALSE(token.IsCancelled());
}

TEST(CancellationToken, CancelSetsFlag) {
  DefaultCancellationToken token;
  token.Cancel();
  EXPECT_TRUE(token.IsCancelled());
}

TEST(CancellationToken, ResetClearsFlag) {
  DefaultCancellationToken token;
  token.Cancel();
  ASSERT_TRUE(token.IsCancelled());
  token.Reset();
  EXPECT_FALSE(token.IsCancelled());
}

TEST(CancellationToken, MultipleCancel) {
  DefaultCancellationToken token;
  token.Cancel();
  token.Cancel();  // 幂等
  EXPECT_TRUE(token.IsCancelled());
}

}  // namespace sim_bt
