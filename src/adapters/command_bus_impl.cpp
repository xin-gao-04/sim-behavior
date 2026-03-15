#include "sim_bt/adapters/i_command_bus.hpp"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// InProcessCommandBus
//
// 进程内同步命令总线，用于开发调试版（单进程部署）。
// 联调集成版中替换为通过 IBusAdapter 走网络/IPC 分发的实现。
// ─────────────────────────────────────────────────────────────────────────────
class InProcessCommandBus : public ICommandBus {
 public:
  SimStatus Dispatch(ActionCommand command) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = handlers_.find(command.command_type);
    if (it == handlers_.end()) {
      // 无处理器时不报错，允许未注册的命令静默丢弃（可配置）
      return SimStatus::Ok();
    }
    // 在持锁状态下同步调用（保持简单；生产版应解锁后调用）
    for (auto& h : it->second) {
      h(command);
    }
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

 private:
  std::mutex mu_;
  std::unordered_map<std::string, std::vector<CommandHandler>> handlers_;
};

// 工厂函数供外部构造
std::shared_ptr<ICommandBus> CreateInProcessCommandBus() {
  return std::make_shared<InProcessCommandBus>();
}

}  // namespace sim_bt
