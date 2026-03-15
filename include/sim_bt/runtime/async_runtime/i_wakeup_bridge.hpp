#pragma once

#include <functional>
#include <memory>

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// IWakeupBridge
//
// 跨线程唤醒桥。
//
// 当 TBB worker 计算完毕或外部事件到达时，需要通知 BT Tick Domain
// 在下一帧或尽快重新 tick 相关树。WakeupBridge 作为两个执行域之间
// 的安全通知通道。
//
// 实现方式：
//   - 通常封装一个 uvw async_handle。
//   - TBB worker / bus adapter 调用 Signal() → uvw async_handle.send()
//   - loop 回调触发后，再通知 BtRuntime 调度 re-tick。
//
// 约束：
//   - Signal() 必须是线程安全的，可在任意线程调用。
//   - 多次并发 Signal() 可能被 libuv 合并为一次回调（这是预期行为）。
// ─────────────────────────────────────────────────────────────────────────────
class IWakeupBridge {
 public:
  virtual ~IWakeupBridge() = default;

  // 从任意线程发出唤醒信号。线程安全。
  virtual void Signal() = 0;

  // 设置唤醒回调（在 loop 线程上执行）。应在 Start() 前设置。
  virtual void SetOnWakeup(std::function<void()> callback) = 0;
};

using WakeupBridgePtr = std::shared_ptr<IWakeupBridge>;

}  // namespace sim_bt
