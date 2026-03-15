#include "uvw_event_loop_runtime.hpp"

#include "uvw_timer_handle.hpp"

namespace sim_bt {

UvwEventLoopRuntime::UvwEventLoopRuntime()
    : loop_(uvw::loop::create()) {}

UvwEventLoopRuntime::~UvwEventLoopRuntime() {
  Stop();
}

SimStatus UvwEventLoopRuntime::Start() {
  if (running_.exchange(true)) {
    return SimStatus::Ok();  // 已在运行，幂等
  }

  // stop async handle：用于安全地从外部停止 loop
  stop_async_ = loop_->resource<uvw::async_handle>();
  stop_async_->on<uvw::async_event>([this](const auto&, auto& handle) {
    handle.close();
    if (post_async_) post_async_->close();
    loop_->stop();
  });

  // post async handle：用于从外部线程向 loop 投递回调
  post_async_ = loop_->resource<uvw::async_handle>();
  post_async_->on<uvw::async_event>([this](const auto&, auto&) {
    DrainPostQueue();
  });

  loop_thread_ = std::thread(&UvwEventLoopRuntime::LoopThreadBody, this);
  return SimStatus::Ok();
}

void UvwEventLoopRuntime::Stop() {
  if (!running_.exchange(false)) {
    return;  // 未在运行，幂等
  }
  if (stop_async_) {
    stop_async_->send();
  }
  if (loop_thread_.joinable()) {
    loop_thread_.join();
  }
}

bool UvwEventLoopRuntime::IsRunning() const {
  return running_.load(std::memory_order_acquire);
}

void UvwEventLoopRuntime::WakeUp(VoidCallback callback) {
  PostToLoop(std::move(callback));
}

TimerHandlePtr UvwEventLoopRuntime::StartOneShotTimer(
    std::chrono::milliseconds delay, VoidCallback on_timeout) {
  auto timer_ref = std::make_shared<UvwTimerHandle>();
  PostToLoop([this, delay, on_timeout = std::move(on_timeout),
              weak_ref = std::weak_ptr<UvwTimerHandle>(timer_ref)]() mutable {
    auto ref = weak_ref.lock();
    if (!ref) return;
    auto handle = loop_->resource<uvw::timer_handle>();
    ref->Attach(handle);
    handle->on<uvw::timer_event>([on_timeout, weak_ref2 = std::weak_ptr<UvwTimerHandle>(ref)](
                                      const auto&, auto& h) mutable {
      h.close();
      if (auto r = weak_ref2.lock()) r->Detach();
      if (on_timeout) on_timeout();
    });
    handle->start(delay, std::chrono::milliseconds{0});
  });
  return timer_ref;
}

TimerHandlePtr UvwEventLoopRuntime::StartRepeatingTimer(
    std::chrono::milliseconds interval, VoidCallback on_tick) {
  auto timer_ref = std::make_shared<UvwTimerHandle>();
  PostToLoop([this, interval, on_tick = std::move(on_tick),
              weak_ref = std::weak_ptr<UvwTimerHandle>(timer_ref)]() mutable {
    auto ref = weak_ref.lock();
    if (!ref) return;
    auto handle = loop_->resource<uvw::timer_handle>();
    ref->Attach(handle);
    handle->on<uvw::timer_event>([on_tick](const auto&, auto&) {
      if (on_tick) on_tick();
    });
    handle->start(interval, interval);
  });
  return timer_ref;
}

void UvwEventLoopRuntime::PostToLoop(VoidCallback callback) {
  {
    std::lock_guard<std::mutex> lock(post_mu_);
    post_queue_.push(std::move(callback));
  }
  if (post_async_) {
    post_async_->send();
  }
}

void UvwEventLoopRuntime::LoopThreadBody() {
  loop_->run();
  running_.store(false, std::memory_order_release);
}

void UvwEventLoopRuntime::DrainPostQueue() {
  std::queue<VoidCallback> local;
  {
    std::lock_guard<std::mutex> lock(post_mu_);
    std::swap(local, post_queue_);
  }
  while (!local.empty()) {
    VoidCallback cb = std::move(local.front());
    local.pop();
    if (cb) cb();
  }
}

}  // namespace sim_bt
