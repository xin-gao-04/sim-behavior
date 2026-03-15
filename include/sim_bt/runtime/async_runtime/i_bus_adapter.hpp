#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sim_bt/common/result.hpp"

namespace sim_bt {

// ─────────────────────────────────────────────────────────────────────────────
// BusMessage
//
// 仿真总线消息的通用表示，payload 使用字节数组保持传输层无关性。
// 具体协议解析由各 adapter 实现负责。
// ─────────────────────────────────────────────────────────────────────────────
struct BusMessage {
  std::string          topic;
  std::vector<uint8_t> payload;
  uint64_t             timestamp_ms = 0;  // 消息时间戳（仿真时间）
};

using BusMessageCallback = std::function<void(const BusMessage&)>;

// ─────────────────────────────────────────────────────────────────────────────
// IBusAdapter
//
// 仿真总线接入层接口（Integration Adapters 第六层）。
//
// 实现通常基于 uvw TCP/UDP/Pipe handle，在 EventLoopRuntime 的 loop
// 中注册，事件到达后回调在 loop 线程上执行，保证串行性。
//
// 具体实现示例：
//   - TcpBusAdapter  — TCP 连接到仿真宿主消息总线
//   - UdpBusAdapter  — UDP 组播接收态势数据
//   - PipeBusAdapter — 进程间通信（同机部署）
// ─────────────────────────────────────────────────────────────────────────────
class IBusAdapter {
 public:
  virtual ~IBusAdapter() = default;

  // 适配器名称（用于日志和调试）。
  virtual const char* Name() const = 0;

  // 连接到总线。由 EventLoopRuntime 在 loop 线程上调用。
  virtual SimStatus Connect() = 0;

  // 断开连接。由 EventLoopRuntime 在 loop 线程上调用。
  virtual void Disconnect() = 0;

  // 是否已连接。
  virtual bool IsConnected() const = 0;

  // 订阅特定 topic，消息到达时在 loop 线程上调用 callback。
  virtual SimStatus Subscribe(const std::string& topic,
                              BusMessageCallback callback) = 0;

  // 取消订阅。
  virtual void Unsubscribe(const std::string& topic) = 0;

  // 向总线发布消息（在 loop 线程上调用）。
  virtual SimStatus Publish(const BusMessage& message) = 0;
};

using BusAdapterPtr = std::shared_ptr<IBusAdapter>;

}  // namespace sim_bt
