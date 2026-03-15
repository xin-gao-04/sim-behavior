#include <gtest/gtest.h>
#include <thread>
#include <vector>

// Include the concrete implementation directly for unit testing
#include "../src/runtime/compute_runtime/default_result_mailbox.hpp"

namespace sim_bt {

TEST(ResultMailbox, PeekBeforePost) {
  DefaultResultMailbox mailbox;
  EXPECT_FALSE(mailbox.Peek(42).has_value());
}

TEST(ResultMailbox, PostAndDrainMakesResultAvailable) {
  DefaultResultMailbox mailbox;

  JobResult r;
  r.job_id    = 1;
  r.succeeded = true;

  mailbox.Post(r);

  // DrainAll 将 incoming_ 移到 ready_
  int count = 0;
  mailbox.DrainAll([&](const JobResult& result) {
    EXPECT_EQ(result.job_id, 1u);
    EXPECT_TRUE(result.succeeded);
    ++count;
  });
  EXPECT_EQ(count, 1);

  auto peeked = mailbox.Peek(1);
  ASSERT_TRUE(peeked.has_value());
  EXPECT_TRUE(peeked->succeeded);
}

TEST(ResultMailbox, ConsumeRemovesResult) {
  DefaultResultMailbox mailbox;
  JobResult r;
  r.job_id    = 7;
  r.succeeded = true;
  mailbox.Post(r);
  mailbox.DrainAll([](const JobResult&) {});

  ASSERT_TRUE(mailbox.Peek(7).has_value());
  mailbox.Consume(7);
  EXPECT_FALSE(mailbox.Peek(7).has_value());
}

TEST(ResultMailbox, MultipleJobsIndependent) {
  DefaultResultMailbox mailbox;
  for (uint64_t i = 1; i <= 5; ++i) {
    JobResult r;
    r.job_id    = i;
    r.succeeded = (i % 2 == 0);
    mailbox.Post(r);
  }
  mailbox.DrainAll([](const JobResult&) {});

  for (uint64_t i = 1; i <= 5; ++i) {
    auto peeked = mailbox.Peek(i);
    ASSERT_TRUE(peeked.has_value()) << "job_id=" << i;
    EXPECT_EQ(peeked->succeeded, (i % 2 == 0));
  }
}

TEST(ResultMailbox, NotifyCallbackFired) {
  DefaultResultMailbox mailbox;
  int notify_count = 0;
  mailbox.SetNotifyCallback([&]() { ++notify_count; });

  JobResult r;
  r.job_id = 99;
  mailbox.Post(r);

  EXPECT_EQ(notify_count, 1);
}

}  // namespace sim_bt
