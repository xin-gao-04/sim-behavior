#pragma once

#include <mutex>
#include <optional>
#include <queue>
#include <unordered_map>

#include "sim_bt/common/types.hpp"
#include "sim_bt/runtime/compute_runtime/i_result_mailbox.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// DefaultResultMailbox
//
// IResultMailbox 的标准线程安全实现。
//
// Post() 使用 mutex 保护写入，DrainAll() 在 loop 线程上串行消费。
// Peek() / Consume() / Discard() 在 BT Tick Domain 调用，与 Post()
// 可能并发，因此也受同一 mutex 保护。
// ─────────────────────────────────────────────────────────────────────────────
class DefaultResultMailbox : public IResultMailbox {
 public:
  DefaultResultMailbox() = default;
  ~DefaultResultMailbox() override = default;

  // 设置 "结果到达" 通知钩子（由 TbbJobExecutor 在构造时注入）。
  // 钩子在 Post() 持锁结束后在调用方线程上调用（不在锁内），
  // 通常用于触发 uvw async_handle.send()。
  void SetNotifyCallback(VoidCallback cb) { notify_cb_ = std::move(cb); }

  // ── IResultMailbox ──────────────────────────────────────────────────────────

  void Post(JobResult result) override;
  void DrainAll(const std::function<void(JobResult)>& consumer) override;
  std::optional<JobResult> Peek(uint64_t job_id) const override;
  void Consume(uint64_t job_id) override;
  void Discard(uint64_t job_id) override;

 private:
  mutable std::mutex mu_;

  // 待消费队列（按到达顺序，供 DrainAll 使用）
  std::queue<JobResult> incoming_;

  // 已 drain 但尚未被 BT 节点 Consume 的结果（按 job_id 检索）
  std::unordered_map<uint64_t, JobResult> ready_;

  VoidCallback notify_cb_;
};

}  // namespace sim_bt
