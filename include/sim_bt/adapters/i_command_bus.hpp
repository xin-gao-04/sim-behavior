#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sim_bt/common/result.hpp"
#include "sim_bt/common/types.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// ActionCommand
//
// 行为树产出的命令描述符，由 CommandBus 向仿真宿主或下游模型分发。
// payload 使用字节数组保持与具体协议无关。
// ─────────────────────────────────────────────────────────────────────────────
struct ActionCommand {
  EntityId             source_entity = kInvalidEntityId;
  std::string          command_type;     // 例如 "move_to", "engage_target", "cease_fire"
  std::vector<uint8_t> payload;          // 命令参数，由接收方解析
  SimTimeMs            issued_at_ms = 0; // 发出时仿真时间
};

using CommandHandler = std::function<void(const ActionCommand&)>;

// ─────────────────────────────────────────────────────────────────────────────
// ICommandBus
//
// 命令总线（Integration Adapters 出站方向）。
//
// 行为树节点在 tick 中调用 Dispatch()，命令经此总线分发给：
//   - 仿真宿主的实体控制器
//   - 武器系统适配器
//   - 运动控制模型
//   - 总线订阅方（通过 IBusAdapter 发出）
//
// 架构原则：
//   - BT 节点只依赖 ICommandBus，不直接持有仿真对象指针。
//   - 命令分发逻辑在 CommandBus 实现层集中，节点可单元测试。
// ─────────────────────────────────────────────────────────────────────────────
class ICommandBus {
 public:
  virtual ~ICommandBus() = default;

  // 分发命令。通常在 BT Tick Domain 中调用（同步入队，异步分发可选）。
  virtual SimStatus Dispatch(ActionCommand command) = 0;

  // 注册命令类型的处理器（用于集成测试和内部路由）。
  virtual void RegisterHandler(const std::string& command_type,
                                CommandHandler handler) = 0;

  // 清除所有处理器（测试用）。
  virtual void ClearHandlers() = 0;
};

using CommandBusPtr = std::shared_ptr<ICommandBus>;

}  // namespace sim_bt
