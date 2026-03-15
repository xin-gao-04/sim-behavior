#pragma once

#include <functional>
#include <memory>

#include <uvw.hpp>

#include "sim_bt/runtime/async_runtime/i_wakeup_bridge.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// UvwWakeupBridge
//
// IWakeupBridge 的 uvw async_handle 实现。
//
// Signal() 调用 uvw async_handle 的 send()，可在任意线程调用。
// 多次并发调用可能被 libuv 合并为一次回调——这是预期行为。
// ─────────────────────────────────────────────────────────────────────────────
class UvwWakeupBridge : public IWakeupBridge {
 public:
  // loop 必须在 Start() 前提供（由 UvwEventLoopRuntime 在 loop 线程初始化后注入）
  explicit UvwWakeupBridge(std::shared_ptr<uvw::loop> loop);
  ~UvwWakeupBridge() override;

  void Signal() override;
  void SetOnWakeup(std::function<void()> callback) override;

  // 在 loop 关闭前调用（在 loop 线程上）
  void Close();

 private:
  std::shared_ptr<uvw::async_handle> handle_;
  std::function<void()>              on_wakeup_;
};

}  // namespace sim_bt
