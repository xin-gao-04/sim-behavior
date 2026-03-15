// test_cross_library_integration.cpp
//
// 跨库交互边界最小验证套件
//
// 覆盖的关键链路：
//   TbbJobExecutor::Submit
//       → DefaultResultMailbox::Post  (worker 线程)
//       → notify_cb_()                (worker 线程)
//       → UvwEventLoopRuntime::PostToLoop  (loop 线程调度)
//       → DefaultResultMailbox::DrainAll   (loop 线程消费)
//
// 这条链路是三方库之间最薄弱的交互边界，单元测试无法覆盖，
// 集成测试可以在不依赖 BT 层的前提下独立验证。
//
// 注意：测试使用真实的 TbbJobExecutor 和 UvwEventLoopRuntime，
//       不使用 mock，目的是验证真实的跨线程通信是否正确运转。

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "../src/runtime/async_runtime/uvw_event_loop_runtime.hpp"
#include "../src/runtime/compute_runtime/default_result_mailbox.hpp"
#include "../src/runtime/compute_runtime/tbb_job_executor.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// 辅助函数：等待原子标志变为 true，超时返回 false
// ─────────────────────────────────────────────────────────────────────────────
static bool WaitFor(const std::atomic<bool>& flag,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!flag.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Fixture：启动 event loop，在每个测试后清理
// ─────────────────────────────────────────────────────────────────────────────
class CrossLibIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    event_loop_ = std::make_shared<UvwEventLoopRuntime>();
    ASSERT_TRUE(static_cast<bool>(event_loop_->Start()))
        << "UvwEventLoopRuntime failed to start";

    executor_ = std::make_unique<TbbJobExecutor>();
  }

  void TearDown() override {
    executor_->Shutdown();
    event_loop_->Stop();
  }

  // 将 executor 的 wakeup 回调绑定到 event_loop 的 PostToLoop，
  // 并在 loop 线程执行 DrainAll，将每条结果传递给 on_result。
  void WireWakeup(std::function<void(const JobResult&)> on_result) {
    auto& mailbox   = executor_->Mailbox();
    auto  loop_weak = std::weak_ptr<UvwEventLoopRuntime>(event_loop_);

    executor_->SetWakeupCallback([loop_weak, &mailbox, on_result]() {
      auto loop = loop_weak.lock();
      if (!loop) return;
      loop->PostToLoop([&mailbox, on_result]() {
        mailbox.DrainAll([&on_result](const JobResult& r) {
          on_result(r);
        });
      });
    });
  }

  std::shared_ptr<UvwEventLoopRuntime> event_loop_;
  std::unique_ptr<TbbJobExecutor>      executor_;
};

