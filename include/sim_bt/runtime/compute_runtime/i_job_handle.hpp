#pragma once

#include <memory>

#include "sim_bt/runtime/compute_runtime/i_cancellation_token.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// JobState
//
// 提交给 oneTBB arena 的任务生命周期状态。
// ─────────────────────────────────────────────────────────────────────────────
enum class JobState : uint8_t {
  kPending   = 0,  // 已提交，等待调度
  kRunning   = 1,  // 正在 worker 线程上执行
  kCompleted = 2,  // 执行完成（结果已写入 ResultMailbox）
  kCancelled = 3,  // 已取消（令牌触发后 worker 提前退出）
  kFailed    = 4,  // 执行中抛出异常或发生内部错误
};

// ─────────────────────────────────────────────────────────────────────────────
// IJobHandle
//
// 对已提交 TBB 任务的轻量引用句柄。
//
// BT 节点在 onStart() 中通过 IJobExecutor::Submit() 获得此句柄，
// 在 onRunning() 中查询 State()，在 onHalted() 中调用 Cancel()。
//
// 注意：句柄本身不持有任务结果；结果由 IResultMailbox 持有。
// ─────────────────────────────────────────────────────────────────────────────
class IJobHandle {
 public:
  virtual ~IJobHandle() = default;

  // 当前任务状态，线程安全（原子读）。
  virtual JobState State() const = 0;

  // 请求取消。若任务已完成则无效，不阻塞。
  virtual void Cancel() = 0;

  // 任务唯一标识符（用于与 ResultMailbox 匹配）。
  virtual uint64_t JobId() const = 0;

  // 获取绑定的取消令牌（由 worker 轮询）。
  virtual CancellationTokenPtr Token() const = 0;
};

using JobHandlePtr = std::shared_ptr<IJobHandle>;

}  // namespace sim_bt
