#include "default_result_mailbox.hpp"

#include <functional>
#include <vector>

#include "sim_bt/common/sim_bt_log.hpp"

namespace sim_bt {

void DefaultResultMailbox::Post(JobResult result) {
  SIMBT_LOG_INFO_S("ResultMailbox: post job_id=" << result.job_id
      << " owner=" << result.owner_entity
      << " succeeded=" << result.succeeded);
  VoidCallback cb;
  {
    std::lock_guard<std::mutex> lock(mu_);
    incoming_.push(std::move(result));
    cb = notify_cb_;  // 取出回调引用，在锁外调用
  }
  if (cb) {
    cb();  // 触发 uvw async_handle.send()，不在锁内执行
  }
}

// 方案 D — DrainAll 批量化：N+1 次加锁 → 2 次加锁
//
// 流程：
//   Step 1: { lock } swap incoming_ → local（1 次锁，最小化 incoming_ 持锁时间）
//   Step 2: 锁外遍历 local，过滤 discarded_ids_，收集有效 batch
//   Step 3: { lock } batch insert 到 ready_（1 次锁，N 次 insert）
//   Step 4: 锁外执行 consumer 回调（通常触发 RequestWakeup）
//
// 总计：2 次加锁，替代原有 N+1 次。
void DefaultResultMailbox::DrainAll(
    const std::function<void(JobResult)>& consumer) {
  // Step 1：原子交换，最小化 incoming_ 持锁时间
  std::queue<JobResult> local;
  {
    std::lock_guard<std::mutex> lock(mu_);
    std::swap(local, incoming_);
  }
  if (local.empty()) return;

  SIMBT_LOG_INFO_S("ResultMailbox: drain " << local.size() << " result(s)");

  // Step 2：锁外遍历，收集有效结果（跳过已 Discard 的 job_id）
  std::vector<JobResult> batch;
  batch.reserve(local.size());
  {
    std::lock_guard<std::mutex> lock(mu_);
    while (!local.empty()) {
      JobResult r = std::move(local.front());
      local.pop();
      if (discarded_ids_.count(r.job_id)) continue;  // 幽灵结果：跳过
      batch.push_back(std::move(r));
    }
    // Step 3：一次加锁，批量写入 ready_
    for (const auto& r : batch) {
      ready_[r.job_id] = r;
    }
  }

  // Step 4：锁外执行 consumer 回调
  if (consumer) {
    for (const auto& r : batch) {
      consumer(r);
    }
  }
}

std::optional<JobResult> DefaultResultMailbox::Peek(uint64_t job_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = ready_.find(job_id);
  if (it == ready_.end()) return std::nullopt;
  return it->second;
}

void DefaultResultMailbox::Consume(uint64_t job_id) {
  std::lock_guard<std::mutex> lock(mu_);
  ready_.erase(job_id);
  discarded_ids_.erase(job_id);  // 同步清理 discard 集合
}

// Discard 精确取消：同时标记 discarded_ids_，防止幽灵结果
// 若 job 已在 incoming_ 中但尚未 DrainAll，DrainAll 时会跳过该 job_id。
void DefaultResultMailbox::Discard(uint64_t job_id) {
  std::lock_guard<std::mutex> lock(mu_);
  ready_.erase(job_id);
  discarded_ids_.insert(job_id);  // 标记：即使从 incoming_ drain 出来也不写入 ready_
}

}  // namespace sim_bt