// ─────────────────────────────────────────────────────────────────────────────
// 测试 1：提交成功任务 → 结果经 mailbox + loop 正确到达
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(CrossLibIntegrationTest, SuccessfulJobResultDelivered) {
  std::atomic<bool> result_received{false};
  std::atomic<bool> result_succeeded{false};
  std::atomic<uint64_t> received_job_id{0};

  WireWakeup([&](const JobResult& r) {
    received_job_id.store(r.job_id, std::memory_order_release);
    result_succeeded.store(r.succeeded, std::memory_order_release);
    result_received.store(true, std::memory_order_release);
  });

  JobDescriptor desc;
  desc.priority = JobPriority::kNormal;
  desc.task = [](CancellationTokenPtr, JobResult& out) {
    out.succeeded = true;
  };

  auto handle = executor_->Submit(std::move(desc));
  ASSERT_NE(handle, nullptr);
  uint64_t expected_job_id = handle->JobId();

  ASSERT_TRUE(WaitFor(result_received)) << "结果未在 2s 内到达 loop 线程";
  EXPECT_TRUE(result_succeeded.load());
  EXPECT_EQ(received_job_id.load(), expected_job_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 2：提交失败任务 → succeeded=false 和 error_message 正确传递
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(CrossLibIntegrationTest, FailedJobResultDelivered) {
  std::atomic<bool> result_received{false};
  std::atomic<bool> result_succeeded{true};  // 故意初始化为 true，验证被覆盖
  std::string received_error;

  WireWakeup([&](const JobResult& r) {
    result_succeeded.store(r.succeeded, std::memory_order_release);
    received_error = r.error_message;
    result_received.store(true, std::memory_order_release);
  });

  JobDescriptor desc;
  desc.task = [](CancellationTokenPtr, JobResult& out) {
    out.succeeded      = false;
    out.error_message  = "compute_failed";
  };

  executor_->Submit(std::move(desc));

  ASSERT_TRUE(WaitFor(result_received));
  EXPECT_FALSE(result_succeeded.load());
  EXPECT_EQ(received_error, "compute_failed");
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 3：多任务并发提交 → 所有结果均到达，job_id 无重复
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(CrossLibIntegrationTest, MultipleJobsAllDelivered) {
  constexpr int kJobCount = 10;

  std::mutex mu;
  std::vector<uint64_t> received_ids;

  WireWakeup([&](const JobResult& r) {
    std::lock_guard<std::mutex> lk(mu);
    received_ids.push_back(r.job_id);
  });

  std::vector<uint64_t> submitted_ids;
  for (int i = 0; i < kJobCount; ++i) {
    JobDescriptor desc;
    desc.priority = (i % 2 == 0) ? JobPriority::kNormal : JobPriority::kLow;
    desc.task = [](CancellationTokenPtr, JobResult& out) {
      out.succeeded = true;
    };
    auto handle = executor_->Submit(std::move(desc));
    ASSERT_NE(handle, nullptr);
    submitted_ids.push_back(handle->JobId());
  }

  // 等待所有结果到达
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (true) {
    {
      std::lock_guard<std::mutex> lk(mu);
      if (static_cast<int>(received_ids.size()) >= kJobCount) break;
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::lock_guard<std::mutex> lk(mu);
  ASSERT_EQ(static_cast<int>(received_ids.size()), kJobCount)
      << "期望 " << kJobCount << " 个结果，实际收到 " << received_ids.size();

  // job_id 无重复
  std::sort(received_ids.begin(), received_ids.end());
  EXPECT_EQ(std::unique(received_ids.begin(), received_ids.end()),
            received_ids.end())
      << "存在重复 job_id";
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 4：取消边界 —— 任务取消后 token 标志被设置，不留脏状态
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(CrossLibIntegrationTest, CancelledJobTokenIsSet) {
  std::atomic<bool> token_was_cancelled{false};
  std::atomic<bool> task_started{false};
  std::atomic<bool> task_exited{false};

  // 不接线 wakeup（取消场景下可能无结果到达）

  JobDescriptor desc;
  desc.task = [&](CancellationTokenPtr token, JobResult& out) {
    task_started.store(true, std::memory_order_release);
    // 模拟短暂工作，之后检查取消
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    token_was_cancelled.store(token->IsCancelled(), std::memory_order_release);
    out.succeeded = !token->IsCancelled();
    task_exited.store(true, std::memory_order_release);
  };

  auto handle = executor_->Submit(std::move(desc));
  ASSERT_NE(handle, nullptr);

  // 等待任务启动再取消
  ASSERT_TRUE(WaitFor(task_started)) << "任务未在 2s 内启动";
  handle->Cancel();

  // 等待任务退出
  ASSERT_TRUE(WaitFor(task_exited)) << "任务未在 2s 内退出";
  EXPECT_TRUE(token_was_cancelled.load())
      << "取消后 token->IsCancelled() 应为 true";
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试 5：PostToLoop 确保回调在 loop 线程（非调用方线程）执行
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(CrossLibIntegrationTest, PostToLoopRunsOnLoopThread) {
  std::atomic<bool> callback_ran{false};
  std::thread::id   caller_tid  = std::this_thread::get_id();
  std::thread::id   callback_tid;

  event_loop_->PostToLoop([&]() {
    callback_tid = std::this_thread::get_id();
    callback_ran.store(true, std::memory_order_release);
  });

  ASSERT_TRUE(WaitFor(callback_ran)) << "PostToLoop 回调未执行";
  EXPECT_NE(callback_tid, caller_tid)
      << "PostToLoop 回调应在 loop 线程执行，而非调用方线程";
}

}  // namespace sim_bt
