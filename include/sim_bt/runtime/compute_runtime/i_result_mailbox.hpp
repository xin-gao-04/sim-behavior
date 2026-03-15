#pragma once

#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// JobResult
//
// TBB worker 完成后写入邮箱的结果描述符。
// payload 由调用者负责解释（type-erased），避免邮箱依赖具体业务类型。
// ─────────────────────────────────────────────────────────────────────────────
struct JobResult {
  uint64_t  job_id    = 0;
  bool      succeeded = false;
  std::any  payload;           // 业务结果，由节点负责 any_cast
  std::string error_message;  // succeeded == false 时的错误描述
};

// ─────────────────────────────────────────────────────────────────────────────
// IResultMailbox
//
// 线程安全的结果邮箱。
//
// 数据流：
//   TBB worker 完成后调用 Post()（在 worker 线程）。
//   uvw async_handle 触发后，loop 线程调用 DrainAll() 批量消费。
//   BT 节点在 onRunning() 中通过 Peek(job_id) 检查自己的结果。
//
// 设计约束：
//   - Post() 必须无锁或使用极短临界区（在 worker 线程热路径上）。
//   - DrainAll() 在 uvw loop 线程上串行调用，无需关心并发。
// ─────────────────────────────────────────────────────────────────────────────
class IResultMailbox {
 public:
  virtual ~IResultMailbox() = default;

  // 由 TBB worker 线程调用：投递结果并触发 uvw wakeup。
  virtual void Post(JobResult result) = 0;

  // 由 uvw loop 线程调用：消费所有积压结果，逐条回调 consumer。
  // consumer 在 loop 线程上同步调用，不会并发。
  virtual void DrainAll(const std::function<void(JobResult)>& consumer) = 0;

  // 由 BT 节点在 onRunning() 中查询特定任务的结果（非消费，保留结果）。
  // 如果结果尚未就绪，返回 std::nullopt。
  virtual std::optional<JobResult> Peek(uint64_t job_id) const = 0;

  // 由 BT 节点在结果已消费后调用，清除对应条目，释放内存。
  virtual void Consume(uint64_t job_id) = 0;

  // 取消某个任务对应的 pending/completed 结果。
  virtual void Discard(uint64_t job_id) = 0;
};

using ResultMailboxPtr = std::shared_ptr<IResultMailbox>;

}  // namespace sim_bt
