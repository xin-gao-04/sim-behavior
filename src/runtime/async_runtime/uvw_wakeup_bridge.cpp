#include "uvw_wakeup_bridge.hpp"

namespace sim_bt {

UvwWakeupBridge::UvwWakeupBridge(std::shared_ptr<uvw::loop> loop) {
  handle_ = loop->resource<uvw::async_handle>();
  handle_->on<uvw::async_event>([this](const auto&, auto&) {
    if (on_wakeup_) on_wakeup_();
  });
}

UvwWakeupBridge::~UvwWakeupBridge() {
  Close();
}

void UvwWakeupBridge::Signal() {
  if (handle_) {
    handle_->send();
  }
}

void UvwWakeupBridge::SetOnWakeup(std::function<void()> callback) {
  on_wakeup_ = std::move(callback);
}

void UvwWakeupBridge::Close() {
  if (handle_) {
    handle_->close();
    handle_.reset();
  }
}

}  // namespace sim_bt
