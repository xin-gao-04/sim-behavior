#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "sim_bt/common/result.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// TimerHandle
//
// 对 uvw timer_handle 的不透明句柄，调用者持有以取消定时器。
// ─────────────────────────────────────────────────────────────────────────────
class ITimerHandle {
 public:
  virtual ~ITimerHandle() = default;

  // 取消定时器。回调不会再被触发，幂等。
  virtual void Cancel() = 0;

  // 定时器是否仍然活跃。
  virtual bool IsActive() const = 0;
};

using TimerHandlePtr = std::shared_ptr<ITimerHandle>;

// ─────────────────────────────────────────────────────────────────────────────
// IEventLoopRuntime
//
// 对 uvw (libuv C++ wrapper) 事件循环的封装层。
//
// 职责：
//   - 管理主 uvw loop 的生命周期（Start / Stop）。
//   - 提供跨线程安全唤醒（WakeUp），对应 uv_async_t 语义。
//   - 提供一次性和重复定时器（StartTimer）。
//   - 提供在 loop 线程上投递回调（PostToLoop）。
//
// 线程模型：
//   - uvw loop 在独立线程（EventLoop Thread）上运行。
//   - WakeUp() 和 PostToLoop() 可从任意线程调用。
//   - 所有回调均在 EventLoop Thread 上串行执行，无需额外加锁。
//
// 此接口不暴露 uvw 或 libuv 的原始类型，确保调用方可在测试中替换
// 为 mock 实现。
// ─────────────────────────────────────────────────────────────────────────────
class IEventLoopRuntime {
 public:
  virtual ~IEventLoopRuntime() = default;

  // ── 生命周期 ────────────────────────────────────────────────────────────────

  // 启动 event loop（在新线程中）。幂等。
  virtual SimStatus Start() = 0;

  // 优雅停止 event loop，等待 loop 线程退出。幂等。
  virtual void Stop() = 0;

  // event loop 是否正在运行。
  virtual bool IsRunning() const = 0;

  // ── 跨线程唤醒 ──────────────────────────────────────────────────────────────

  // 从任意线程安全唤醒 loop。对应 uv_async_send()。
  // callback 在 loop 线程上被调用一次（多次并发调用可能合并）。
  virtual void WakeUp(VoidCallback callback) = 0;

  // ── 定时器 ──────────────────────────────────────────────────────────────────

  // 创建一次性定时器，delay 后触发 on_timeout，然后自动销毁句柄。
  // 返回句柄可在超时前调用 Cancel() 取消。
  virtual TimerHandlePtr StartOneShotTimer(
      std::chrono::milliseconds delay,
      VoidCallback on_timeout) = 0;

  // 创建重复定时器，每 interval 触发一次 on_tick。
  // 返回句柄调用 Cancel() 停止。
  virtual TimerHandlePtr StartRepeatingTimer(
      std::chrono::milliseconds interval,
      VoidCallback on_tick) = 0;

  // ── loop 线程任务投递 ───────────────────────────────────────────────────────

  // 从任意线程将 callback 安排在 loop 线程上执行（队列语义，不合并）。
  // 底层通过 async_handle + 内部任务队列实现。
  virtual void PostToLoop(VoidCallback callback) = 0;
};

using EventLoopRuntimePtr = std::shared_ptr<IEventLoopRuntime>;

}  // namespace sim_bt
