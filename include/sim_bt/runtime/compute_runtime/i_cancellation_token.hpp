#pragma once

#include <atomic>
#include <memory>

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// ICancellationToken
//
// 合作式取消令牌。
//
// TBB 任务实现者在热路径中轮询 IsCancelled()，在收到取消信号后
// 安全地提前结束计算，并不得修改任何共享状态。
//
// 取消语义是合作式的（cooperative），不是强制打断（preemptive）。
// BehaviorTree.CPP 的 halt() 也遵循相同假定。
// ─────────────────────────────────────────────────────────────────────────────
class ICancellationToken {
 public:
  virtual ~ICancellationToken() = default;

  // 任务代码在热路径中调用此方法检查是否需要提前退出。
  virtual bool IsCancelled() const = 0;

  // 由 BT halt() 或超时回调触发，线程安全。
  virtual void Cancel() = 0;

  // 重置状态（供对象池复用）。
  virtual void Reset() = 0;
};

using CancellationTokenPtr = std::shared_ptr<ICancellationToken>;

// ─────────────────────────────────────────────────────────────────────────────
// DefaultCancellationToken
//
// 基于原子标志的标准实现，无需依赖任何具体库。
// ─────────────────────────────────────────────────────────────────────────────
class DefaultCancellationToken : public ICancellationToken {
 public:
  bool IsCancelled() const override {
    return cancelled_.load(std::memory_order_acquire);
  }

  void Cancel() override {
    cancelled_.store(true, std::memory_order_release);
  }

  void Reset() override {
    cancelled_.store(false, std::memory_order_relaxed);
  }

 private:
  std::atomic<bool> cancelled_{false};
};

}  // namespace sim_bt
