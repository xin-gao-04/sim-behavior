#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include <uvw.hpp>

#include "sim_bt/runtime/async_runtime/i_event_loop_runtime.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// UvwEventLoopRuntime
//
// IEventLoopRuntime 的 uvw/libuv 实现。
//
// 内部结构：
//   - 一个 uvw::loop 在独立线程中运行（loop_thread_）。
//   - 一个 uvw::async_handle 用于跨线程提交 PostToLoop 回调（post_async_）。
//   - 一个 uvw::async_handle 用于停止 loop（stop_async_）。
//   - PostToLoop 将 callback 写入 post_queue_ 后触发 post_async_，
//     在 loop 线程上批量执行。
// ─────────────────────────────────────────────────────────────────────────────
class UvwEventLoopRuntime : public IEventLoopRuntime {
 public:
  UvwEventLoopRuntime();
  ~UvwEventLoopRuntime() override;

  // ── IEventLoopRuntime ────────────────────────────────────────────────────────

  SimStatus Start() override;
  void Stop() override;
  bool IsRunning() const override;

  void WakeUp(VoidCallback callback) override;

  TimerHandlePtr StartOneShotTimer(std::chrono::milliseconds delay,
                                   VoidCallback on_timeout) override;

  TimerHandlePtr StartRepeatingTimer(std::chrono::milliseconds interval,
                                     VoidCallback on_tick) override;

  void PostToLoop(VoidCallback callback) override;

  // ── 扩展接口（供 TbbJobExecutor 注入 wakeup callback） ─────────────────────

  // 获取 loop 对象，供 bus adapter 在 loop 线程初始化 handle。
  // 注意：只在 loop 线程内安全使用 loop 的 handle API。
  uvw::loop& Loop() { return *loop_; }

 private:
  void LoopThreadBody();
  void DrainPostQueue();

  std::shared_ptr<uvw::loop> loop_;
  std::shared_ptr<uvw::async_handle> post_async_;
  std::shared_ptr<uvw::async_handle> stop_async_;

  std::thread loop_thread_;
  std::atomic<bool> running_{false};

  // Post 任务队列
  mutable std::mutex post_mu_;
  std::queue<VoidCallback> post_queue_;
};

}  // namespace sim_bt
