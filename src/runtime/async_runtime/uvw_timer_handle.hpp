#pragma once

#include <atomic>
#include <memory>

#include <uvw.hpp>

#include "sim_bt/runtime/async_runtime/i_event_loop_runtime.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// UvwTimerHandle
//
// ITimerHandle 的 uvw 实现。
//
// 生命周期说明：
//   - UvwTimerHandle 由调用方（BT 节点）持有（shared_ptr）。
//   - 内部持有 uvw::timer_handle 的弱引用，在 loop 线程上创建和销毁。
//   - Cancel() 通过 PostToLoop 在 loop 线程上关闭 timer handle，
//     避免跨线程操作 uvw handle。
// ─────────────────────────────────────────────────────────────────────────────
class UvwTimerHandle : public ITimerHandle {
 public:
  UvwTimerHandle() = default;
  ~UvwTimerHandle() override = default;

  // 在 loop 线程上调用，绑定底层 uvw timer handle。
  void Attach(std::shared_ptr<uvw::timer_handle> handle) {
    handle_ = std::move(handle);
    active_.store(true, std::memory_order_release);
  }

  // 在 loop 线程上调用，解除绑定（timer 已触发或已取消）。
  void Detach() {
    handle_.reset();
    active_.store(false, std::memory_order_release);
  }

  void Cancel() override {
    if (!active_.load(std::memory_order_acquire)) return;
    // uvw handle 操作必须在 loop 线程上执行。
    // 此处直接关闭（调用方须保证在 loop 线程上调用 Cancel()）。
    if (handle_) {
      handle_->stop();
      handle_->close();
      handle_.reset();
    }
    active_.store(false, std::memory_order_release);
  }

  bool IsActive() const override {
    return active_.load(std::memory_order_acquire);
  }

 private:
  std::shared_ptr<uvw::timer_handle> handle_;
  std::atomic<bool>                  active_{false};
};

}  // namespace sim_bt
