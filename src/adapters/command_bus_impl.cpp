#include "sim_bt/adapters/i_command_bus.hpp"
#include "sim_bt/runtime/async_runtime/i_event_loop_runtime.hpp"
#include "sim_bt/runtime/async_runtime/i_bus_adapter.hpp"

#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// InProcessCommandBus
//
// 进程内同步命令总线，用于开发调试版（单进程部署）。
//
// Phase 3 扩展：可通过 SetBusAdapter() 挂接 IBusAdapter。
//   挂接后，每次 Dispatch() 在本地路由完成后，还会将命令序列化为
//   BusMessage，通过 PostToLoop 在 uvw loop 线程上调用
//   IBusAdapter::Publish()，实现出站转发。
//
// ActionCommand → BusMessage 编码：
//   topic   = command.command_type
//   payload = [4B source_entity LE][8B issued_at_ms LE][原始 payload...]
// ─────────────────────────────────────────────────────────────────────────────
class InProcessCommandBus : public ICommandBus {
 public:
  SimStatus Dispatch(ActionCommand command) override {
    // 1. 本地路由（同步）
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = handlers_.find(command.command_type);
      if (it != handlers_.end()) {
        for (auto& h : it->second) h(command);
      }
    }

    // 2. 出站转发（如果已挂接 BusAdapter）
    ForwardToAdapter(command);

    return SimStatus::Ok();
  }

  void RegisterHandler(const std::string& command_type,
                        CommandHandler handler) override {
    std::lock_guard<std::mutex> lock(mu_);
    handlers_[command_type].push_back(std::move(handler));
  }

  void ClearHandlers() override {
    std::lock_guard<std::mutex> lock(mu_);
    handlers_.clear();
  }

  // ── Phase 3 出站适配器 ────────────────────────────────────────────────────

  // 挂接 BusAdapter + EventLoop。两者必须同时设置，否则不转发。
  // 线程安全：可在任意线程调用，通常在 Initialize() 阶段设置一次。
  void SetBusAdapter(std::shared_ptr<IBusAdapter>       adapter,
                     std::shared_ptr<IEventLoopRuntime>  event_loop) {
    std::lock_guard<std::mutex> lk(adapter_mu_);
    adapter_    = std::move(adapter);
    event_loop_ = std::move(event_loop);
  }

 private:
  // ── ActionCommand → BusMessage 序列化 ────────────────────────────────────

  static BusMessage EncodeCommand(const ActionCommand& cmd) {
    BusMessage msg;
    msg.topic        = cmd.command_type;
    msg.timestamp_ms = cmd.issued_at_ms;

    // payload = [4B source_entity LE][8B issued_at_ms LE][原始 payload]
    msg.payload.resize(4 + 8 + cmd.payload.size());
    uint8_t* p = msg.payload.data();
    const auto eid = cmd.source_entity;
    p[0] = (eid      ) & 0xFF;
    p[1] = (eid >>  8) & 0xFF;
    p[2] = (eid >> 16) & 0xFF;
    p[3] = (eid >> 24) & 0xFF;
    p += 4;
    const auto ts = cmd.issued_at_ms;
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((ts >> (i * 8)) & 0xFF);
    p += 8;
    if (!cmd.payload.empty()) {
      std::memcpy(p, cmd.payload.data(), cmd.payload.size());
    }
    return msg;
  }

  void ForwardToAdapter(const ActionCommand& cmd) {
    std::shared_ptr<IBusAdapter>      adapter;
    std::shared_ptr<IEventLoopRuntime> loop;
    {
      std::lock_guard<std::mutex> lk(adapter_mu_);
      adapter = adapter_;
      loop    = event_loop_;
    }
    if (!adapter || !loop) return;

    BusMessage msg = EncodeCommand(cmd);
    loop->PostToLoop([adapter, msg = std::move(msg)]() {
      adapter->Publish(msg);
    });
  }

  std::mutex mu_;
  std::unordered_map<std::string, std::vector<CommandHandler>> handlers_;

  std::mutex                         adapter_mu_;
  std::shared_ptr<IBusAdapter>       adapter_;
  std::shared_ptr<IEventLoopRuntime> event_loop_;
};

// ── 工厂函数 ─────────────────────────────────────────────────────────────────

std::shared_ptr<ICommandBus> CreateInProcessCommandBus() {
  return std::make_shared<InProcessCommandBus>();
}

// 返回具体类型，供 SimHostApp 调用 SetBusAdapter()
std::shared_ptr<InProcessCommandBus> CreateInProcessCommandBusImpl() {
  return std::make_shared<InProcessCommandBus>();
}

}  // namespace sim_bt
